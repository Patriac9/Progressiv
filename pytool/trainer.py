import json
import sys
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

horizon = 10  # Position lasting time (seconds)
TAKER_FEE = 0  # flip / horizon / eod 吃单
MAKER_FEE = 0  # 建仓 / TP 挂单
QTY = 2  # 1 张标的
TP_OFFSET = 0.09  # 止盈/止损距离：开仓价 ± offset
ALPHA_TP_SCALE = 1.0  # 动态 TP/SL = clip(scale * alpha_hat)
ALPHA_CLIP_Q = 0.80  # 动态上限用训练集 alpha 分位，避免 vol_exp 撑到数个点
ALPHA_CLIP_MULT = 6.0  # 动态上限不超过 TP_OFFSET 的倍数
MAX_ALIGN = timedelta(milliseconds=250)

ROOT = Path(__file__).resolve().parent.parent
DATA_ROOTS = [
    Path(__file__).resolve().parent / "training_data",
    ROOT / "cmake-build-debug" / "training_data",
    ROOT / "training_data",
]
INST_CANDIDATES = [
    ROOT / "cmake-build-debug" / "instId.cfg",
    ROOT / "instId.cfg",
]


def parse_timestamp(ts: str) -> datetime:
    main, _, frac = ts.partition(".")
    dt = datetime.strptime(main, "%Y%m%d%H%M%S")
    millis = int((frac + "000")[:3]) if frac else 0
    return dt + timedelta(milliseconds=millis)


def parse_inst_id(path: Path) -> tuple[str, str]:
    symbols = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        symbols.append(line.upper())
        if len(symbols) == 2:
            break
    if not symbols:
        raise RuntimeError(f"no symbol in {path}")
    if len(symbols) == 1:
        return symbols[0], symbols[0]
    return symbols[0], symbols[1]


def load_inst_pair() -> tuple[str, str, Path]:
    last_err = None
    for path in INST_CANDIDATES:
        if not path.is_file():
            continue
        try:
            signal_sym, trade_sym = parse_inst_id(path)
            return signal_sym, trade_sym, path
        except RuntimeError as exc:
            last_err = exc
    if last_err:
        raise last_err
    raise RuntimeError("instId.cfg not found")


def find_symbol_dir(symbol: str) -> Path:
    for root in DATA_ROOTS:
        folder = root / symbol
        if folder.is_dir() and any(folder.glob("*.jsonl")):
            return folder
    return DATA_ROOTS[0] / symbol


def load_day_records(folder: Path) -> dict[str, list]:
    days = {}
    for data_path in sorted(folder.glob("*.jsonl")):
        records = []
        with data_path.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
        days[data_path.stem] = records
    return days


signal_sym, trade_sym, inst_path = load_inst_pair()
split_exec = signal_sym != trade_sym
signal_dir = find_symbol_dir(signal_sym)
exec_dir = find_symbol_dir(trade_sym)

signal_days = load_day_records(signal_dir)
if not signal_days:
    raise RuntimeError(f"no jsonl files in {signal_dir}")
exec_days = load_day_records(exec_dir) if split_exec else {}

day_keys = sorted(signal_days)
total_records = [signal_days[day] for day in day_keys]


def label_machine(horizon: int):
    horizon_delta = timedelta(seconds=horizon)
    for file in total_records:
        times = [parse_timestamp(data["timestamp"]) for data in file]
        n = len(file)
        j = 0
        for i, data in enumerate(file):
            target = times[i] + horizon_delta
            if j < i + 1:
                j = i + 1
            while j < n and times[j] < target:
                j += 1
            if j >= n or times[j] - target > timedelta(seconds=2):
                data["label"] = None
                data["alpha"] = None
                continue
            mid0 = float(data["mid_price"])
            data["label"] = float(file[j]["mid_price"]) - mid0
            peak = 0.0
            for k in range(i, j + 1):
                peak = max(peak, abs(float(file[k]["mid_price"]) - mid0))
            data["alpha"] = peak


label_machine(horizon)

VOL_KEYS = ("n_mid_moves_30s", "n_mid_moves_60s", "rv_30s", "rv_60s")
MID_EPS = 1e-8


