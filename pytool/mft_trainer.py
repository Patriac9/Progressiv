"""分钟因子两阶段模型（读 C++ write_factors 的 jsonl）。

因子已在 mft_script 算好，这里不再重算。树模型不是线性公式，而是：

    p_hit = P(min(τ_up, τ_dn) ≤ H | X_hit)     # 触及模型
    q_up  = P(τ_up < τ_dn | 发生触及, X_dir)    # 方向模型（只在触及样本上训）
    p_up  = p_hit * q_up                       # P(H 内先碰到上障)
    p_dn  = p_hit * (1 - q_up)                 # P(H 内先碰到下障)

X_hit 用波动 / 障碍距 / 活跃度 / 时钟（会不会在 H 内碰到 ±θ）。
X_dir 用 T_W、VWAP 偏离、TFI/CVD、基差/OI（碰到的话哪边先）。
CatBoost/LightGBM 拟合的是 logit 上的加性树：f(X) = Σ trees，P = σ(f(X))。

标签（与实盘同一套 g=ℓ=θ、horizon H）：
    入场参考 mid_t；上障 mid*(1+θ)，下障 mid*(1−θ)。
    用之后的 bid（多头可卖出）看上障、ask（空头可买回）看下障；
    若缺盘口则退回 mid。Y=+1 上先，−1 下先，0 都未触。同秒双触丢掉。

返回（infer / 实盘）：
    p_hit, q_up, p_up, p_dn, side ∈ {+1, −1, 0}
    side=+1 若 p_up ≥ τ_up 且 p_up ≥ p_dn
    side=-1 若 p_dn ≥ τ_dn 且 p_dn > p_up
    否则 0（空仓）

用法：
    py -3 pytool/mft_trainer.py
    py -3 pytool/mft_trainer.py --horizon 180 --theta 0.001
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np

HORIZON_S = 60.0
THETA = 0.001  # g = ℓ = 0.1%
WARMUP = timedelta(seconds=300)
HORIZON_SLACK = timedelta(seconds=2)
GRID_MS = 5000
TARGET_PPV = 0.60
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

ID_KEYS = {
    "timestamp",
    "transaction_time",
    "t_unix_ms",
    "tick",
    "symbol",
    "bid",
    "ask",
    "mid",
    "bid_qty",
    "ask_qty",
    "mark",
    "index",
    "oi",
}

# 触及：波动、障碍距、活跃度、冲击、时钟
HIT_CANDIDATES = [
    "rv_15s", "rv_30s", "rv_1m", "rv_2m", "rv_5m", "rv_15m", "rv_30m",
    "sigma_15s", "sigma_30s", "sigma_1m", "sigma_2m", "sigma_5m", "sigma_15m", "sigma_30m",
    "z_tp_60", "z_sl_60", "z_tp_180", "z_sl_180", "z_tp_300", "z_sl_300",
    "rs_plus_1m", "rs_minus_1m", "rs_imb_1m",
    "rs_plus_5m", "rs_minus_5m", "rs_imb_5m",
    "hl_1m", "hl_5m", "hl_15m",
    "vvol_15m", "vvol_30m", "vol_q", "drv",
    "vol_30s", "vol_1m", "vol_5m",
    "n_30s", "n_1m", "n_5m",
    "nps_30s", "nps_1m", "nps_5m", "vol_acc",
    "spread_frac", "amihud_1m", "amihud_5m", "lambda_1m", "lambda_5m",
    "utc_minute", "utc_hour", "is_m5", "is_m15",
    "liq_acc",
]

# 方向：斜率 / VWAP / TFI / CVD / 基差 / OI
DIR_CANDIDATES = [
    "r_5s", "r_15s", "r_30s", "r_1m", "r_2m", "r_5m", "r_10m", "r_30m",
    "t_30s", "t_1m", "t_5m", "t_15m", "dt_1m_5m", "r_acc_30s",
    "er_1m", "er_5m", "er_15m",
    "pos_hl_5m", "pos_hl_15m", "pos_hl_30m",
    "dist_hi_5m_bps", "dist_lo_5m_bps",
    "dist_hi_15m_bps", "dist_lo_15m_bps",
    "dist_hi_30m_bps", "dist_lo_30m_bps",
    "d_vwap_1m", "d_vwap_5m", "d_vwap_30m",
    "clv_1m", "clv_5m",
    "wick_up_1m", "wick_dn_1m", "wick_up_5m", "wick_dn_5m",
    "tfi_5s", "tfi_15s", "tfi_30s", "tfi_1m", "tfi_2m", "tfi_5m", "run_len",
    "beta_cvd_1m", "beta_cvd_5m", "t_cvd_1m", "t_cvd_5m",
    "div_1m", "div_5m", "corr_px_cvd_5m",
    "basis", "basis_z", "d_basis", "funding", "t_to_fund_s",
    "doi_1m", "doi_5m", "px_oi_1m",
    "liq_imb_30s", "liq_imb_5m",
]


def parse_timestamp(ts: str) -> datetime:
    main, _, frac = ts.partition(".")
    dt = datetime.strptime(main, "%Y%m%d%H%M%S")
    millis = int((frac + "000")[:3]) if frac else 0
    return dt + timedelta(milliseconds=millis)


def rec_time(rec: dict) -> datetime | None:
    ms = rec.get("t_unix_ms")
    if ms is not None:
        try:
            return datetime.fromtimestamp(float(ms) / 1000.0, tz=timezone.utc).replace(tzinfo=None)
        except (OSError, OverflowError, ValueError, TypeError):
            pass
    ts = rec.get("timestamp")
    if isinstance(ts, str) and ts not in ("", "-"):
        try:
            return parse_timestamp(ts)
        except (ValueError, TypeError):
            return None
    return None


def finite_number(value):
    try:
        x = float(value)
    except (TypeError, ValueError):
        return None
    return x if np.isfinite(x) else None


def parse_inst_id(path: Path) -> str:
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        return line.upper()
    raise RuntimeError(f"no symbol in {path}")


def load_symbol() -> tuple[str, Path]:
    last_err = None
    for path in INST_CANDIDATES:
        if not path.is_file():
            continue
        try:
            return parse_inst_id(path), path
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
        if data_path.name.endswith(".lob.jsonl"):
            continue
        stem = data_path.stem.split(".")[0]
        if not stem.isdigit():
            continue
        records = []
        with data_path.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
        if records:
            days[stem] = records
    return days


def px_row(rec: dict) -> tuple[float, float, float]:
    bid = finite_number(rec.get("bid"))
    ask = finite_number(rec.get("ask"))
    mid = finite_number(rec.get("mid"))
    if mid is None and bid and ask and bid > 0 and ask > 0:
        mid = 0.5 * (bid + ask)
    return (
        bid if bid and bid > 0 else float("nan"),
        ask if ask and ask > 0 else float("nan"),
        mid if mid and mid > 0 else float("nan"),
    )


def label_first_touch(
    mids: np.ndarray,
    bids: np.ndarray,
    asks: np.ndarray,
    ts_sec: np.ndarray,
    horizon_s: float,
    theta: float,
):
    """Y ∈ {+1, −1, 0}。上障用 bid（多头可成交卖出），下障用 ask（空头可成交买回）。"""
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
            hi = bids[k] if np.isfinite(bids[k]) else mids[k]
            lo = asks[k] if np.isfinite(asks[k]) else mids[k]
            hit_up = np.isfinite(hi) and hi >= up
            hit_dn = np.isfinite(lo) and lo <= dn
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
    raise RuntimeError("需要 catboost 或 lightgbm：pip install catboost lightgbm scikit-learn")


def fit_binary(Xtr, ytr, Xva, yva, backend: str):
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
    print("loading mft jsonl ...", flush=True)
    symbol, inst_path = load_symbol()
    folder = find_symbol_dir(symbol)
    days_map = load_day_records(folder)
    if not days_map:
        raise RuntimeError(
            f"no factor jsonl in {folder}（先跑 Progressiv，"
            f"enable_training_capture=1，文件形如 {symbol}/YYYYMMDD.jsonl）"
        )
    day_keys = sorted(days_map)
    print(f"symbol={symbol} days={day_keys} inst={inst_path} dir={folder}", flush=True)

    sample = days_map[day_keys[0]][0]
    if "t_1m" not in sample and "tfi_1m" not in sample:
        raise RuntimeError(
            f"{folder} 里的 jsonl 不是 mft 因子文件（缺少 t_1m/tfi_1m）。"
            "请用 write_factors 新采的数据，不要用旧的 OFI jsonl。"
        )
    present = set(sample.keys())
    hit_keys = [k for k in HIT_CANDIDATES if k in present]
    dir_keys = [k for k in DIR_CANDIDATES if k in present]
    if not hit_keys or not dir_keys:
        raise RuntimeError(f"no usable mft features hit={hit_keys} dir={dir_keys}")

    rows = []
    print("labeling triple-barrier ...", flush=True)
    for day in day_keys:
        file = days_map[day]
        times = []
        keep_idx = []
        for i, rec in enumerate(file):
            t = rec_time(rec)
            if t is None:
                continue
            times.append(t)
            keep_idx.append(i)
        if len(times) < 50:
            continue
        file = [file[i] for i in keep_idx]
        ts_sec = np.array([(t - times[0]).total_seconds() for t in times], dtype=float)
        bids, asks, mids = [], [], []
        for rec in file:
            b, a, m = px_row(rec)
            bids.append(b)
            asks.append(a)
            mids.append(m)
        bids = np.asarray(bids, dtype=float)
        asks = np.asarray(asks, dtype=float)
        mids = np.asarray(mids, dtype=float)
        y, valid = label_first_touch(mids, bids, asks, ts_sec, horizon_s, theta)
        keep = grid_keep(times, valid, grid_ms, times[0])
        n_ok = 0
        for i in np.flatnonzero(keep):
            rec = file[i]
            feat = {}
            for k in set(hit_keys + dir_keys):
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
        print(f"  {day} ticks={len(file)} grid={n_ok} hits={n_hit}", flush=True)

    if len(rows) < 80:
        raise RuntimeError(f"labeled grid samples too few: {len(rows)}（需要更长 capture 或缩短 --horizon）")
    return rows, hit_keys, dir_keys, symbol, day_keys


def matrix(rows, keys):
    X = np.empty((len(rows), len(keys)), dtype=float)
    for i, row in enumerate(rows):
        for j, k in enumerate(keys):
            X[i, j] = row["feat"].get(k, np.nan)
    keep = [j for j in range(len(keys)) if np.nanstd(X[:, j]) > 1e-12]
    names = [keys[j] for j in keep]
    return X[:, keep], names


def infer_side(p_up: float, p_dn: float, tau_up: float | None, tau_dn: float | None) -> int:
    tau_up = 1.1 if tau_up is None else tau_up
    tau_dn = 1.1 if tau_dn is None else tau_dn
    if p_up >= tau_up and p_up >= p_dn:
        return 1
    if p_dn >= tau_dn and p_dn > p_up:
        return -1
    return 0


def features_from_record(rec: dict, names: list[str]) -> np.ndarray:
    row = np.empty((1, len(names)), dtype=float)
    for j, k in enumerate(names):
        v = finite_number(rec.get(k))
        row[0, j] = v if v is not None else np.nan
    return row


def infer(bundle: dict, rec: dict) -> dict:
    """对一条因子快照打分。返回 p_hit / q_up / p_up / p_dn / side。"""
    backend = bundle["meta"]["backend"]
    x_hit = features_from_record(rec, bundle["meta"]["hit_features"])
    x_dir = features_from_record(rec, bundle["meta"]["dir_features"])
    p_hit = float(bundle["cal_hit"].transform(
        predict_proba(bundle["hit_model"], x_hit, backend)
    )[0])
    q_up = float(bundle["cal_dir"].transform(
        predict_proba(bundle["dir_model"], x_dir, backend)
    )[0])
    p_up = p_hit * q_up
    p_dn = p_hit * (1.0 - q_up)
    side = infer_side(p_up, p_dn, bundle["meta"].get("tau_up"), bundle["meta"].get("tau_dn"))
    return {
        "p_hit": p_hit,
        "q_up": q_up,
        "p_up": p_up,
        "p_dn": p_dn,
        "side": side,
    }


def load_bundle(path: Path | None = None) -> dict:
    path = path or (PYTOOL / "models" / "mft_catboost" / "bundle.pkl")
    with path.open("rb") as f:
        return pickle.load(f)


def main():
    ap = argparse.ArgumentParser(description="分钟因子两阶段 CatBoost/LightGBM")
    ap.add_argument("--horizon", type=float, default=HORIZON_S, help="60 / 180 / 300")
    ap.add_argument("--theta", type=float, default=THETA, help="g=ℓ，默认 0.001")
    ap.add_argument("--backend", default="auto", help="auto | catboost | lightgbm")
    ap.add_argument("--target-ppv", type=float, default=TARGET_PPV)
    ap.add_argument("--grid-ms", type=int, default=GRID_MS)
    args = ap.parse_args()

    backend = resolve_backend(args.backend)
    purge_s = float(args.horizon)
    rows, hit_keys, dir_keys, symbol, day_keys = collect_dataset(
        args.horizon, args.theta, args.grid_ms,
    )

    y = np.array([r["y"] for r in rows], dtype=np.int8)
    y_hit = (y != 0).astype(np.int32)
    ts_sec = np.array([r["t_sec"] for r in rows], dtype=float)
    day_index = {d: i for i, d in enumerate(day_keys)}
    day_off = np.array([day_index[r["day"]] * 86_400.0 for r in rows], dtype=float)
    split_t = ts_sec + day_off
    order = np.argsort(split_t, kind="mergesort")
    rows = [rows[i] for i in order]
    y, y_hit, ts_sec, split_t = y[order], y_hit[order], ts_sec[order], split_t[order]

    train_m, calib_m, test_m = time_masks(split_t, TRAIN_FRAC, CALIB_FRAC, purge_s)
    if int(np.sum(train_m)) < 80 or int(np.sum(test_m)) < 30:
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
    result_path = result_dir / (datetime.now().strftime("%Y%m%d%H%M%S") + "_mft")
    result_file = result_path.open("w", encoding="utf-8")
    orig = sys.stdout
    sys.stdout = Tee(orig, result_file)
    try:
        print("========== 分钟因子两阶段 ==========")
        print("p_hit = P(H 内碰到 ±θ | X_hit)")
        print("q_up  = P(先碰上障 | 碰到, X_dir)")
        print("p_up  = p_hit * q_up     p_dn = p_hit * (1-q_up)")
        print(f"backend={backend}  H={args.horizon}s  θ={args.theta} ({args.theta * 1e4:.1f}bp)")
        print(f"symbol={symbol} grid={args.grid_ms}ms n={len(rows)}")
        print(f"train={int(np.sum(train_m))} calib={int(np.sum(calib_m))} test={int(np.sum(test_m))}")
        print(
            f"P(hit) train={float(np.mean(yh_tr)):.3f} "
            f"calib={float(np.mean(yh_ca)):.3f} test={float(np.mean(yh_te)):.3f}"
        )
        print(f"hit features ({len(hit_names)}): {hit_names}")
        print(f"dir features ({len(dir_names)}): {dir_names}")

        print("fitting hit model ...", flush=True)
        hit_model = fit_binary(Xh_tr, yh_tr, Xh_ca, yh_ca, backend)
        p_hit_ca_raw = predict_proba(hit_model, Xh_ca, backend)
        p_hit_te_raw = predict_proba(hit_model, Xh_te, backend)
        cal_hit = IsotonicCalibrator().fit(p_hit_ca_raw, yh_ca)
        p_hit_ca = cal_hit.transform(p_hit_ca_raw)
        p_hit_te = cal_hit.transform(p_hit_te_raw)

        print("fitting direction model ...", flush=True)
        n_dir_tr = int(np.sum(dir_tr))
        if n_dir_tr < 40:
            raise RuntimeError(f"too few hit samples for direction model: {n_dir_tr}")
        dir_va_x = X_dir_all[dir_ca] if int(np.sum(dir_ca)) >= 20 else X_dir_all[dir_tr][:1]
        dir_va_y = y_up[dir_ca] if int(np.sum(dir_ca)) >= 20 else y_up[dir_tr][:1]
        dir_model = fit_binary(X_dir_all[dir_tr], y_up[dir_tr], dir_va_x, dir_va_y, backend)
        q_ca_raw = predict_proba(dir_model, X_dir_all[calib_m], backend)
        q_te_raw = predict_proba(dir_model, X_dir_all[test_m], backend)
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

        print("\n========== 校准后概率 ==========")
        print(f"{'split':<8} {'hit LL':>8} {'hit Brier':>10} {'hit AUC':>8} {'dir AUC':>8}")
        dir_auc_ca = auc(q_ca_raw[y_hit[calib_m] == 1], y_up[dir_ca]) if int(np.sum(dir_ca)) else float("nan")
        dir_auc_te = auc(q_te_raw[y_hit[test_m] == 1], y_up[dir_te]) if int(np.sum(dir_te)) else float("nan")
        print(
            f"{'calib':<8} {logloss(yh_ca, p_hit_ca):8.4f} {brier(yh_ca, p_hit_ca):10.4f} "
            f"{auc(p_hit_ca, yh_ca):8.3f} {dir_auc_ca:8.3f}"
        )
        print(
            f"{'test':<8} {logloss(yh_te, p_hit_te):8.4f} {brier(yh_te, p_hit_te):10.4f} "
            f"{auc(p_hit_te, yh_te):8.3f} {dir_auc_te:8.3f}"
        )

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
                print(
                    f"selected c={best[0]:.4f}  n={best[1]}  "
                    f"PPV={100 * best[2]:.1f}%  cover={100 * best[3]:.3f}%"
                )

        print_curve("calib PPV 做多 p_up", up_rows, up_best)
        print_curve("calib PPV 做空 p_dn", dn_rows, dn_best)

        print("\n========== 测试集 ==========")
        for name, best, p, y_pos, t in (
            ("up", up_best, p_up_te, y_up_te, t_te),
            ("dn", dn_best, p_dn_te, y_dn_te, t_te),
        ):
            if best is None:
                print(f"{name}: no threshold")
                continue
            n, ppv, cov = ppv_coverage(p, y_pos, t, best[0], args.horizon)
            print(
                f"{name} c={best[0]:.4f}  n={n}  PPV={100 * ppv:5.1f}%  "
                f"cover={100 * cov:.3f}%  base={100 * float(np.mean(y_pos)):.1f}%"
            )

        print("\n========== hit 重要性 ==========")
        for name, w in feature_importance(hit_model, hit_names, backend)[:15]:
            print(f"  {name:<22} {w:10.4g}")
        print("========== dir 重要性 ==========")
        for name, w in feature_importance(dir_model, dir_names, backend)[:15]:
            print(f"  {name:<22} {w:10.4g}")

        tag = "catboost" if backend == "catboost" else "lightgbm"
        models_dir = PYTOOL / "models" / f"mft_{tag}"
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
            "symbol": symbol,
            "days": day_keys,
            "hit_model": str(hit_path.name),
            "dir_model": str(dir_path.name),
            "formula": {
                "p_hit": "P(touch ±theta within H | X_hit)",
                "q_up": "P(up barrier first | touch, X_dir)",
                "p_up": "p_hit * q_up",
                "p_dn": "p_hit * (1 - q_up)",
                "side": "+1 if p_up>=tau_up; -1 if p_dn>=tau_dn; else 0",
            },
        }
        with (models_dir / "meta.json").open("w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        bundle = {
            "meta": meta,
            "hit_model": hit_model,
            "dir_model": dir_model,
            "cal_hit": cal_hit,
            "cal_dir": cal_dir,
        }
        with (models_dir / "bundle.pkl").open("wb") as f:
            pickle.dump(bundle, f)
        print(f"\nwrote {models_dir}")
        print("infer(bundle, rec) -> {p_hit, q_up, p_up, p_dn, side}")
        print(f"wrote {result_path}")
    finally:
        sys.stdout = orig
        result_file.close()


if __name__ == "__main__":
    main()
