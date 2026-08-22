"""PDF 第一版：15s / 相对 mid ±0.1% 双障碍首次触碰 + 两阶段 CatBoost/LightGBM。

hit  : P(min(τ+,τ-) ≤ H | X)
dir  : P(τ+ < τ- | 发生触碰, X)
p+   = p_hit * q_up
p-   = p_hit * (1 - q_up)

数据沿用 trainer 管线：instId.cfg、按日 jsonl、信号/成交对齐、60s warmup。
新 capture 的 ±0.1% 深度 / 耗尽 / 空洞 / refill / Hawkes 若存在则自动加入
（jsonl 键名仍是 *_10bp，与 10bp=0.1% 同义）；旧 ETH jsonl 缺少这些键时降级。
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

HORIZON_S = 6.0
THETA = 0.0006  # 相对 mid ±0.1%（=10bp）
WARMUP = timedelta(seconds=60)
HORIZON_SLACK = timedelta(seconds=2)
GRID_MS = 100  # PDF 因果网格
PURGE_S = 15.0
MAX_ALIGN = timedelta(milliseconds=250)
TARGET_PPV = 0.85
TRAIN_FRAC = 0.55
CALIB_FRAC = 0.15
RANDOM_SEED = 42

ROOT = Path(__file__).resolve().parent.parent
PYTOOL = Path(__file__).resolve().parent
DATA_ROOTS = [
    PYTOOL / "training_data",
    ROOT / "cmake-build-debug" / "training_data",
    ROOT / "training_data",
]
INST_CANDIDATES = [
    ROOT / "cmake-build-debug" / "instId.cfg",
    ROOT / "instId.cfg",
]

SKIP_KEYS = {
    "timestamp",
    "transaction_time",
    "tick",
    "mid_price",
    "T",
    "alpha",
    "label",
    "signal_timestamp",
}

# PDF：hit 学波动/深度/空洞/事件速度；dir 学 OFI/失衡/微观价格/跨市场
HIT_CANDIDATES = [
    "rv_30s",
    "rv_60s",
    "n_mid_moves_30s",
    "n_mid_moves_60s",
    "d_ask_10bp",
    "d_bid_10bp",
    "cover_ask_10bp",
    "cover_bid_10bp",
    "r_ask",
    "r_bid",
    "t_clear_ask",
    "t_clear_bid",
    "t_clear_ask_h",
    "t_clear_bid_h",
    "d_ask_chg_1s",
    "d_bid_chg_1s",
    "gap_ask",
    "gap_bid",
    "gap_max_ask",
    "gap_max_bid",
    "refill_ask_50ms",
    "refill_ask_100ms",
    "refill_ask_250ms",
    "refill_bid_50ms",
    "refill_bid_100ms",
    "refill_bid_250ms",
    "hawkes_buy_mo",
    "hawkes_sell_mo",
    "hawkes_ask_add",
    "hawkes_bid_add",
    "hawkes_ask_cancel",
    "hawkes_bid_cancel",
    "spread_ticks",
    "spread_frac",
]
DIR_CANDIDATES = [
    "obi",
    "ml_ofi_250ms",
    "ml_ofi_1s",
    "ml_ofi_2s",
    "ml_ofi_5s",
    "ml_ofi_15s",
    "ml_ofi_30s",
    "ml_ofi_60s",
    "ofi_tick",
    "l1_ofi_tick",
    "ret_100ms",
    "ret_250ms",
    "ret_1s",
    "ret_5s",
    "aggressor_imb_250ms",
    "aggressor_imb_1s",
    "aggressor_imb_5s",
    "aggressor_net_1s",
    "last_trade_sign",
    "last_trade_mid_bps",
    "last_trade_age_ms",
    "d_imb_10bp",
    "gap_imb",
    "slope_imb",
    "bid_slope",
    "ask_slope",
    "l1_imb",
    "d5_imb",
    "d10_imb",
    "micro_off",
    "sig_l1_imb",
    "sig_l3_imb",
    "sig_micro_off",
    "sig_spread_ticks",
    "hawkes_buy_mo",
    "hawkes_sell_mo",
    "hawkes_ask_cancel",
    "hawkes_bid_cancel",
    "hawkes_ask_add",
    "hawkes_bid_add",
    "hawkes_mo_imb",
    "hawkes_add_imb",
    "hawkes_cancel_imb",
    "r_imb",
    "d_chg_imb_1s",
    "refill_imb_100ms",
    "cover_imb_10bp",
    "exec_aggressor_imb_250ms",
    "exec_aggressor_imb_1s",
    "basis",
    "basis_chg_1s",
]


def parse_timestamp(ts: str) -> datetime:
    main, _, frac = ts.partition(".")
    dt = datetime.strptime(main, "%Y%m%d%H%M%S")
    millis = int((frac + "000")[:3]) if frac else 0
    return dt + timedelta(milliseconds=millis)


def finite_number(value):
    try:
        x = float(value)
    except (TypeError, ValueError):
        return None
    return x if np.isfinite(x) else None


def imb(a, b):
    if a is None or b is None:
        return np.nan
    s = a + b
    if not np.isfinite(s) or abs(s) < 1e-12:
        return np.nan
    return (a - b) / s


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
            return *parse_inst_id(path), path
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
        # DeepLOB 单独文件：YYYYMMDD.lob.jsonl，不要当因子日文件
        if data_path.name.endswith(".lob.jsonl"):
            continue
        if not data_path.stem.isdigit():
            continue
        records = []
        with data_path.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
        days[data_path.stem] = records
    return days


def label_first_touch(mids: np.ndarray, ts_sec: np.ndarray, horizon_s: float, theta: float):
    """Y ∈ {+1, −1, 0}：H 内谁先碰到 mid*(1±θ)。同 tick 双边都触则剔除。"""
    n = len(mids)
    y = np.zeros(n, dtype=np.int8)
    valid = np.zeros(n, dtype=bool)
    j = 0
    slack = HORIZON_SLACK.total_seconds()
    for i in range(n):
        p0 = mids[i]
        if not np.isfinite(p0) or p0 <= 0:
            continue
        target = ts_sec[i] + horizon_s
        if j < i + 1:
            j = i + 1
        while j < n and ts_sec[j] < target:
            j += 1
        if j >= n or (ts_sec[j] - target) > slack:
            continue
        up = p0 * (1.0 + theta)
        dn = p0 * (1.0 - theta)
        bar = 0
        ambiguous = False
        for k in range(i, j + 1):
            px = mids[k]
            if not np.isfinite(px):
                continue
            hit_up = px >= up
            hit_dn = px <= dn
            if hit_up and hit_dn:
                ambiguous = True
                break
            if hit_up:
                bar = 1
                break
            if hit_dn:
                bar = -1
                break
        if ambiguous:
            continue
        y[i] = bar
        valid[i] = True
    return y, valid


def grid_keep(times: list[datetime], valid: np.ndarray, step_ms: int, t0: datetime) -> np.ndarray:
    keep = np.zeros(len(times), dtype=bool)
    last_bucket = None
    for i, t in enumerate(times):
        if not valid[i] or t - t0 < WARMUP:
            continue
        bucket = int((t - t0).total_seconds() * 1000.0) // max(step_ms, 1)
        if last_bucket is None or bucket != last_bucket:
            keep[i] = True
            last_bucket = bucket
    return keep


def exec_extra_row(rec: dict) -> dict[str, float]:
    bid = finite_number(rec.get("bid"))
    ask = finite_number(rec.get("ask"))
    mid = finite_number(rec.get("mid_price"))
    bq = finite_number(rec.get("bid_qty"))
    aq = finite_number(rec.get("ask_qty"))
    b5 = finite_number(rec.get("bid_qty_5"))
    a5 = finite_number(rec.get("ask_qty_5"))
    b10 = finite_number(rec.get("bid_qty_10"))
    a10 = finite_number(rec.get("ask_qty_10"))
    st = finite_number(rec.get("spread_ticks"))
    micro = np.nan
    if bid and ask and bq and aq and (bq + aq) > 0 and mid:
        micro = (ask * bq + bid * aq) / (bq + aq) - mid
    spread_frac = np.nan
    if bid and ask and mid and mid > 0:
        spread_frac = (ask - bid) / mid
    return {
        "l1_imb": imb(bq, aq),
        "d5_imb": imb(b5, a5),
        "d10_imb": imb(b10, a10),
        "micro_off": micro,
        "spread_ticks": st if st is not None else np.nan,
        "spread_frac": spread_frac,
    }


def align_exec_book(signal_file, exec_file):
    n = len(signal_file)
    mids = np.full(n, np.nan)
    extra = {k: np.full(n, np.nan) for k in (
        "l1_imb", "d5_imb", "d10_imb", "micro_off", "spread_ticks", "spread_frac",
    )}
    if not exec_file:
        return mids, extra
    by_ts = {}
    exec_times = []
    exec_mids = []
    exec_extra = []
    for rec in exec_file:
        bid = finite_number(rec.get("bid"))
        ask = finite_number(rec.get("ask"))
        mid = finite_number(rec.get("mid_price"))
        if bid and ask and bid > 0 and ask > 0:
            px = 0.5 * (bid + ask)
        elif mid and mid > 0:
            px = mid
        else:
            continue
        row = exec_extra_row(rec)
        sig_ts = rec.get("signal_timestamp") or rec.get("timestamp")
        if isinstance(sig_ts, str) and sig_ts:
            by_ts[sig_ts] = (px, row)
        try:
            exec_times.append(parse_timestamp(rec["timestamp"]))
        except (KeyError, ValueError, TypeError):
            continue
        exec_mids.append(px)
        exec_extra.append(row)
    order = np.argsort(exec_times) if exec_times else np.array([], dtype=int)
    sorted_times = [exec_times[i] for i in order]
    sorted_mids = [exec_mids[i] for i in order]
    sorted_extra = [exec_extra[i] for i in order]
    for i, rec in enumerate(signal_file):
        ts = rec.get("timestamp")
        hit = by_ts.get(ts)
        if hit is None and sorted_times:
            t = parse_timestamp(ts)
            idx = int(np.searchsorted(sorted_times, t, side="right")) - 1
            if idx >= 0 and t - sorted_times[idx] <= MAX_ALIGN:
                hit = (sorted_mids[idx], sorted_extra[idx])
        if hit is None:
            continue
        mids[i] = hit[0]
        for k, v in hit[1].items():
            extra[k][i] = v
    return mids, extra


def resolve_backend(name: str) -> str:
    name = (name or "auto").lower()
    if name == "catboost":
        import catboost  # noqa: F401
        return "catboost"
    if name in ("lightgbm", "lgbm", "lightbgm"):
        import lightgbm  # noqa: F401
        return "lightgbm"
    try:
        import catboost  # noqa: F401
        return "catboost"
    except ImportError:
        pass
    try:
        import lightgbm  # noqa: F401
        return "lightgbm"
    except ImportError:
        pass
    raise RuntimeError(
        "需要 catboost 或 lightgbm，例如: pip install catboost lightgbm scikit-learn"
    )


def fit_binary(Xtr, ytr, Xva, yva, backend: str, name: str):
    ytr = np.asarray(ytr, dtype=np.int32)
    yva = np.asarray(yva, dtype=np.int32)
    if backend == "catboost":
        from catboost import CatBoostClassifier
        model = CatBoostClassifier(
            loss_function="Logloss",
            eval_metric="Logloss",
            iterations=500,
            depth=6,
            learning_rate=0.05,
            l2_leaf_reg=4.0,
            random_seed=RANDOM_SEED,
            auto_class_weights="Balanced",
            od_type="Iter",
            od_wait=40,
            verbose=False,
            allow_writing_files=False,
        )
        fit_kw = {"X": Xtr, "y": ytr}
        if len(yva) >= 50:
            fit_kw["eval_set"] = (Xva, yva)
        model.fit(**fit_kw)
        return model
    import lightgbm as lgb
    pos = max(int(np.sum(ytr == 1)), 1)
    neg = max(int(np.sum(ytr == 0)), 1)
    model = lgb.LGBMClassifier(
        objective="binary",
        n_estimators=500,
        max_depth=6,
        learning_rate=0.05,
        subsample=0.8,
        colsample_bytree=0.8,
        reg_lambda=4.0,
        random_state=RANDOM_SEED,
        scale_pos_weight=neg / pos,
        verbosity=-1,
    )
    kw = {}
    if len(yva) >= 50:
        kw["eval_set"] = [(Xva, yva)]
        try:
            kw["callbacks"] = [lgb.early_stopping(40, verbose=False)]
        except Exception:
            pass
    model.fit(Xtr, ytr, **kw)
    return model


def predict_proba(model, X, backend: str) -> np.ndarray:
    p = model.predict_proba(X)
    if p.ndim == 2 and p.shape[1] >= 2:
        return p[:, 1].astype(float)
    return np.asarray(p, dtype=float).reshape(-1)


def feature_importance(model, names, backend: str) -> list[tuple[str, float]]:
    if backend == "catboost":
        raw = np.asarray(model.get_feature_importance(), dtype=float)
    else:
        raw = np.asarray(model.feature_importances_, dtype=float)
    if raw.size != len(names):
        return []
    order = np.argsort(-raw)
    return [(names[i], float(raw[i])) for i in order]


class IsotonicCalibrator:
    def __init__(self):
        self.iso = None
        self.fallback = None

    def fit(self, p: np.ndarray, y: np.ndarray):
        p = np.clip(np.asarray(p, dtype=float), 1e-6, 1.0 - 1e-6)
        y = np.asarray(y, dtype=float)
        m = np.isfinite(p) & np.isfinite(y)
        p, y = p[m], y[m]
        if len(p) < 80 or y.max() == y.min():
            self.fallback = float(np.mean(y)) if len(y) else 0.5
            return self
        try:
            from sklearn.isotonic import IsotonicRegression
            self.iso = IsotonicRegression(y_min=0.0, y_max=1.0, out_of_bounds="clip")
            self.iso.fit(p, y)
        except ImportError:
            self.fallback = float(np.mean(y))
        return self

    def transform(self, p: np.ndarray) -> np.ndarray:
        p = np.clip(np.asarray(p, dtype=float), 1e-6, 1.0 - 1e-6)
        if self.iso is not None:
            return np.clip(self.iso.predict(p), 1e-6, 1.0 - 1e-6)
        if self.fallback is not None:
            return np.full(len(p), self.fallback)
        return p


def logloss(y, p):
    p = np.clip(np.asarray(p, dtype=float), 1e-6, 1.0 - 1e-6)
    y = np.asarray(y, dtype=float)
    return float(-np.mean(y * np.log(p) + (1.0 - y) * np.log(1.0 - p)))


def brier(y, p):
    return float(np.mean((np.asarray(p) - np.asarray(y)) ** 2))


def auc(score, y):
    s = np.asarray(score, dtype=float)
    z = np.asarray(y, dtype=bool)
    m = np.isfinite(s)
    s, z = s[m], z[m]
    n1 = int(np.count_nonzero(z))
    n0 = int(z.size - n1)
    if n1 < 8 or n0 < 8:
        return float("nan")
    order = np.argsort(s, kind="mergesort")
    ranks = np.empty(len(s), dtype=float)
    ranks[order] = np.arange(1, len(s) + 1, dtype=float)
    return float((ranks[z].sum() - n1 * (n1 + 1) / 2.0) / (n1 * n0))


def nonoverlap_mask(times: np.ndarray, fire: np.ndarray, horizon_s: float) -> np.ndarray:
    """一个信号发出后，H 结束前不重复计数。"""
    out = np.zeros(len(fire), dtype=bool)
    next_ok = times[0] if len(times) else 0.0
    for i in range(len(fire)):
        if not fire[i]:
            continue
        if times[i] < next_ok:
            continue
        out[i] = True
        next_ok = times[i] + horizon_s
    return out


def ppv_coverage(p, y_pos, times, c, horizon_s):
    fire = p >= c
    use = nonoverlap_mask(times, fire, horizon_s)
    n = int(np.count_nonzero(use))
    if n == 0:
        return n, float("nan"), 0.0
    hits = int(np.count_nonzero(y_pos[use]))
    return n, hits / n, n / max(len(p), 1)


def choose_threshold(p, y_pos, times, target_ppv, horizon_s):
    cand = np.unique(np.quantile(p[np.isfinite(p)], np.linspace(0.5, 0.995, 40)))
    best = None
    rows = []
    for c in cand:
        n, ppv, cov = ppv_coverage(p, y_pos, times, float(c), horizon_s)
        rows.append((float(c), n, ppv, cov))
        if n < 20 or not np.isfinite(ppv):
            continue
        if ppv + 1e-12 >= target_ppv:
            if best is None or cov > best[3] or (cov == best[3] and n > best[1]):
                best = (float(c), n, ppv, cov)
    if best is None:
        # 达不到目标 PPV 时取校准集最高 PPV、覆盖尚可的点
        feasible = [r for r in rows if r[1] >= 20 and np.isfinite(r[2])]
        if feasible:
            best = max(feasible, key=lambda r: (r[2], r[3]))
    return best, rows


class Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, data):
        for s in self.streams:
            s.write(data)
            s.flush()

    def flush(self):
        for s in self.streams:
            s.flush()


def time_masks(ts_sec: np.ndarray, train_frac: float, calib_frac: float, purge_s: float):
    t0, t1 = float(ts_sec[0]), float(ts_sec[-1])
    span = max(t1 - t0, 1.0)
    t_tr = t0 + span * train_frac
    t_ca = t0 + span * (train_frac + calib_frac)
    train = ts_sec < (t_tr - purge_s)
    calib = (ts_sec >= t_tr) & (ts_sec < (t_ca - purge_s))
    test = ts_sec >= t_ca
    return train, calib, test


def collect_dataset(horizon_s: float, theta: float, grid_ms: int):
    print("loading ...", flush=True)
    signal_sym, trade_sym, inst_path = load_inst_pair()
    split_exec = signal_sym != trade_sym
    signal_days = load_day_records(find_symbol_dir(signal_sym))
    if not signal_days:
        raise RuntimeError(f"no jsonl in {find_symbol_dir(signal_sym)}")
    exec_days = load_day_records(find_symbol_dir(trade_sym)) if split_exec else {}
    day_keys = sorted(signal_days)
    print(f"signal={signal_sym} exec={trade_sym} days={day_keys} inst={inst_path}", flush=True)

    present = set()
    for day in day_keys:
        for rec in signal_days[day][:80] + signal_days[day][-5:]:
            present.update(rec.keys())
    hit_keys = [k for k in HIT_CANDIDATES if k in present or k in (
        "l1_imb", "d5_imb", "d10_imb", "micro_off", "spread_ticks", "spread_frac",
    )]
    dir_keys = [k for k in DIR_CANDIDATES if k in present or k in (
        "l1_imb", "d5_imb", "d10_imb", "micro_off",
    )]

    rows = []
    print("labeling first-touch ...", flush=True)
    for day in day_keys:
        file = signal_days[day]
        n = len(file)
        if n < 50:
            continue
        times = [parse_timestamp(r["timestamp"]) for r in file]
        ts_sec = np.array([(t - times[0]).total_seconds() for t in times], dtype=float)
        sig_mids = np.array(
            [float(r["mid_price"]) if r.get("mid_price") is not None else np.nan for r in file],
            dtype=float,
        )
        extra = {k: np.full(n, np.nan) for k in (
            "l1_imb", "d5_imb", "d10_imb", "micro_off", "spread_ticks", "spread_frac",
        )}
        if split_exec:
            mids, extra = align_exec_book(file, exec_days.get(day, []))
            if not np.isfinite(mids).any():
                mids = sig_mids
        else:
            mids = sig_mids
        y, valid = label_first_touch(mids, ts_sec, horizon_s, theta)
        keep = grid_keep(times, valid, grid_ms, times[0])
        n_ok = 0
        for i in np.flatnonzero(keep):
            rec = file[i]
            feat = {}
            for k in set(hit_keys + dir_keys):
                if k in extra:
                    feat[k] = float(extra[k][i]) if np.isfinite(extra[k][i]) else np.nan
                else:
                    v = finite_number(rec.get(k))
                    feat[k] = v if v is not None else np.nan
            rows.append({
                "day": day,
                "t": times[i],
                "t_sec": ts_sec[i],
                "y": int(y[i]),
                "mid": float(mids[i]),
                "feat": feat,
            })
            n_ok += 1
        n_hit = int(np.sum((y != 0) & keep))
        print(f"  {day} ticks={n} grid={n_ok} hits={n_hit}", flush=True)

    if len(rows) < 200:
        raise RuntimeError(f"too few labeled grid samples: {len(rows)}")
    return rows, hit_keys, dir_keys, signal_sym, trade_sym, day_keys


def matrix(rows, keys):
    X = np.empty((len(rows), len(keys)), dtype=float)
    for i, row in enumerate(rows):
        for j, k in enumerate(keys):
            X[i, j] = row["feat"].get(k, np.nan)
    keep = [j for j in range(len(keys)) if np.nanstd(X[:, j]) > 1e-12]
    names = [keys[j] for j in keep]
    return X[:, keep], names


def main():
    ap = argparse.ArgumentParser(description="两阶段 CatBoost/LightGBM 盘口双障碍模型")
    ap.add_argument("--horizon", type=float, default=HORIZON_S)
    ap.add_argument("--theta", type=float, default=THETA)
    ap.add_argument("--backend", default="auto", help="auto | catboost | lightgbm")
    ap.add_argument("--target-ppv", type=float, default=TARGET_PPV)
    ap.add_argument("--grid-ms", type=int, default=GRID_MS)
    args = ap.parse_args()

    backend = resolve_backend(args.backend)
    rows, hit_keys, dir_keys, signal_sym, trade_sym, day_keys = collect_dataset(
        args.horizon, args.theta, args.grid_ms,
    )

    y = np.array([r["y"] for r in rows], dtype=np.int8)
    y_hit = (y != 0).astype(np.int32)
    ts_sec = np.array([r["t_sec"] for r in rows], dtype=float)
    # 跨日：把日偏移加进时间轴，避免 split 只看日内秒
    day_index = {d: i for i, d in enumerate(day_keys)}
    day_off = np.array([day_index[r["day"]] * 86_400.0 for r in rows], dtype=float)
    split_t = ts_sec + day_off
    order = np.argsort(split_t, kind="mergesort")
    rows = [rows[i] for i in order]
    y, y_hit, ts_sec, split_t = y[order], y_hit[order], ts_sec[order], split_t[order]

    train_m, calib_m, test_m = time_masks(split_t, TRAIN_FRAC, CALIB_FRAC, PURGE_S)
    if int(np.sum(train_m)) < 100 or int(np.sum(test_m)) < 50:
        n = len(y)
        cut_tr = int(n * TRAIN_FRAC)
        cut_ca = int(n * (TRAIN_FRAC + CALIB_FRAC))
        train_m = np.zeros(n, dtype=bool)
        calib_m = np.zeros(n, dtype=bool)
        test_m = np.zeros(n, dtype=bool)
        train_m[:cut_tr] = True
        calib_m[cut_tr:cut_ca] = True
        test_m[cut_ca:] = True

    X_hit_all, hit_names = matrix(rows, hit_keys)
    X_dir_all, dir_names = matrix(rows, dir_keys)
    if not hit_names or not dir_names:
        raise RuntimeError(f"no usable features hit={hit_names} dir={dir_names}")

    Xh_tr, yh_tr = X_hit_all[train_m], y_hit[train_m]
    Xh_ca, yh_ca = X_hit_all[calib_m], y_hit[calib_m]
    Xh_te, yh_te = X_hit_all[test_m], y_hit[test_m]

    hit_tr = y_hit == 1
    dir_tr = hit_tr & train_m
    dir_ca = hit_tr & calib_m
    dir_te = hit_tr & test_m
    y_up = (y == 1).astype(np.int32)

    result_dir = PYTOOL / "result"
    result_dir.mkdir(parents=True, exist_ok=True)
    result_path = result_dir / (datetime.now().strftime("%Y%m%d%H%M%S") + "_twostage")
    result_file = result_path.open("w", encoding="utf-8")
    orig = sys.stdout
    sys.stdout = Tee(orig, result_file)
    try:
        print("========== 两阶段树模型 ==========")
        print(f"backend={backend}  horizon={args.horizon}s  theta={args.theta} ({args.theta * 1e4:.1f}bp)")
        print(f"signal={signal_sym} exec={trade_sym} grid={args.grid_ms}ms n={len(rows)}")
        print(f"train={int(np.sum(train_m))} calib={int(np.sum(calib_m))} test={int(np.sum(test_m))}")
        print(f"P(hit) train={float(np.mean(yh_tr)):.3f} calib={float(np.mean(yh_ca)):.3f} test={float(np.mean(yh_te)):.3f}")
        print(f"hit features ({len(hit_names)}): {hit_names}")
        print(f"dir features ({len(dir_names)}): {dir_names}")

        print("fitting hit model ...", flush=True)
        hit_model = fit_binary(Xh_tr, yh_tr, Xh_ca, yh_ca, backend, "hit")
        p_hit_ca_raw = predict_proba(hit_model, Xh_ca, backend)
        p_hit_te_raw = predict_proba(hit_model, Xh_te, backend)
        cal_hit = IsotonicCalibrator().fit(p_hit_ca_raw, yh_ca)
        p_hit_ca = cal_hit.transform(p_hit_ca_raw)
        p_hit_te = cal_hit.transform(p_hit_te_raw)

        print("fitting direction model ...", flush=True)
        n_dir_tr = int(np.sum(dir_tr))
        if n_dir_tr < 80:
            raise RuntimeError(f"too few hit samples for direction model: {n_dir_tr}")
        dir_model = fit_binary(
            X_dir_all[dir_tr], y_up[dir_tr],
            X_dir_all[dir_ca] if int(np.sum(dir_ca)) >= 30 else X_dir_all[dir_tr][:1],
            y_up[dir_ca] if int(np.sum(dir_ca)) >= 30 else y_up[dir_tr][:1],
            backend,
            "dir",
        )
        q_ca_raw = predict_proba(dir_model, X_dir_all[calib_m], backend)
        q_te_raw = predict_proba(dir_model, X_dir_all[test_m], backend)
        # 方向校准只用 calib 中真正触碰的样本
        if int(np.sum(dir_ca)) >= 40:
            cal_dir = IsotonicCalibrator().fit(q_ca_raw[y_hit[calib_m] == 1], y_up[dir_ca])
        else:
            cal_dir = IsotonicCalibrator().fit(q_ca_raw, y_up[calib_m])
        q_ca = cal_dir.transform(q_ca_raw)
        q_te = cal_dir.transform(q_te_raw)

        p_up_ca = p_hit_ca * q_ca
        p_dn_ca = p_hit_ca * (1.0 - q_ca)
        p_up_te = p_hit_te * q_te
        p_dn_te = p_hit_te * (1.0 - q_te)
        y_up_ca, y_dn_ca = y_up[calib_m], (y[calib_m] == -1).astype(np.int32)
        y_up_te, y_dn_te = y_up[test_m], (y[test_m] == -1).astype(np.int32)
        t_ca, t_te = split_t[calib_m], split_t[test_m]

        print("\n========== 校准后概率质量 ==========")
        print(f"{'split':<8} {'hit LL':>8} {'hit Brier':>10} {'hit AUC':>8} {'dir AUC':>8}")
        dir_auc_ca = auc(q_ca_raw[y_hit[calib_m] == 1], y_up[dir_ca]) if int(np.sum(dir_ca)) else float("nan")
        dir_auc_te = auc(q_te_raw[y_hit[test_m] == 1], y_up[dir_te]) if int(np.sum(dir_te)) else float("nan")
        print(f"{'calib':<8} {logloss(yh_ca, p_hit_ca):8.4f} {brier(yh_ca, p_hit_ca):10.4f} {auc(p_hit_ca, yh_ca):8.3f} {dir_auc_ca:8.3f}")
        print(f"{'test':<8} {logloss(yh_te, p_hit_te):8.4f} {brier(yh_te, p_hit_te):10.4f} {auc(p_hit_te, yh_te):8.3f} {dir_auc_te:8.3f}")

        up_best, up_rows = choose_threshold(p_up_ca, y_up_ca, t_ca, args.target_ppv, args.horizon)
        dn_best, dn_rows = choose_threshold(p_dn_ca, y_dn_ca, t_ca, args.target_ppv, args.horizon)

        def print_curve(title, rows, best):
            print(f"\n========== {title} ==========")
            print(f"{'c':>8} {'n':>6} {'PPV':>8} {'cover':>8}")
            for c, n, ppv, cov in rows[:: max(len(rows) // 12, 1)]:
                mark = " *" if best and abs(c - best[0]) < 1e-12 else ""
                ppvs = f"{100 * ppv:5.1f}%" if np.isfinite(ppv) else "   nan"
                print(f"{c:8.4f} {n:6d} {ppvs:>8} {100 * cov:7.3f}%{mark}")
            if best:
                print(f"selected c={best[0]:.4f}  n={best[1]}  PPV={100 * best[2]:.1f}%  cover={100 * best[3]:.3f}%")

        print_curve("calib PPV–Coverage 做多 p+", up_rows, up_best)
        print_curve("calib PPV–Coverage 做空 p-", dn_rows, dn_best)

        print("\n========== 测试集（阈值只在 calib 上选一次） ==========")
        for name, best, p, y_pos, t in (
            ("up", up_best, p_up_te, y_up_te, t_te),
            ("dn", dn_best, p_dn_te, y_dn_te, t_te),
        ):
            if best is None:
                print(f"{name}: no threshold")
                continue
            n, ppv, cov = ppv_coverage(p, y_pos, t, best[0], args.horizon)
            print(f"{name} c={best[0]:.4f}  n={n}  PPV={100 * ppv:5.1f}%  cover={100 * cov:.3f}%  base={100 * float(np.mean(y_pos)):.1f}%")

        print("\n========== hit 重要性 ==========")
        for name, w in feature_importance(hit_model, hit_names, backend)[:15]:
            print(f"  {name:<22} {w:10.4g}")
        print("========== dir 重要性 ==========")
        for name, w in feature_importance(dir_model, dir_names, backend)[:15]:
            print(f"  {name:<22} {w:10.4g}")

        models_dir = PYTOOL / "models" / f"two_stage_{backend}"
        models_dir.mkdir(parents=True, exist_ok=True)
        if backend == "catboost":
            hit_path = models_dir / "hit.cbm"
            dir_path = models_dir / "dir.cbm"
            hit_model.save_model(str(hit_path))
            dir_model.save_model(str(dir_path))
        else:
            hit_path = models_dir / "hit.txt"
            dir_path = models_dir / "dir.txt"
            hit_model.booster_.save_model(str(hit_path))
            dir_model.booster_.save_model(str(dir_path))
        meta = {
            "backend": backend,
            "horizon": args.horizon,
            "theta": args.theta,
            "grid_ms": args.grid_ms,
            "target_ppv": args.target_ppv,
            "hit_features": hit_names,
            "dir_features": dir_names,
            "tau_up": None if up_best is None else up_best[0],
            "tau_dn": None if dn_best is None else dn_best[0],
            "signal": signal_sym,
            "exec": trade_sym,
            "days": day_keys,
            "hit_model": str(hit_path.name),
            "dir_model": str(dir_path.name),
        }
        with (models_dir / "meta.json").open("w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        with (models_dir / "bundle.pkl").open("wb") as f:
            pickle.dump(
                {
                    "meta": meta,
                    "hit_model": hit_model,
                    "dir_model": dir_model,
                    "cal_hit": cal_hit,
                    "cal_dir": cal_dir,
                },
                f,
            )
        print(f"\nwrote {models_dir}")
        print(f"wrote {result_path}")
    finally:
        sys.stdout = orig
        result_file.close()


if __name__ == "__main__":
    main()