def attach_vol_features(file):
    n = len(file)
    if n == 0:
        return
    times = [parse_timestamp(r["timestamp"]) for r in file]
    mids = np.array([float(r["mid_price"]) for r in file], dtype=float)
    for window_s, n_key, rv_key in (
        (30, "n_mid_moves_30s", "rv_30s"),
        (60, "n_mid_moves_60s", "rv_60s"),
    ):
        window = timedelta(seconds=window_s)
        left = 0
        for i in range(n):
            t = times[i]
            while left < i and t - times[left] > window:
                left += 1
            if t - times[0] < window or i <= left:
                file[i][n_key] = 0.0
                file[i][rv_key] = 0.0
                continue
            sumsq = 0.0
            moves = 0
            for k in range(left, i):
                d = float(mids[k + 1] - mids[k])
                sumsq += d * d
                if abs(d) > MID_EPS:
                    moves += 1
            file[i][n_key] = float(moves)
            file[i][rv_key] = float(np.sqrt(sumsq))
  

for file in total_records:
    attach_vol_features(file)

FEATURE_KEYS = [
    "obi",
    "ml_ofi_5s",
    "ml_ofi_15s",
    "ml_ofi_30s",
    "ml_ofi_60s",
]
WARMUP = timedelta(seconds=60)

present_keys = set()
for file in total_records:
    for rec in file:
        present_keys.update(rec.keys())
missing = [k for k in FEATURE_KEYS if k not in present_keys]
FEATURE_KEYS = [k for k in FEATURE_KEYS if k in present_keys]
if not FEATURE_KEYS:
    raise RuntimeError("no usable features in loaded jsonl")


def finite_number(value):
    try:
        x = float(value)
    except (TypeError, ValueError):
        return None
    return x if np.isfinite(x) else None


def is_clean_sample(data) -> bool:
    y = finite_number(data.get("label"))
    alpha = finite_number(data.get("alpha"))
    mid = finite_number(data.get("mid_price"))
    if y is None or alpha is None or alpha < 0 or mid is None or mid <= 0:
        return False
    if any(finite_number(data.get(k)) is None for k in VOL_KEYS):
        return False
    return all(finite_number(data.get(k)) is not None for k in FEATURE_KEYS)


def tick_size_of(symbol_dir: str) -> float:
    name = symbol_dir.upper()
    if "BTC" in name:
        return 0.1
    if "ETH" in name:
        return 0.01
    return 0.01


samples = []
sample_fi = []
sample_ri = []
for fi, file in enumerate(total_records):
    if not file:
        continue
    t0 = parse_timestamp(file[0]["timestamp"])
    for ri, data in enumerate(file):
        if parse_timestamp(data["timestamp"]) - t0 >= WARMUP and is_clean_sample(data):
            samples.append(data)
            sample_fi.append(fi)
            sample_ri.append(ri)

if not samples:
    raise RuntimeError("no training samples after cleaning")

X = np.array([[float(row[k]) for k in FEATURE_KEYS] for row in samples], dtype=float)
y = np.array([float(row["label"]) for row in samples], dtype=float)
alpha = np.array([float(row["alpha"]) for row in samples], dtype=float)

usable = np.std(X, axis=0) > 1e-12
active_features = [k for k, ok in zip(FEATURE_KEYS, usable) if ok]
X = X[:, usable]
X_design = np.column_stack([np.ones(len(X)), X])

split = int(len(y) * 0.7)
X_train, X_test = X_design[:split], X_design[split:]
y_train, y_test = y[:split], y[split:]
alpha_train, alpha_test = alpha[:split], alpha[split:]

coef, *_ = np.linalg.lstsq(X_train, y_train, rcond=None)
intercept = float(coef[0])
weights = dict(zip(active_features, coef[1:]))

T_all = X_design @ coef
y_hat_train = T_all[:split]
y_hat_test = T_all[split:]


def sign_hit(y_hat, y_arr, min_move=1e-8):
    mask = np.abs(y_arr) > min_move
    if int(np.count_nonzero(mask)) == 0:
        return float("nan")
    return float(np.mean(np.sign(y_hat[mask]) == np.sign(y_arr[mask])))


def corr_or_nan(a, b):
    if len(a) < 2 or float(np.std(a)) < 1e-18 or float(np.std(b)) < 1e-18:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


train_mse = float(np.mean((y_hat_train - y_train) ** 2))
test_mse = float(np.mean((y_hat_test - y_test) ** 2))
train_r2 = 1.0 - train_mse / float(np.var(y_train))
test_r2 = 1.0 - test_mse / float(np.var(y_test))


def poly_design(x, degree):
    return np.column_stack([x ** k for k in range(degree + 1)])


def linear_design(z):
    return np.column_stack([np.ones(len(z)), z])


def quadratic_design(z):
    cols = [np.ones(len(z))]
    for j in range(z.shape[1]):
        cols.append(z[:, j])
        cols.append(z[:, j] ** 2)
    return np.column_stack(cols)


