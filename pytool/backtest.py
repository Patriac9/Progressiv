"""用两阶段树模型 / TCN-DeepLOB 做三重障碍回测。

开仓：p+≥τ+ 做多，p-≥τ- 做空（同时触发取超出阈值更多的一侧）。
平仓三种：
  tp      同向先碰到 mid*(1±θ)，成交在障碍价
  sl      反向先碰到 mid*(1∓θ)，成交在障碍价
  timeout 到 horizon 仍未碰到，按当时 mid 吃单

手续费（相对成交名义）：
  开仓 = 0.036%
  止盈 = 0
  止损 / 超时 = 0.036%
同 tick 双边都碰到时记为 sl（偏保守）。
同时只持一仓，平仓后才接下一个信号。
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import LightBGM_catBoost as v1

FEE_ENTRY = 0.00036
FEE_TP = 0.0
FEE_OTHER = 0.00036  # 止损、超时
QTY = 1.0
TWOSTAGE_DIR = v1.PYTOOL / "models" / "two_stage_catboost"
LGBM_DIR = v1.PYTOOL / "models" / "two_stage_lightgbm"
TCN_DIR = v1.PYTOOL / "models" / "tcn_deeplob"


def load_tick_paths():
    """按日加载成交 mid 全路径，供障碍扫描。"""
    signal_sym, trade_sym, _ = v1.load_inst_pair()
    split_exec = signal_sym != trade_sym
    signal_days = v1.load_day_records(v1.find_symbol_dir(signal_sym))
    exec_days = v1.load_day_records(v1.find_symbol_dir(trade_sym)) if split_exec else {}
    paths = {}
    for day, file in signal_days.items():
        if len(file) < 50:
            continue
        times = [v1.parse_timestamp(r["timestamp"]) for r in file]
        ts_sec = np.array([(t - times[0]).total_seconds() for t in times], dtype=float)
        sig_mids = np.array(
            [float(r["mid_price"]) if r.get("mid_price") is not None else np.nan for r in file],
            dtype=float,
        )
        if split_exec:
            mids, _ = v1.align_exec_book(file, exec_days.get(day, []))
            if not np.isfinite(mids).any():
                mids = sig_mids
        else:
            mids = sig_mids
        paths[day] = {"times": times, "ts_sec": ts_sec, "mids": mids}
    return paths, signal_sym, trade_sym


def attach_tick_index(rows, paths):
    for row in rows:
        p = paths[row["day"]]
        t = float(row["t_sec"])
        i = int(np.searchsorted(p["ts_sec"], t, side="left"))
        i = min(max(i, 0), len(p["ts_sec"]) - 1)
        if i > 0 and abs(p["ts_sec"][i - 1] - t) <= abs(p["ts_sec"][i] - t):
            i -= 1
        row["tick_i"] = i


def feat_matrix(rows, names):
    X = np.full((len(rows), len(names)), np.nan, dtype=float)
    for i, row in enumerate(rows):
        feat = row["feat"]
        for j, k in enumerate(names):
            v = feat.get(k, np.nan)
            X[i, j] = v if v is not None else np.nan
    return X


def split_masks(rows, day_keys):
    ts_sec = np.array([r["t_sec"] for r in rows], dtype=float)
    day_index = {d: i for i, d in enumerate(day_keys)}
    split_t = ts_sec + np.array([day_index[r["day"]] * 86_400.0 for r in rows], dtype=float)
    order = np.argsort(split_t, kind="mergesort")
    rows = [rows[i] for i in order]
    split_t = split_t[order]
    train_m, calib_m, test_m = v1.time_masks(split_t, v1.TRAIN_FRAC, v1.CALIB_FRAC, v1.PURGE_S)
    if int(np.sum(train_m)) < 100 or int(np.sum(test_m)) < 50:
        n = len(rows)
        cut_tr = int(n * v1.TRAIN_FRAC)
        cut_ca = int(n * (v1.TRAIN_FRAC + v1.CALIB_FRAC))
        train_m = np.zeros(n, dtype=bool)
        calib_m = np.zeros(n, dtype=bool)
        test_m = np.zeros(n, dtype=bool)
        train_m[:cut_tr] = True
        calib_m[cut_tr:cut_ca] = True
        test_m[cut_ca:] = True
    names = np.full(len(rows), "test", dtype=object)
    names[train_m] = "train"
    names[calib_m] = "calib"
    return rows, split_t, names


def resolve_exit(mids, ts_sec, i0, side, theta, horizon_s):
    """side=+1 多 / -1 空。返回 (reason, exit_px, hold_s)。"""
    p0 = mids[i0]
    if not np.isfinite(p0) or p0 <= 0:
        return "timeout", p0, 0.0
    up = p0 * (1.0 + theta)
    dn = p0 * (1.0 - theta)
    tp = up if side > 0 else dn
    sl = dn if side > 0 else up
    target = ts_sec[i0] + horizon_s
    slack = v1.HORIZON_SLACK.total_seconds()
    last_px = p0
    last_t = ts_sec[i0]
    n = len(mids)
    for k in range(i0 + 1, n):
        px = mids[k]
        t = ts_sec[k]
        if not np.isfinite(px):
            continue
        last_px, last_t = px, t
        hit_up = px >= up
        hit_dn = px <= dn
        if hit_up and hit_dn:
            return "sl", sl, max(0.0, t - ts_sec[i0])
        if side > 0:
            if hit_dn:
                return "sl", sl, max(0.0, t - ts_sec[i0])
            if hit_up:
                return "tp", tp, max(0.0, t - ts_sec[i0])
        else:
            if hit_up:
                return "sl", sl, max(0.0, t - ts_sec[i0])
            if hit_dn:
                return "tp", tp, max(0.0, t - ts_sec[i0])
        if t >= target:
            return "timeout", px, max(0.0, t - ts_sec[i0])
        if t > target + slack:
            break
    return "timeout", last_px, max(0.0, last_t - ts_sec[i0])


def trade_pnl(entry_px, exit_px, side, reason, fee_entry, fee_tp, fee_other, qty):
    fee_exit = fee_tp if reason == "tp" else fee_other
    gross = side * (exit_px - entry_px) * qty
    fee = (fee_entry * entry_px + fee_exit * exit_px) * qty
    return gross - fee, fee, gross


def choose_side(p_up, p_dn, tau_up, tau_dn):
    long_ok = tau_up is not None and np.isfinite(p_up) and p_up >= tau_up
    short_ok = tau_dn is not None and np.isfinite(p_dn) and p_dn >= tau_dn
    if long_ok and short_ok:
        return 1 if (p_up - tau_up) >= (p_dn - tau_dn) else -1
    if long_ok:
        return 1
    if short_ok:
        return -1
    return 0


def run_book(p_up, p_dn, tau_up, tau_dn, rows, split_t, split_names, paths,
             theta, horizon_s, fee_entry, fee_tp, fee_other, qty):
    trades = []
    free_at = -1e18
    for i, row in enumerate(rows):
        t = float(split_t[i])
        if t < free_at:
            continue
        side = choose_side(p_up[i], p_dn[i], tau_up, tau_dn)
        if side == 0:
            continue
        path = paths[row["day"]]
        tick_i = int(row["tick_i"])
        entry = float(path["mids"][tick_i])
        if not np.isfinite(entry) or entry <= 0:
            continue
        reason, exit_px, hold_s = resolve_exit(
            path["mids"], path["ts_sec"], tick_i, side, theta, horizon_s,
        )
        if not np.isfinite(exit_px) or exit_px <= 0:
            continue
        net, fee, gross = trade_pnl(
            entry, exit_px, side, reason, fee_entry, fee_tp, fee_other, qty,
        )
        trades.append({
            "i": i,
            "day": row["day"],
            "split": split_names[i],
            "side": int(side),
            "reason": reason,
            "entry": entry,
            "exit": float(exit_px),
            "hold_s": float(hold_s),
            "gross": float(gross),
            "fee": float(fee),
            "net": float(net),
            "ret_bps": 1e4 * float(net) / (entry * qty),
            "p_up": float(p_up[i]),
            "p_dn": float(p_dn[i]),
            "y": int(row.get("y", 0)),
        })
        free_at = t + hold_s
    return trades


def _pct(n, d):
    return 100.0 * n / d if d else float("nan")


def summarize(trades, qty, fee_entry, fee_tp, fee_other, theta, horizon_s, title):
    print(f"\n========== {title} ==========")
    print(
        f"horizon={horizon_s:g}s  theta={100 * theta:.4f}% ({1e4 * theta:.1f}bp)  "
        f"qty={qty:g}  fee_entry={100 * fee_entry:.4f}%  fee_tp={100 * fee_tp:.4f}%  "
        f"fee_sl/to={100 * fee_other:.4f}%"
    )
    print("开仓=mid  TP/SL=mid*(1±θ)  timeout=当时 mid；同时一仓")
    splits = ("train", "calib", "test", "all")
    reasons = ("tp", "sl", "timeout")
    grouped = defaultdict(list)
    for tr in trades:
        grouped[tr["split"]].append(tr)
        grouped["all"].append(tr)

    header = (
        f"{'split':<7} {'n':>6} {'tp':>5} {'sl':>5} {'to':>5} "
        f"{'win%':>7} {'ppv%':>7} {'mean_bps':>9} {'sum_net':>12} {'fee':>10}"
    )
    print(header)
    for sp in splits:
        xs = grouped.get(sp, [])
        n = len(xs)
        if n == 0:
            print(f"{sp:<7} {0:6d}")
            continue
        n_tp = sum(1 for t in xs if t["reason"] == "tp")
        n_sl = sum(1 for t in xs if t["reason"] == "sl")
        n_to = sum(1 for t in xs if t["reason"] == "timeout")
        wins = sum(1 for t in xs if t["net"] > 0)
        # PPV：标签方向与开仓方向一致（先碰到同侧障碍），不是账户胜率
        ppv = sum(1 for t in xs if t["y"] == t["side"])
        mean_bps = float(np.mean([t["ret_bps"] for t in xs]))
        sum_net = float(np.sum([t["net"] for t in xs]))
        fee = float(np.sum([t["fee"] for t in xs]))
        print(
            f"{sp:<7} {n:6d} {n_tp:5d} {n_sl:5d} {n_to:5d} "
            f"{_pct(wins, n):6.1f}% {_pct(ppv, n):6.1f}% "
            f"{mean_bps:9.2f} {sum_net:12.4f} {fee:10.4f}"
        )

    print("\n按退出原因（test；若无测试单则用 all）")
    base = grouped["test"] or grouped["all"]
    print(f"{'reason':<8} {'n':>6} {'share':>7} {'win%':>7} {'mean_bps':>9} {'sum_net':>12}")
    for reason in reasons:
        xs = [t for t in base if t["reason"] == reason]
        n = len(xs)
        if n == 0:
            print(f"{reason:<8} {0:6d}")
            continue
        wins = sum(1 for t in xs if t["net"] > 0)
        print(
            f"{reason:<8} {n:6d} {_pct(n, len(base)):6.1f}% "
            f"{_pct(wins, n):6.1f}% "
            f"{float(np.mean([t['ret_bps'] for t in xs])):9.2f} "
            f"{float(np.sum([t['net'] for t in xs])):12.4f}"
        )

    xs = grouped["test"] or grouped["all"]
    if xs:
        n = len(xs)
        p_tp = sum(1 for t in xs if t["reason"] == "tp") / n
        p_sl = sum(1 for t in xs if t["reason"] == "sl") / n
        p_to = 1.0 - p_tp - p_sl
        to_move = float(np.mean([
            t["side"] * (t["exit"] / t["entry"] - 1.0) for t in xs if t["reason"] == "timeout"
        ])) if p_to > 0 and any(t["reason"] == "timeout" for t in xs) else 0.0
        # 理论：TP 赚 θ、SL 亏 θ；开仓始终扣 entry，出场 TP 免费、SL/超时扣 other
        e_tp = theta - fee_entry - fee_tp
        e_sl = -theta - fee_entry - fee_other
        e_to = to_move - fee_entry - fee_other
        ev = p_tp * e_tp + p_sl * e_sl + p_to * e_to
        print(
            f"\n近似期望（相对名义, test/all）："
            f"  P(tp)={100 * p_tp:.1f}%  P(sl)={100 * p_sl:.1f}%  P(to)={100 * p_to:.1f}%"
        )
        print(
            f"  E[r]≈ {1e4 * ev:.2f} bp/笔   "
            f"(tp贡献 {1e4 * p_tp * e_tp:.2f}  sl {1e4 * p_sl * e_sl:.2f}  "
            f"to {1e4 * p_to * e_to:.2f})"
        )
        print("  口径：TP/SL 按挂单价成交，忽略排队和滑点；timeout 按 mid 吃单。")
        if theta + 1e-12 < fee_entry + fee_tp:
            print(
                f"  注意：θ={1e4 * theta:.1f}bp 小于开仓+TP手续费 "
                f"{1e4 * (fee_entry + fee_tp):.1f}bp，即使次次止盈净利也为负。"
            )


def remap_unpickle(path: Path):
    """训练脚本以 __main__ 跑时，校准器被 pickle 成 __main__.IsotonicCalibrator。"""
    import LightBGM_catBoost as lb

    class _Unpickler(pickle.Unpickler):
        def find_class(self, module, name):
            if name == "IsotonicCalibrator":
                return lb.IsotonicCalibrator
            if module == "__main__":
                return getattr(lb, name)
            return super().find_class(module, name)

    with path.open("rb") as f:
        return _Unpickler(f).load()


def load_twostage_bundle(path: Path):
    bundle_path = path / "bundle.pkl"
    meta_path = path / "meta.json"
    if not bundle_path.is_file():
        return None
    bundle = remap_unpickle(bundle_path)
    meta = bundle.get("meta")
    if meta is None and meta_path.is_file():
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    return {
        "hit": bundle["hit_model"],
        "dir": bundle["dir_model"],
        "cal_hit": bundle["cal_hit"],
        "cal_dir": bundle["cal_dir"],
        "meta": meta or {},
        "dirpath": path,
    }


def score_twostage(bundle, rows):
    meta = bundle["meta"]
    hit_names = list(meta["hit_features"])
    dir_names = list(meta["dir_features"])
    Xh = feat_matrix(rows, hit_names)
    Xd = feat_matrix(rows, dir_names)
    backend = meta.get("backend") or "catboost"
    p_hit_raw = v1.predict_proba(bundle["hit"], Xh, backend)
    q_raw = v1.predict_proba(bundle["dir"], Xd, backend)
    p_hit = bundle["cal_hit"].transform(p_hit_raw)
    q_up = bundle["cal_dir"].transform(q_raw)
    p_up = p_hit * q_up
    p_dn = p_hit * (1.0 - q_up)
    return p_up, p_dn


def backtest_twostage(args, rows, split_t, split_names, paths, day_keys):
    path = Path(args.twostage_dir) if args.twostage_dir else TWOSTAGE_DIR
    if not (path / "bundle.pkl").is_file() and (LGBM_DIR / "bundle.pkl").is_file():
        path = LGBM_DIR
    bundle = load_twostage_bundle(path)
    if bundle is None:
        print(f"跳过两阶段：找不到 {path / 'bundle.pkl'}")
        return []
    meta = bundle["meta"]
    horizon = float(args.horizon if args.horizon is not None else meta.get("horizon", v1.HORIZON_S))
    theta = float(args.theta if args.theta is not None else meta.get("theta", v1.THETA))
    tau_up = meta.get("tau_up") if args.tau_up is None else args.tau_up
    tau_dn = meta.get("tau_dn") if args.tau_dn is None else args.tau_dn
    print(
        f"\n两阶段模型 {bundle['dirpath']}  backend={meta.get('backend')}  "
        f"tau_up={tau_up}  tau_dn={tau_dn}",
        flush=True,
    )
    p_up, p_dn = score_twostage(bundle, rows)
    trades = run_book(
        p_up, p_dn, tau_up, tau_dn, rows, split_t, split_names, paths,
        theta, horizon, args.fee_entry, args.fee_tp, args.fee_other, args.qty,
    )
    summarize(
        trades, args.qty, args.fee_entry, args.fee_tp, args.fee_other, theta, horizon,
        f"回测 两阶段 {meta.get('backend', 'tree')}",
    )
    return trades


def score_tcn(meta, ckpt, calib, days, keys, batch):
    import torch
    import TCN_DeepLob as v2

    arch = ckpt["arch"]
    keys = list(ckpt.get("feat_names") or keys)
    lookback = int(arch["lookback"])
    hazard_ms = int(arch.get("hazard_ms", 250))
    horizon = float(meta.get("horizon", v1.HORIZON_S))
    data = v2.SeqWindows(days, keys, lookback, horizon, hazard_ms, stride=1)
    data.feat_median = np.asarray(ckpt["feat_median"], dtype=np.float32)
    data.feat_iqr = np.asarray(ckpt["feat_iqr"], dtype=np.float32)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = v2.build_model(
        torch, torch.nn,
        int(arch["n_feat"]), lookback, int(arch["n_bins"]),
        bool(arch["use_vol"]), bool(arch["use_ofi"]), int(arch["n_buckets"]),
        int(arch.get("hidden", 64)),
    ).to(device)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    idx = np.arange(len(data.y))
    ups, dns = [], []
    with torch.no_grad():
        for s in range(0, len(idx), batch):
            sl = idx[s:s + batch]
            batch_t = {k: v.to(device) for k, v in data.tensors(sl, torch).items()}
            pu, pd = v2.accumulate_p(model(batch_t))
            ups.append(pu.cpu().numpy())
            dns.append(pd.cpu().numpy())
    p_up = np.concatenate(ups)
    p_dn = np.concatenate(dns)
    if calib is not None:
        if calib.get("cal_up") is not None:
            p_up = calib["cal_up"].transform(p_up)
        if calib.get("cal_dn") is not None:
            p_dn = calib["cal_dn"].transform(p_dn)
    return data, p_up, p_dn


def tcn_rows_from_windows(data, days, day_keys, paths):
    rows = []
    for k in range(len(data.y)):
        di = int(data.day_i[k])
        si = int(data.src_i[k])
        day = day_keys[di]
        d = days[di]
        t_ms = int(np.asarray(d["t_ms"])[si])
        t_sec = t_ms * 1e-3
        mid = float(np.asarray(d["mid"])[si])
        path = paths[day]
        i = int(np.searchsorted(path["ts_sec"], t_sec, side="left"))
        i = min(max(i, 0), len(path["ts_sec"]) - 1)
        if i > 0 and abs(path["ts_sec"][i - 1] - t_sec) <= abs(path["ts_sec"][i] - t_sec):
            i -= 1
        rows.append({
            "day": day,
            "t_sec": t_sec,
            "mid": mid,
            "tick_i": i,
            "y": int(data.y[k]),
        })
    return rows


def backtest_tcn(args, paths, day_keys):
    """先按窗口原始顺序建 rows+概率，再统一时间排序。"""
    path = Path(args.tcn_dir) if args.tcn_dir else TCN_DIR
    pt = path / "model.pt"
    meta_path = path / "meta.json"
    if not pt.is_file() or not meta_path.is_file():
        print(f"跳过 TCN/DeepLOB：找不到 {pt} 或 {meta_path}（需要先跑 TCN_DeepLob.py）")
        return []
    try:
        import torch
        import TCN_DeepLob as v2
    except ImportError as exc:
        print(f"跳过 TCN/DeepLOB：{exc}")
        return []

    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    ckpt = torch.load(pt, map_location="cpu", weights_only=False)
    calib = None
    calib_path = path / "calib.pkl"
    if calib_path.is_file():
        with calib_path.open("rb") as f:
            calib = remap_unpickle(calib_path)
    horizon = float(args.horizon if args.horizon is not None else meta.get("horizon", v1.HORIZON_S))
    theta = float(args.theta if args.theta is not None else meta.get("theta", v1.THETA))
    grid_ms = int(meta.get("grid_ms", v1.GRID_MS))
    print(f"\n加载 TCN {path}  打网格 ...", flush=True)
    days, keys, _, _, loaded_days, _ = v2.load_days(horizon, theta, grid_ms, pack=False)
    day_keys = list(loaded_days)
    data, p_up, p_dn = score_tcn(meta, ckpt, calib, days, keys, args.batch)
    rows = tcn_rows_from_windows(data, days, day_keys, paths)
    for i, row in enumerate(rows):
        row["_p_up"] = float(p_up[i])
        row["_p_dn"] = float(p_dn[i])
    rows, split_t, split_names = split_masks(rows, day_keys)
    p_up = np.array([r["_p_up"] for r in rows], dtype=float)
    p_dn = np.array([r["_p_dn"] for r in rows], dtype=float)
    tau_up = meta.get("tau_up") if args.tau_up is None else args.tau_up
    tau_dn = meta.get("tau_dn") if args.tau_dn is None else args.tau_dn
    print(f"TCN tau_up={tau_up}  tau_dn={tau_dn}  n_win={len(rows)}", flush=True)
    trades = run_book(
        p_up, p_dn, tau_up, tau_dn, rows, split_t, split_names, paths,
        theta, horizon, args.fee_entry, args.fee_tp, args.fee_other, args.qty,
    )
    summarize(
        trades, args.qty, args.fee_entry, args.fee_tp, args.fee_other, theta, horizon,
        "回测 TCN/DeepLOB",
    )
    return trades


def write_trades_csv(path: Path, trades, model_name):
    if not trades:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = [
        "model", "day", "split", "side", "reason", "entry", "exit", "hold_s",
        "gross", "fee", "net", "ret_bps", "p_up", "p_dn", "y",
    ]
    with path.open("w", encoding="utf-8") as f:
        f.write(",".join(cols) + "\n")
        for t in trades:
            rec = dict(t)
            rec["model"] = model_name
            f.write(",".join(str(rec.get(c, "")) for c in cols) + "\n")


def main():
    ap = argparse.ArgumentParser(description="两阶段 / TCN 三重障碍回测")
    ap.add_argument("--model", default="both", choices=("both", "twostage", "tcn"))
    ap.add_argument("--horizon", type=float, default=None, help="默认用各模型 meta")
    ap.add_argument("--theta", type=float, default=None, help="默认用各模型 meta")
    ap.add_argument("--fee-entry", type=float, default=FEE_ENTRY)
    ap.add_argument("--fee-tp", type=float, default=FEE_TP)
    ap.add_argument("--fee-other", type=float, default=FEE_OTHER, help="止损、超时出场费率")
    ap.add_argument("--qty", type=float, default=QTY, help="标的数量，净盈亏=价格差×qty−手续费")
    ap.add_argument("--tau-up", type=float, default=None)
    ap.add_argument("--tau-dn", type=float, default=None)
    ap.add_argument("--twostage-dir", default=None)
    ap.add_argument("--tcn-dir", default=None)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--save-trades", action="store_true")
    args = ap.parse_args()

    result_dir = v1.PYTOOL / "result"
    result_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d%H%M%S")
    result_path = result_dir / (stamp + "_backtest")
    result_file = result_path.open("w", encoding="utf-8")
    orig = sys.stdout
    sys.stdout = v1.Tee(orig, result_file)
    try:
        print("========== 三重障碍回测 ==========")
        print(
            f"fee_entry={100 * args.fee_entry:.4f}%  fee_tp={100 * args.fee_tp:.4f}%  "
            f"fee_sl/to={100 * args.fee_other:.4f}%  qty={args.qty:g}  model={args.model}"
        )
        print("加载 tick 路径 ...", flush=True)
        paths, signal_sym, trade_sym = load_tick_paths()
        print(f"signal={signal_sym} exec={trade_sym} days={sorted(paths)}", flush=True)

        if args.model in ("both", "twostage"):
            horizon = args.horizon if args.horizon is not None else v1.HORIZON_S
            theta = args.theta if args.theta is not None else v1.THETA
            meta_p = (Path(args.twostage_dir) if args.twostage_dir else TWOSTAGE_DIR) / "meta.json"
            if meta_p.is_file():
                m = json.loads(meta_p.read_text(encoding="utf-8"))
                if args.horizon is None:
                    horizon = float(m.get("horizon", horizon))
                if args.theta is None:
                    theta = float(m.get("theta", theta))
            print(f"两阶段打标 horizon={horizon}s theta={theta} ...", flush=True)
            rows, _, _, _, _, day_keys = v1.collect_dataset(horizon, theta, v1.GRID_MS)
            attach_tick_index(rows, paths)
            rows, split_t, split_names = split_masks(rows, day_keys)
            tw_trades = backtest_twostage(args, rows, split_t, split_names, paths, day_keys)
            if args.save_trades:
                write_trades_csv(result_dir / (stamp + "_twostage_trades.csv"), tw_trades, "twostage")
        else:
            day_keys = sorted(paths)

        if args.model in ("both", "tcn"):
            tcn_trades = backtest_tcn(args, paths, day_keys)
            if args.save_trades:
                write_trades_csv(result_dir / (stamp + "_tcn_trades.csv"), tcn_trades, "tcn")

        print(f"\nwrote {result_path}")
    finally:
        sys.stdout = orig
        result_file.close()
    print(f"wrote {result_path}")


if __name__ == "__main__":
    main()