abs_t_train = np.abs(y_hat_train)
abs_t_test = np.abs(y_hat_test)
abs_t_all = np.abs(T_all)

Z_all = np.array([[float(row[k]) for k in VOL_KEYS] for row in samples], dtype=float)
vol_ok = np.std(Z_all, axis=0) > 1e-12
vol_names = [k for k, ok in zip(VOL_KEYS, vol_ok) if ok]
Z_all = Z_all[:, vol_ok]
Z_train, Z_test = Z_all[:split], Z_all[split:]

alpha_models = []


def eval_alpha_model(name, design_tr, design_te, design_all, target_tr, kind="linear", features=None):
    coef, *_ = np.linalg.lstsq(design_tr, target_tr, rcond=None)
    if kind == "exp":
        hat_tr = np.exp(design_tr @ coef)
        hat_te = np.exp(design_te @ coef)
        hat_all = np.exp(design_all @ coef)
    else:
        hat_tr = design_tr @ coef
        hat_te = design_te @ coef
        hat_all = design_all @ coef
    hat_tr = np.clip(hat_tr, 0.0, None)
    hat_te = np.clip(hat_te, 0.0, None)
    hat_all = np.clip(hat_all, 0.0, None)
    tr_mse = float(np.mean((hat_tr - alpha_train) ** 2))
    te_mse = float(np.mean((hat_te - alpha_test) ** 2))
    tr_var = float(np.var(alpha_train))
    te_var = float(np.var(alpha_test))
    tr_r2 = 1.0 - tr_mse / tr_var if tr_var > 0 else float("nan")
    te_r2 = 1.0 - te_mse / te_var if te_var > 0 else float("nan")
    model = {
        "name": name,
        "kind": kind,
        "features": list(features or []),
        "coef": coef,
        "hat_tr": hat_tr,
        "hat_te": hat_te,
        "hat_all": hat_all,
        "train_r2": tr_r2,
        "test_r2": te_r2,
        "test_mse": te_mse,
        "train_corr": corr_or_nan(hat_tr, alpha_train),
        "test_corr": corr_or_nan(hat_te, alpha_test),
    }
    alpha_models.append(model)
    return model


def quadratic_features(names):
    out = []
    for name in names:
        out.append(name)
        out.append(f"{name}^2")
    return out


if vol_names:
    eval_alpha_model(
        "vol_linear", linear_design(Z_train), linear_design(Z_test), linear_design(Z_all),
        alpha_train, features=vol_names,
    )
    eval_alpha_model(
        "vol_quadratic", quadratic_design(Z_train), quadratic_design(Z_test), quadratic_design(Z_all),
        alpha_train, features=quadratic_features(vol_names),
    )
    log_tr = np.log(np.clip(alpha_train, 1e-8, None))
    eval_alpha_model(
        "vol_exp", linear_design(Z_train), linear_design(Z_test), linear_design(Z_all),
        log_tr, kind="exp", features=vol_names,
    )
    if "rv_60s" in vol_names:
        j = vol_names.index("rv_60s")
        z1_tr = Z_train[:, j:j + 1]
        z1_te = Z_test[:, j:j + 1]
        z1_all = Z_all[:, j:j + 1]
        eval_alpha_model(
            "rv60_linear", linear_design(z1_tr), linear_design(z1_te), linear_design(z1_all),
            alpha_train, features=["rv_60s"],
        )
    z_t_tr = np.column_stack([Z_train, abs_t_train])
    z_t_te = np.column_stack([Z_test, abs_t_test])
    z_t_all = np.column_stack([Z_all, abs_t_all])
    eval_alpha_model(
        "vol+|T|_linear", linear_design(z_t_tr), linear_design(z_t_te), linear_design(z_t_all),
        alpha_train, features=[*vol_names, "abs_T"],
    )

for deg in (1, 2, 3):
    poly_names = ["abs_T"] + [f"abs_T^{k}" for k in range(2, deg + 1)]
    eval_alpha_model(
        f"|T|_poly{deg}",
        poly_design(abs_t_train, deg),
        poly_design(abs_t_test, deg),
        poly_design(abs_t_all, deg),
        alpha_train,
        features=poly_names,
    )

if not alpha_models:
    raise RuntimeError("no alpha models fitted")
best_alpha_model = min(alpha_models, key=lambda m: m["test_mse"])
alpha_hat_all = best_alpha_model["hat_all"]

signal_tick = tick_size_of(signal_sym)
exec_tick = tick_size_of(trade_sym)
horizon_delta = timedelta(seconds=horizon)


def exec_quote(rec, default_tick: float):
    bid = finite_number(rec.get("bid"))
    ask = finite_number(rec.get("ask"))
    mid = finite_number(rec.get("mid_price"))
    tick = finite_number(rec.get("tick_size")) or default_tick
    if bid is not None and ask is not None and bid > 0 and ask > 0:
        return 0.5 * (bid + ask), 0.5 * (ask - bid)
    if mid is None or mid <= 0:
        return None, None
    st = finite_number(rec.get("spread_ticks"))
    ticks = 1.0 if st is None or st <= 0 else st
    return mid, 0.5 * ticks * tick


def align_exec_to_signal(signal_file, exec_file, default_tick: float):
    n = len(signal_file)
    mids = np.full(n, np.nan, dtype=float)
    halfs = np.full(n, np.nan, dtype=float)
    if not exec_file:
        return mids, halfs, 0

    by_signal_ts = {}
    exec_times = []
    exec_mids = []
    exec_halfs = []
    for rec in exec_file:
        mid, half = exec_quote(rec, default_tick)
        if mid is None or half is None:
            continue
        sig_ts = rec.get("signal_timestamp") or rec.get("timestamp")
        if isinstance(sig_ts, str) and sig_ts:
            by_signal_ts[sig_ts] = (mid, half)
        try:
            exec_times.append(parse_timestamp(rec["timestamp"]))
        except (KeyError, ValueError, TypeError):
            continue
        exec_mids.append(mid)
        exec_halfs.append(half)

    order = np.argsort(exec_times) if exec_times else np.array([], dtype=int)
    sorted_times = [exec_times[i] for i in order]
    sorted_mids = [exec_mids[i] for i in order]
    sorted_halfs = [exec_halfs[i] for i in order]
    matched = 0
    for i, rec in enumerate(signal_file):
        ts = rec.get("timestamp")
        if ts in by_signal_ts:
            mids[i], halfs[i] = by_signal_ts[ts]
            matched += 1
            continue
        if not sorted_times:
            continue
        t = parse_timestamp(ts)
        idx = int(np.searchsorted(sorted_times, t, side="right")) - 1
        if idx < 0:
            continue
        if t - sorted_times[idx] <= MAX_ALIGN:
            mids[i] = sorted_mids[idx]
            halfs[i] = sorted_halfs[idx]
            matched += 1
    return mids, halfs, matched


file_times = []
file_exec_mids = []
file_exec_halfs = []
file_T = []
file_alpha_hat = []
aligned_n = 0
signal_n = 0
for fi, file in enumerate(total_records):
    n = len(file)
    signal_n += n
    times = [parse_timestamp(r["timestamp"]) if n else None for r in file]
    signal_mids = np.array(
        [float(r["mid_price"]) if r.get("mid_price") is not None else np.nan for r in file],
        dtype=float,
    )
    if split_exec:
        exec_mids, exec_halfs, n_match = align_exec_to_signal(
            file, exec_days.get(day_keys[fi], []), exec_tick
        )
        aligned_n += n_match
    else:
        exec_mids = signal_mids.copy()
        exec_halfs = np.empty(n, dtype=float)
        for i, r in enumerate(file):
            st = finite_number(r.get("spread_ticks"))
            ticks = 1.0 if st is None or st <= 0 else st
            exec_halfs[i] = 0.5 * ticks * signal_tick
        aligned_n += n
    file_times.append(times)
    file_exec_mids.append(exec_mids)
    file_exec_halfs.append(exec_halfs)
    file_T.append(np.full(n, np.nan, dtype=float))
    file_alpha_hat.append(np.full(n, np.nan, dtype=float))

for k, (fi, ri) in enumerate(zip(sample_fi, sample_ri)):
    file_T[fi][ri] = T_all[k]
    file_alpha_hat[fi][ri] = alpha_hat_all[k]

align_pct = (aligned_n / signal_n * 100.0) if signal_n else 0.0
if split_exec and aligned_n == 0:
    for fi, file in enumerate(total_records):
        n = len(file)
        mids = np.array(
            [float(r["mid_price"]) if r.get("mid_price") is not None else np.nan for r in file],
            dtype=float,
        )
        halfs = np.empty(n, dtype=float)
        for i, r in enumerate(file):
            st = finite_number(r.get("spread_ticks"))
            ticks = 1.0 if st is None or st <= 0 else st
            halfs[i] = 0.5 * ticks * signal_tick
        file_exec_mids[fi] = mids
        file_exec_halfs[fi] = halfs


def taker_fill(mid, half_spread, side_buy: bool) -> float:
    # 买打卖一，卖打买一
    return mid + half_spread if side_buy else mid - half_spread


def maker_fill(mid, half_spread, side_buy: bool) -> float:
    # 买挂买一，卖挂卖一
    return mid - half_spread if side_buy else mid + half_spread


def trade_pnl(entry_px, exit_px, direction, entry_fee_rate, exit_fee_rate, qty=QTY):
    gross = direction * (exit_px - entry_px) * qty
    fee = (entry_fee_rate * entry_px + exit_fee_rate * exit_px) * qty
    return gross - fee, fee


def backtest(tau, tp_mode="static"):
    train_pnl = 0.0
    test_pnl = 0.0
    train_n = 0
    test_n = 0
    train_fee = 0.0
    test_fee = 0.0
    exits = {"tp": 0, "sl": 0, "horizon": 0, "flip": 0, "eod": 0}
    train_exits = {"tp": 0, "sl": 0, "horizon": 0, "flip": 0, "eod": 0}
    test_exits = {"tp": 0, "sl": 0, "horizon": 0, "flip": 0, "eod": 0}
    wins = 0
    train_wins = 0
    test_wins = 0
    tp_used = []

    for fi, file in enumerate(total_records):
        times = file_times[fi]
        mids = file_exec_mids[fi]
        halfs = file_exec_halfs[fi]
        Ts = file_T[fi]
        tps = file_alpha_hat[fi]
        n = len(file)
        pos = None

        def close_at(ri, reason):
            nonlocal pos, train_pnl, test_pnl, train_n, test_n, train_fee, test_fee, wins
            nonlocal train_wins, test_wins
            mid = mids[ri]
            if not np.isfinite(mid) or mid <= 0:
                pos = None
                return
            exit_buy = pos["direction"] < 0
            if reason == "tp":
                exit_px = pos["entry_px"] + pos["direction"] * pos["tp_offset"]
                exit_fee_rate = MAKER_FEE
            elif reason == "sl":
                exit_px = pos["entry_px"] - pos["direction"] * pos["tp_offset"]
                exit_fee_rate = MAKER_FEE
            else:
                exit_px = taker_fill(mid, halfs[ri], exit_buy)
                exit_fee_rate = TAKER_FEE
            pnl, fee = trade_pnl(
                pos["entry_px"], exit_px, pos["direction"], MAKER_FEE, exit_fee_rate
            )
            if pos["is_train"]:
                train_pnl += pnl
                train_fee += fee
                train_n += 1
                train_exits[reason] += 1
                if pnl > 0:
                    train_wins += 1
            else:
                test_pnl += pnl
                test_fee += fee
                test_n += 1
                test_exits[reason] += 1
                if pnl > 0:
                    test_wins += 1
            if pnl > 0:
                wins += 1
            exits[reason] += 1
            pos = None

        for ri in range(n):
            mid = mids[ri]
            now = times[ri]
            if pos is not None and np.isfinite(mid) and mid > 0:
                t_now = Ts[ri]
                bid = mid - halfs[ri]
                ask = mid + halfs[ri]
                tp_px = pos["entry_px"] + pos["direction"] * pos["tp_offset"]
                sl_px = pos["entry_px"] - pos["direction"] * pos["tp_offset"]
                if pos["direction"] > 0:
                    hit_tp = bid >= tp_px
                    hit_sl = ask <= sl_px
                else:
                    hit_tp = ask <= tp_px
                    hit_sl = bid >= sl_px
                hit_horizon = now >= pos["deadline"]
                hit_flip = np.isfinite(t_now) and t_now * pos["direction"] < 0
                if hit_tp:
                    close_at(ri, "tp")
                elif hit_sl:
                    close_at(ri, "sl")
                elif hit_horizon:
                    close_at(ri, "horizon")
                elif hit_flip:
                    close_at(ri, "flip")

            if pos is not None:
                continue
            t_now = Ts[ri]
            if not np.isfinite(mid) or mid <= 0 or not np.isfinite(t_now) or t_now == 0:
                continue
            if abs(t_now) < tau:
                continue
            if tp_mode == "dynamic":
                raw_tp = tps[ri]
                if not np.isfinite(raw_tp):
                    continue
                tp_off = float(np.clip(raw_tp * ALPHA_TP_SCALE, tp_min, tp_max))
            else:
                tp_off = TP_OFFSET
            direction = 1.0 if t_now > 0 else -1.0
            half = halfs[ri]
            if not np.isfinite(half):
                continue
            entry_px = maker_fill(mid, half, direction > 0)
            tp_used.append(tp_off)
            pos = {
                "direction": direction,
                "entry_mid": mid,
                "entry_px": entry_px,
                "tp_offset": tp_off,
                "deadline": now + horizon_delta,
                "is_train": (fi, ri) in train_keys,
            }

        if pos is not None:
            close_at(n - 1, "eod")

    return {
        "mode": tp_mode,
        "tau": tau,
        "train_pnl": train_pnl,
        "test_pnl": test_pnl,
        "train_n": train_n,
        "test_n": test_n,
        "train_fee": train_fee,
        "test_fee": test_fee,
        "exits": exits,
        "train_exits": train_exits,
        "test_exits": test_exits,
        "wins": wins,
        "train_wins": train_wins,
        "test_wins": test_wins,
        "trades": train_n + test_n,
        "median_tp": float(np.median(tp_used)) if tp_used else float("nan"),
    }


train_keys = set()
for k in range(split):
    train_keys.add((sample_fi[k], sample_ri[k]))

tp_min = max(exec_tick * 2.0, 0.05)
q_cap = float(np.quantile(alpha_train, ALPHA_CLIP_Q))
tp_max = max(TP_OFFSET, min(q_cap, TP_OFFSET * ALPHA_CLIP_MULT))
tau_grid = np.unique(np.quantile(abs_t_train, np.linspace(0.0, 0.99, 40)))

mode_results = {}
for tp_mode in ("static", "dynamic"):
    mode_results[tp_mode] = [backtest(float(tau), tp_mode=tp_mode) for tau in tau_grid]


def median_hit(hat, actual):
    med = float(np.median(actual))
    if not np.isfinite(med):
        return float("nan")
    return float(np.mean((hat >= med) == (actual >= med)))


def t_quintile_hits(t_hat, y_arr):
    abs_t = np.abs(t_hat)
    edges = np.unique(np.quantile(abs_t, np.linspace(0.0, 1.0, 6)))
    rows = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        if i == len(edges) - 2:
            mask = (abs_t >= lo) & (abs_t <= hi)
        else:
            mask = (abs_t >= lo) & (abs_t < hi)
        if not np.any(mask):
            continue
        rows.append((i + 1, int(np.sum(mask)), float(np.median(abs_t[mask])), sign_hit(t_hat[mask], y_arr[mask])))
    return rows


def wr(wins, n):
    return wins / n if n else float("nan")


feat_tr = X_train[:, 1:]
feat_te = X_test[:, 1:]
factor_rows = []
for j, name in enumerate(active_features):
    factor_rows.append((
        name,
        corr_or_nan(feat_tr[:, j], y_train),
        corr_or_nan(feat_te[:, j], y_test),
        corr_or_nan(feat_tr[:, j], y_hat_train),
        corr_or_nan(feat_te[:, j], y_hat_test),
        corr_or_nan(feat_tr[:, j], alpha_train),
        corr_or_nan(feat_te[:, j], alpha_test),
    ))
for j, name in enumerate(vol_names):
    factor_rows.append((
        name,
        corr_or_nan(Z_train[:, j], y_train),
        corr_or_nan(Z_test[:, j], y_test),
        corr_or_nan(Z_train[:, j], y_hat_train),
        corr_or_nan(Z_test[:, j], y_hat_test),
        corr_or_nan(Z_train[:, j], alpha_train),
        corr_or_nan(Z_test[:, j], alpha_test),
    ))
factor_rows.append((
    "|T|",
    corr_or_nan(abs_t_train, y_train),
    corr_or_nan(abs_t_test, y_test),
    corr_or_nan(abs_t_train, y_hat_train),
    corr_or_nan(abs_t_test, y_hat_test),
    corr_or_nan(abs_t_train, alpha_train),
    corr_or_nan(abs_t_test, alpha_test),
))

result_dir = Path(__file__).resolve().parent / "result"
result_dir.mkdir(parents=True, exist_ok=True)
result_path = result_dir / datetime.now().strftime("%Y%m%d%H%M%S")
result_file = result_path.open("w", encoding="utf-8")


class _Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, data):
        for stream in self.streams:
            stream.write(data)
            stream.flush()

    def flush(self):
        for stream in self.streams:
            stream.flush()


_orig_stdout = sys.stdout
sys.stdout = _Tee(_orig_stdout, result_file)
try:
    print("========== 训练 ==========")
    print(f"signal={signal_sym}  exec={trade_sym}  n={len(y)}  train={split}  test={len(y) - split}")
    print(f"align={aligned_n}/{signal_n} ({align_pct:.1f}%)  alpha_model={best_alpha_model['name']}")
    print(f"horizon={horizon}  TP_OFFSET={TP_OFFSET}  SL=opposite  maker={MAKER_FEE}  taker={TAKER_FEE}  qty={QTY}")
    print(f"dynamic TP/SL clip=[{tp_min:.4g}, {tp_max:.4g}]  scale={ALPHA_TP_SCALE}  q={ALPHA_CLIP_Q}")
    print(f"T intercept={intercept:.6g}")
    print(f"{'factor':<22} {'weight':>12}")
    print(f"{'intercept':<22} {intercept:12.6g}")
    for name, w in weights.items():
        print(f"{name:<22} {w:12.6g}")

    print("\n========== 因子相关 ==========")
    print(f"{'factor':<22} {'corr_y_tr':>10} {'corr_y_te':>10} {'corr_T_tr':>10} {'corr_T_te':>10} "
          f"{'corr_a_tr':>10} {'corr_a_te':>10}")
    for row in factor_rows:
        print(f"{row[0]:<22} {row[1]:10.4f} {row[2]:10.4f} {row[3]:10.4f} {row[4]:10.4f} "
              f"{row[5]:10.4f} {row[6]:10.4f}")
    print(f"{'T vs y':<22} {corr_or_nan(y_hat_train, y_train):10.4f} {corr_or_nan(y_hat_test, y_test):10.4f}")
    print(f"{'|T| vs alpha':<22} {corr_or_nan(abs_t_train, alpha_train):10.4f} {corr_or_nan(abs_t_test, alpha_test):10.4f}")
    print(f"{'T vs alpha':<22} {corr_or_nan(y_hat_train, alpha_train):10.4f} {corr_or_nan(y_hat_test, alpha_test):10.4f}")
    print(f"T r2 train/test: {train_r2:.6g} / {test_r2:.6g}")

    print("\n========== T / alpha 命中率 ==========")
    print(f"T  sign(T)==sign(y)     train={sign_hit(y_hat_train, y_train):.4f}  test={sign_hit(y_hat_test, y_test):.4f}")
    print(f"alpha  高于中位数一致  train={median_hit(best_alpha_model['hat_tr'], alpha_train):.4f}  "
          f"test={median_hit(best_alpha_model['hat_te'], alpha_test):.4f}")
    print(f"{'bin':<6} {'n_tr':>7} {'|T|med_tr':>10} {'T_hit_tr':>10} {'n_te':>7} {'|T|med_te':>10} {'T_hit_te':>10}")
    tr_q = t_quintile_hits(y_hat_train, y_train)
    te_q = t_quintile_hits(y_hat_test, y_test)
    for a, b in zip(tr_q, te_q):
        print(f"{a[0]:<6} {a[1]:7d} {a[2]:10.5f} {a[3]:10.4f} {b[1]:7d} {b[2]:10.5f} {b[3]:10.4f}")

    print("\n========== alpha 模型 ==========")
    print(f"{'model':<20} {'train_r2':>10} {'test_r2':>10} {'train_corr':>10} {'test_corr':>10}")
    for model in alpha_models:
        mark = "*" if model["name"] == best_alpha_model["name"] else " "
        print(f"{mark}{model['name']:<19} {model['train_r2']:10.6g} {model['test_r2']:10.6g} "
              f"{model['train_corr']:10.4f} {model['test_corr']:10.4f}")

    def print_tau_table(title, rows):
        print(f"\n========== {title} ==========")
        print(f"{'tau':>12} {'n_tr':>7} {'pnl_tr':>12} {'wr_tr':>8} {'tp/n_tr':>8} {'sl/n_tr':>8} "
              f"{'n_te':>7} {'pnl_te':>12} {'wr_te':>8} {'tp/n_te':>8} {'sl/n_te':>8} "
              f"{'med_tp':>8} {'tp':>6} {'sl':>6} {'hor':>5} {'flip':>6} {'eod':>5}")
        for s in rows:
            e = s["exits"]
            print(f"{s['tau']:12.6g} {s['train_n']:7d} {s['train_pnl']:12.6g} {wr(s['train_wins'], s['train_n']):8.4f} "
                  f"{wr(s['train_exits']['tp'], s['train_n']):8.4f} {wr(s['train_exits']['sl'], s['train_n']):8.4f} "
                  f"{s['test_n']:7d} {s['test_pnl']:12.6g} {wr(s['test_wins'], s['test_n']):8.4f} "
                  f"{wr(s['test_exits']['tp'], s['test_n']):8.4f} {wr(s['test_exits']['sl'], s['test_n']):8.4f} "
                  f"{s['median_tp']:8.4g} {e['tp']:6d} {e['sl']:6d} {e['horizon']:5d} {e['flip']:6d} {e['eod']:5d}")

    print_tau_table(f"tau static  TP/SL=entry±{TP_OFFSET}", mode_results["static"])
    print_tau_table(f"tau dynamic  TP/SL=entry±alpha_hat[{best_alpha_model['name']}]", mode_results["dynamic"])
finally:
    sys.stdout = _orig_stdout
    result_file.close()


def minmax_norm(vals):
    arr = np.asarray(vals, dtype=float)
    lo = float(np.nanmin(arr))
    hi = float(np.nanmax(arr))
    if not np.isfinite(lo) or not np.isfinite(hi) or hi - lo < 1e-12:
        return np.zeros(len(arr), dtype=float)
    return (arr - lo) / (hi - lo)


def select_tau(rows, min_test_n=30):
    eligible = [s for s in rows if s["test_n"] >= min_test_n]
    if not eligible:
        eligible = [s for s in rows if s["test_n"] > 0]
    if not eligible:
        return None
    pnls = np.array([s["test_pnl"] for s in eligible], dtype=float)
    wrs = np.array([s["test_wins"] / s["test_n"] for s in eligible], dtype=float)
    tprs = np.array([s["test_exits"]["tp"] / s["test_n"] for s in eligible], dtype=float)
    sp, sw, st = minmax_norm(pnls), minmax_norm(wrs), minmax_norm(tprs)
    scores = sp + sw + st
    i = int(np.argmax(scores))
    best = eligible[i]
    return {
        "tau": float(best["tau"]),
        "test_n": int(best["test_n"]),
        "pnl_te": float(best["test_pnl"]),
        "wr_te": float(best["test_wins"] / best["test_n"]),
        "tpr_te": float(best["test_exits"]["tp"] / best["test_n"]),
        "score": float(scores[i]),
        "score_pnl": float(sp[i]),
        "score_wr": float(sw[i]),
        "score_tpr": float(st[i]),
    }


# 实盘 enable_dynamic_risk_management=true，用 dynamic 表选 tau
tau_choice = select_tau(mode_results["dynamic"])
if tau_choice is None:
    tau_choice = select_tau(mode_results["static"])
if tau_choice is None:
    raise RuntimeError("no tau candidate with test trades")

print(
    "selected tau={tau:.6g}  (dynamic, minmax pnl+wr+tpr)  "
    "n_te={test_n} pnl_te={pnl_te:.6g} wr_te={wr_te:.4f} tpr_te={tpr_te:.4f} "
    "score={score:.4f} (pnl={score_pnl:.3f} wr={score_wr:.3f} tpr={score_tpr:.3f})".format(**tau_choice)
)


def fmt_param(value):
    if isinstance(value, (float, np.floating)):
        return f"{float(value):.16g}"
    return str(value)


def write_param_file(path, items):
    with path.open("w", encoding="utf-8") as f:
        for key, value in items:
            f.write(f"{key} = {fmt_param(value)}\n")


models_dir = Path(__file__).resolve().parent / "models"
models_dir.mkdir(parents=True, exist_ok=True)
t_path = models_dir / "T_Param"
alpha_path = models_dir / "alpha_Param"

write_param_file(t_path, [
    ("intercept", intercept),
    *weights.items(),
    ("tau", tau_choice["tau"]),
    ("tau_mode", "dynamic"),
    ("tau_n_te", tau_choice["test_n"]),
    ("tau_pnl_te", tau_choice["pnl_te"]),
    ("tau_wr_te", tau_choice["wr_te"]),
    ("tau_tpr_te", tau_choice["tpr_te"]),
    ("tau_score", tau_choice["score"]),
])

alpha_m = best_alpha_model
alpha_items = [
    ("model", alpha_m["name"]),
    ("kind", alpha_m["kind"]),
    ("intercept", float(alpha_m["coef"][0])),
]
for name, value in zip(alpha_m["features"], alpha_m["coef"][1:]):
    alpha_items.append((name, float(value)))
alpha_items.append(("scale", ALPHA_TP_SCALE))
alpha_items.append(("clip_min", tp_min))
alpha_items.append(("clip_max", tp_max))
alpha_items.append(("sl", "opposite"))
write_param_file(alpha_path, alpha_items)

print(f"wrote {result_path}")
print(f"wrote {t_path}")
print(f"wrote {alpha_path}")
