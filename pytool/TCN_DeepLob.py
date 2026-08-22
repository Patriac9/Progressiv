"""PDF 第二版：因果 TCN / DeepLOB 编码器 + 离散竞争风险头。

与第一版同一标签（15s、相对 mid ±0.1% 首次触碰），但输入是时间序列，输出是
每个未来时间格子上「上触 / 下触 / 本格都不触」的 hazard，再累积成 p+、p-。

训练数据（两种，优先 1）
------------------------
1) 完整序列（真正 DeepLOB），每个交易日一个压缩 npz：

   training_data/<SYMBOL>/YYYYMMDD.seq.npz

   t_ms        int64    (N,)         100ms 网格时间戳（Unix ms 或当日 ms，单调）
   mid         float32  (N,)
   vol         float32  (N, 2, B)    相对 mid 的距离桶深度；0=bid, 1=ask
   ofi         float32  (N, 2, B)    该网格内订单流（成交+挂撤净额）
   feat        float32  (N, F)       与 jsonl 相同的障碍因子
   feat_names  unicode  (F,)
   bucket_bp   float32  (B,)         例如 [1, 2, 5, 10] 即 0.01%/0.02%/0.05%/0.1%
   grid_ms     int32    ()           默认 100

   C++ capture 单独写（WS @depth20，不用 REST）：
     training_data/<SYMBOL>/YYYYMMDD.lob.jsonl
   100ms 一格：用可见 20 档累进 1/2/5/10 bp 桶的 vol/ofi。
   本脚本会把 .lob.jsonl 对齐进网格并打开 DeepLOB CNN。

2) 回退：现有 jsonl（与 LightBGM_catBoost 同一管线）
   抽成规则 100ms 网格 + 手工因子序列，只训 TCN + 因子 MLP + 竞争风险头。
   可先 `python pytool/TCN_DeepLob.py --pack` 写成 .seq.npz（有 lob 则含 vol/ofi）。

训练后导出（pytool/models/tcn_deeplob/）
---------------------------------------
   model.pt     PyTorch：state_dict、结构超参、因子中位数/IQR、桶定义
   meta.json    文本：horizon/theta/网格、特征名、是否启用 DeepLOB、阈值
   calib.pkl    若校准成功：p+ / p- 的 isotonic
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
from datetime import datetime
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import LightBGM_catBoost as v1

LOOKBACK_S = 10.0
HAZARD_MS = 250
EPOCHS = 8
BATCH = 256
LR = 1e-3
STRIDE = 2  # 网格点抽样，减轻重叠
HIDDEN = 64


def label_first_touch_tau(mids: np.ndarray, ts_sec: np.ndarray, horizon_s: float, theta: float):
    """返回 y ∈ {+1,-1,0}、首次触碰秒数 tau、valid。y=0 时 tau=horizon（右删失）。"""
    n = len(mids)
    y = np.zeros(n, dtype=np.int8)
    tau = np.full(n, np.nan, dtype=np.float32)
    valid = np.zeros(n, dtype=bool)
    j = 0
    slack = v1.HORIZON_SLACK.total_seconds()
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
        hit_t = horizon_s
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
            if hit_up or hit_dn:
                bar = 1 if hit_up else -1
                hit_t = max(0.0, float(ts_sec[k] - ts_sec[i]))
                break
        if ambiguous:
            continue
        y[i] = bar
        tau[i] = np.float32(hit_t if bar != 0 else horizon_s)
        valid[i] = True
    return y, tau, valid


def _feat_keys(present: set[str]) -> list[str]:
    extra = {"l1_imb", "d5_imb", "d10_imb", "micro_off", "spread_ticks", "spread_frac"}
    names = []
    for k in list(dict.fromkeys(v1.HIT_CANDIDATES + v1.DIR_CANDIDATES)):
        if k in present or k in extra:
            names.append(k)
    return names


def _row_feat(rec: dict, extra_i: dict[str, np.ndarray], i: int, keys: list[str]) -> np.ndarray:
    out = np.empty(len(keys), dtype=np.float32)
    for j, k in enumerate(keys):
        if k in extra_i:
            v = extra_i[k][i]
            out[j] = np.float32(v) if np.isfinite(v) else np.float32("nan")
        else:
            x = v1.finite_number(rec.get(k))
            out[j] = np.float32(x) if x is not None else np.float32("nan")
    return out


def pack_jsonl_day(file: list, exec_file: list | None, keys: list[str],
                   grid_ms: int, horizon_s: float, theta: float) -> dict:
    """把一天 jsonl 抽成规则网格。vol/ofi 不在 jsonl 里，这里不写。"""
    n = len(file)
    times = [v1.parse_timestamp(r["timestamp"]) for r in file]
    t0 = times[0]
    ts_sec = np.array([(t - t0).total_seconds() for t in times], dtype=np.float64)
    sig_mids = np.array(
        [float(r["mid_price"]) if r.get("mid_price") is not None else np.nan for r in file],
        dtype=np.float64,
    )
    extra = {k: np.full(n, np.nan) for k in (
        "l1_imb", "d5_imb", "d10_imb", "micro_off", "spread_ticks", "spread_frac",
    )}
    if exec_file:
        mids, extra = v1.align_exec_book(file, exec_file)
        if not np.isfinite(mids).any():
            mids = sig_mids
    else:
        mids = sig_mids

    y_raw, tau_raw, valid_raw = label_first_touch_tau(mids, ts_sec, horizon_s, theta)

    last_in_bucket: dict[int, int] = {}
    for i, t in enumerate(times):
        b = int((t - t0).total_seconds() * 1000.0) // max(grid_ms, 1)
        last_in_bucket[b] = i
    if not last_in_bucket:
        raise RuntimeError("empty day")
    b0, b1 = min(last_in_bucket), max(last_in_bucket)
    g_idx = []
    last = last_in_bucket[b0]
    warmup_b = int(v1.WARMUP.total_seconds() * 1000.0) // max(grid_ms, 1)
    for b in range(b0, b1 + 1):
        if b in last_in_bucket:
            last = last_in_bucket[b]
        if b < b0 + warmup_b:
            continue
        g_idx.append(last)
    g_idx = np.array(g_idx, dtype=np.int32)
    feat = np.stack([_row_feat(file[i], extra, i, keys) for i in g_idx], axis=0)
    t_ms = np.array(
        [int((times[i] - t0).total_seconds() * 1000.0) for i in g_idx],
        dtype=np.int64,
    )
    return {
        "t_ms": t_ms,
        "mid": mids[g_idx].astype(np.float32),
        "feat": feat,
        "feat_names": np.array(keys),
        "grid_ms": np.int32(grid_ms),
        "y": y_raw[g_idx],
        "tau": tau_raw[g_idx],
        "valid": valid_raw[g_idx],
        "t0_epoch_ms": np.int64(int(t0.timestamp() * 1000)),
    }


def save_seq_npz(path: Path, day: dict) -> None:
    np.savez_compressed(path, **day)


def load_seq_npz(path: Path) -> dict:
    with np.load(path, allow_pickle=True) as z:
        d = {k: z[k] for k in z.files}
    d["feat_names"] = [str(x) for x in d["feat_names"].tolist()]
    return d


def _as_float_list(v) -> list[float]:
    if v is None:
        return []
    if isinstance(v, (list, tuple)):
        return [float(x) for x in v]
    return []


def attach_lob_jsonl(day: dict, path: Path, grid_ms: int) -> dict:
    """把 C++ 单独写的 YYYYMMDD.lob.jsonl 对齐到因子网格，填 vol/ofi。"""
    if day.get("vol") is not None and getattr(day.get("vol"), "ndim", 0) == 3:
        return day
    if not path.is_file():
        return day
    recs = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            recs.append(json.loads(line))
    if not recs:
        return day

    bucket = np.array(recs[0].get("bucket_bp") or [1, 2, 5, 10], dtype=np.float32)
    B = int(bucket.size)
    lob_ms = np.empty(len(recs), dtype=np.int64)
    for i, r in enumerate(recs):
        u = r.get("t_unix_ms")
        if u is not None:
            lob_ms[i] = int(u)
        else:
            lob_ms[i] = int(v1.parse_timestamp(r["timestamp"]).timestamp() * 1000)

    t_ms = np.asarray(day["t_ms"], dtype=np.int64)
    t0 = int(np.asarray(day["t0_epoch_ms"]).reshape(-1)[0])
    grid_ms_abs = t0 + t_ms
    n = len(t_ms)
    vol = np.zeros((n, 2, B), dtype=np.float32)
    ofi = np.zeros((n, 2, B), dtype=np.float32)
    j = 0
    matched = 0
    tol = max(int(grid_ms) * 3 // 2, 150)
    for i, gs in enumerate(grid_ms_abs):
        while j + 1 < len(lob_ms) and abs(int(lob_ms[j + 1]) - int(gs)) <= abs(int(lob_ms[j]) - int(gs)):
            j += 1
        if abs(int(lob_ms[j]) - int(gs)) > tol:
            continue
        r = recs[j]
        vb = _as_float_list(r.get("vol_bid"))
        va = _as_float_list(r.get("vol_ask"))
        ob = _as_float_list(r.get("ofi_bid"))
        oa = _as_float_list(r.get("ofi_ask"))
        for b in range(B):
            if b < len(vb):
                vol[i, 0, b] = vb[b]
            if b < len(va):
                vol[i, 1, b] = va[b]
            if b < len(ob):
                ofi[i, 0, b] = ob[b]
            if b < len(oa):
                ofi[i, 1, b] = oa[b]
        matched += 1
    if matched < 10:
        return day
    day["vol"] = vol
    day["ofi"] = ofi
    day["bucket_bp"] = bucket
    day["grid_ms"] = np.int32(grid_ms)
    return day


def attach_labels(day: dict, horizon_s: float, theta: float):
    """npz 若无标签，用网格 mid 打标。pack 自 jsonl 时已用原始 tick 打过。"""
    if "y" in day and "tau" in day and "valid" in day:
        return day
    mid = np.asarray(day["mid"], dtype=np.float64)
    t_ms = np.asarray(day["t_ms"], dtype=np.float64)
    ts = (t_ms - t_ms[0]) * 1e-3
    y, tau, valid = label_first_touch_tau(mid, ts, horizon_s, theta)
    day["y"], day["tau"], day["valid"] = y, tau, valid
    return day


class SeqWindows:
    def __init__(self, days: list[dict], feat_names: list[str], lookback: int,
                 horizon_s: float, hazard_ms: int, stride: int):
        self.feat_names = feat_names
        self.lookback = lookback
        self.n_bins = int(round(horizon_s * 1000.0 / hazard_ms))
        self.hazard_s = hazard_ms / 1000.0
        self.horizon_s = horizon_s
        self.has_vol = all("vol" in d and d["vol"].ndim == 3 for d in days)
        self.has_ofi = all("ofi" in d and d["ofi"].ndim == 3 for d in days)
        self.buckets = int(days[0]["vol"].shape[-1]) if self.has_vol else 0
        self.X = []
        self.vol = []
        self.ofi = []
        self.side = []  # 0 censor, 1 up, 2 down
        self.bin_idx = []
        self.t = []
        self.y = []
        F = len(feat_names)
        for di, d in enumerate(days):
            names = d["feat_names"]
            col = {k: i for i, k in enumerate(names)}
            feat = np.full((len(d["feat"]), F), np.nan, dtype=np.float32)
            for j, k in enumerate(feat_names):
                if k in col:
                    feat[:, j] = d["feat"][:, col[k]]
            valid = np.asarray(d["valid"], dtype=bool)
            y = np.asarray(d["y"], dtype=np.int8)
            tau = np.asarray(d["tau"], dtype=np.float32)
            t_ms = np.asarray(d["t_ms"], dtype=np.int64)
            vol = d["vol"] if self.has_vol else None
            ofi = d["ofi"] if self.has_ofi else None
            n = len(feat)
            for i in range(lookback - 1, n, stride):
                if not valid[i]:
                    continue
                sl = slice(i + 1 - lookback, i + 1)
                self.X.append(feat[sl])
                if vol is not None:
                    self.vol.append(np.transpose(vol[sl], (1, 2, 0)))  # (2,B,T)
                if ofi is not None:
                    self.ofi.append(np.transpose(ofi[sl], (1, 2, 0)))
                if y[i] > 0:
                    self.side.append(1)
                    self.bin_idx.append(int(min(self.n_bins - 1, max(0, tau[i] / self.hazard_s))))
                elif y[i] < 0:
                    self.side.append(2)
                    self.bin_idx.append(int(min(self.n_bins - 1, max(0, tau[i] / self.hazard_s))))
                else:
                    self.side.append(0)
                    self.bin_idx.append(self.n_bins - 1)
                self.t.append(float(di) * 86_400.0 + float(t_ms[i]) * 1e-3)
                self.y.append(int(y[i]))
        if not self.X:
            raise RuntimeError("no sequence windows")
        self.X = np.stack(self.X, axis=0)
        self.side = np.array(self.side, dtype=np.int64)
        self.bin_idx = np.array(self.bin_idx, dtype=np.int64)
        self.t = np.array(self.t, dtype=np.float64)
        self.y = np.array(self.y, dtype=np.int8)
        if self.has_vol:
            self.vol = np.stack(self.vol, axis=0)
        if self.has_ofi:
            self.ofi = np.stack(self.ofi, axis=0)

    def fit_norm(self, idx: np.ndarray):
        x = self.X[idx]
        med = np.nanmedian(x.reshape(-1, x.shape[-1]), axis=0)
        q1 = np.nanpercentile(x.reshape(-1, x.shape[-1]), 25, axis=0)
        q3 = np.nanpercentile(x.reshape(-1, x.shape[-1]), 75, axis=0)
        iqr = np.maximum(q3 - q1, 1e-6)
        self.feat_median = med.astype(np.float32)
        self.feat_iqr = iqr.astype(np.float32)

    def _norm(self, x):
        z = (x - self.feat_median) / self.feat_iqr
        return np.nan_to_num(z, nan=0.0, posinf=0.0, neginf=0.0).astype(np.float32)

    def tensors(self, idx, torch):
        x = self._norm(self.X[idx])
        now = x[:, -1, :]
        out = {
            "feat_seq": torch.from_numpy(np.transpose(x, (0, 2, 1))),  # (B,F,T)
            "feat_now": torch.from_numpy(now),
            "side": torch.from_numpy(self.side[idx]),
            "bin": torch.from_numpy(self.bin_idx[idx]),
        }
        if self.has_vol:
            v = np.nan_to_num(self.vol[idx], nan=0.0).astype(np.float32)
            out["vol"] = torch.from_numpy(np.log1p(np.maximum(v, 0.0)))
        if self.has_ofi:
            o = np.nan_to_num(self.ofi[idx], nan=0.0).astype(np.float32)
            out["ofi"] = torch.from_numpy(o)
        return out


def build_model(torch, nn, n_feat: int, lookback: int, n_bins: int,
                use_vol: bool, use_ofi: bool, n_buckets: int, hidden: int = HIDDEN):
    F = nn.functional

    class _Causal(nn.Module):
        def __init__(self, in_ch, out_ch, kernel=3, dilation=1):
            super().__init__()
            pad = (kernel - 1) * dilation
            self.conv = nn.Conv1d(in_ch, out_ch, kernel, dilation=dilation, padding=pad)
            self.crop = pad

        def forward(self, x):
            y = self.conv(x)
            return y[:, :, :-self.crop] if self.crop else y

    class TCNBlock(nn.Module):
        def __init__(self, ch, dilation):
            super().__init__()
            self.c1 = _Causal(ch, ch, 3, dilation)
            self.c2 = _Causal(ch, ch, 3, dilation)
            self.n1 = nn.BatchNorm1d(ch)
            self.n2 = nn.BatchNorm1d(ch)
            self.drop = nn.Dropout(0.1)

        def forward(self, x):
            h = self.drop(F.relu(self.n1(self.c1(x))))
            h = self.drop(F.relu(self.n2(self.c2(h))))
            return F.relu(h + x)

    class DeepLOB(nn.Module):
        def __init__(self, levels, out_dim):
            super().__init__()
            self.conv = nn.Sequential(
                nn.Conv2d(1, 16, kernel_size=(1, 5), padding=(0, 2)),
                nn.LeakyReLU(0.01),
                nn.Conv2d(16, 16, kernel_size=(1, 5), padding=(0, 2)),
                nn.LeakyReLU(0.01),
                nn.Conv2d(16, 32, kernel_size=(3, 1), padding=(1, 0)),
                nn.LeakyReLU(0.01),
                nn.Conv2d(32, 32, kernel_size=(3, 1), padding=(1, 0)),
                nn.LeakyReLU(0.01),
            )
            self.lstm = nn.LSTM(32 * levels, 64, batch_first=True)
            self.proj = nn.Linear(64, out_dim)

        def forward(self, x):
            b = x.shape[0]
            h = self.conv(x.reshape(b, 1, x.shape[1] * x.shape[2], x.shape[3]))
            b, c, lev, t = h.shape
            h = h.permute(0, 3, 1, 2).reshape(b, t, c * lev)
            out, _ = self.lstm(h)
            return self.proj(out[:, -1])

    class CompetingRiskLOB(nn.Module):
        def __init__(self):
            super().__init__()
            self.use_vol = use_vol
            self.use_ofi = use_ofi
            self.n_bins = n_bins
            ch = hidden
            self.feat_in = nn.Conv1d(n_feat, ch, 1)
            self.tcn = nn.Sequential(TCNBlock(ch, 1), TCNBlock(ch, 2), TCNBlock(ch, 4))
            self.now = nn.Sequential(nn.Linear(n_feat, ch), nn.ReLU(), nn.Linear(ch, ch))
            branches = 2
            if use_vol:
                self.deeplob = DeepLOB(2 * n_buckets, ch)
                branches += 1
            if use_ofi:
                self.ofi_in = nn.Conv1d(2 * n_buckets, ch, 1)
                self.ofi_tcn = nn.Sequential(TCNBlock(ch, 1), TCNBlock(ch, 2))
                branches += 1
            self.fuse = nn.Sequential(
                nn.Linear(ch * branches, hidden),
                nn.ReLU(),
                nn.Dropout(0.1),
                nn.Linear(hidden, hidden),
                nn.ReLU(),
            )
            self.head = nn.Linear(hidden, n_bins * 3)

        def encode(self, batch):
            h_seq = self.tcn(self.feat_in(batch["feat_seq"]))[:, :, -1]
            parts = [h_seq, self.now(batch["feat_now"])]
            if self.use_vol:
                parts.append(self.deeplob(batch["vol"]))
            if self.use_ofi:
                b, two, bk, t = batch["ofi"].shape
                o = batch["ofi"].reshape(b, two * bk, t)
                parts.append(self.ofi_tcn(self.ofi_in(o))[:, :, -1])
            return self.fuse(torch.cat(parts, dim=-1))

        def forward(self, batch):
            z = self.encode(batch)
            return self.head(z).view(-1, self.n_bins, 3)

    return CompetingRiskLOB()


def cr_nll(logits, side, bin_idx):
    torch = sys.modules["torch"]
    F = torch.nn.functional
    logp = F.log_softmax(logits, dim=-1)
    surv = logp[..., 2]
    cum = torch.cumsum(surv, dim=1)
    surv_before = torch.cat([torch.zeros_like(surv[:, :1]), cum[:, :-1]], dim=1)
    ar = torch.arange(logits.shape[0], device=logits.device)
    nll_censor = -surv.sum(dim=1)
    k = bin_idx.clamp(0, logits.shape[1] - 1)
    ev_lp = torch.where(side == 1, logp[ar, k, 0], logp[ar, k, 1])
    nll_event = -(surv_before[ar, k] + ev_lp)
    nll = torch.where(side > 0, nll_event, nll_censor)
    return nll.mean()


def accumulate_p(logits):
    torch = sys.modules["torch"]
    F = torch.nn.functional
    p = F.softmax(logits, dim=-1)
    surv = p[..., 2]
    cum = torch.cumprod(surv, dim=1)
    stay_before = torch.cat([torch.ones_like(surv[:, :1]), cum[:, :-1]], dim=1)
    p_up = (stay_before * p[..., 0]).sum(dim=1)
    p_dn = (stay_before * p[..., 1]).sum(dim=1)
    return p_up, p_dn


def load_days(horizon_s, theta, grid_ms, pack: bool):
    signal_sym, trade_sym, inst_path = v1.load_inst_pair()
    split_exec = signal_sym != trade_sym
    folder = v1.find_symbol_dir(signal_sym)
    signal_days = v1.load_day_records(folder)
    if not signal_days:
        raise RuntimeError(f"no jsonl in {folder}")
    exec_days = v1.load_day_records(v1.find_symbol_dir(trade_sym)) if split_exec else {}
    day_keys = sorted(signal_days)
    present = set()
    for d in day_keys:
        sample = signal_days[d]
        for rec in sample[:40] + sample[-3:]:
            present.update(rec.keys())
    keys = _feat_keys(present)
    days = []
    for d in day_keys:
        npz_path = folder / f"{d}.seq.npz"
        if npz_path.is_file() and not pack:
            print(f"  load {npz_path.name}", flush=True)
            day = load_seq_npz(npz_path)
            day = attach_labels(day, horizon_s, theta)
        else:
            print(f"  pack jsonl {d}", flush=True)
            day = pack_jsonl_day(
                signal_days[d], exec_days.get(d, []) if split_exec else None,
                keys, grid_ms, horizon_s, theta,
            )
            if pack:
                save_seq_npz(npz_path, day)
                print(f"    wrote {npz_path}", flush=True)
        day = attach_lob_jsonl(day, folder / f"{d}.lob.jsonl", grid_ms)
        if pack and day.get("vol") is not None:
            save_seq_npz(npz_path, day)
        days.append(day)
    return days, keys, signal_sym, trade_sym, day_keys, folder


def train(args):
    import torch
    import torch.nn as nn
    sys.modules["torch"] = torch

    days, keys, signal_sym, trade_sym, day_keys, folder = load_days(
        args.horizon, args.theta, args.grid_ms, pack=False,
    )
    lookback = max(8, int(round(args.lookback / (args.grid_ms / 1000.0))))
    data = SeqWindows(days, keys, lookback, args.horizon, args.hazard_ms, args.stride)
    n = len(data.y)
    train_m, calib_m, test_m = v1.time_masks(data.t, v1.TRAIN_FRAC, v1.CALIB_FRAC, v1.PURGE_S)
    if int(np.sum(train_m)) < 200:
        cut_tr = int(n * v1.TRAIN_FRAC)
        cut_ca = int(n * (v1.TRAIN_FRAC + v1.CALIB_FRAC))
        train_m = np.zeros(n, dtype=bool)
        calib_m = np.zeros(n, dtype=bool)
        test_m = np.zeros(n, dtype=bool)
        train_m[:cut_tr] = True
        calib_m[cut_tr:cut_ca] = True
        test_m[cut_ca:] = True
    tr = np.flatnonzero(train_m)
    ca = np.flatnonzero(calib_m)
    te = np.flatnonzero(test_m)
    data.fit_norm(tr)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = build_model(
        torch, nn, len(keys), lookback, data.n_bins,
        data.has_vol, data.has_ofi, data.buckets, args.hidden,
    ).to(device)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)

    def run_epoch(idx, train=True):
        model.train(train)
        total, nbat = 0.0, 0
        order = np.random.permutation(idx) if train else idx
        for s in range(0, len(order), args.batch):
            sl = order[s:s + args.batch]
            if len(sl) < 8:
                continue
            batch = {k: v.to(device) for k, v in data.tensors(sl, torch).items()}
            logits = model(batch)
            loss = cr_nll(logits, batch["side"], batch["bin"])
            if train:
                opt.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                opt.step()
            total += float(loss.detach())
            nbat += 1
        return total / max(nbat, 1)

    print(
        f"backend=torch/{device}  n={n} train={len(tr)} calib={len(ca)} test={len(te)} "
        f"lookback={lookback} bins={data.n_bins} deeplob={data.has_vol} ofi_seq={data.has_ofi}",
        flush=True,
    )
    for ep in range(1, args.epochs + 1):
        tr_loss = run_epoch(tr, True)
        ca_loss = run_epoch(ca, False) if len(ca) >= 32 else float("nan")
        print(f"epoch {ep:02d}  train_nll={tr_loss:.4f}  calib_nll={ca_loss:.4f}", flush=True)

    @torch.no_grad()
    def predict_split(idx):
        model.eval()
        ups, dns = [], []
        for s in range(0, len(idx), args.batch):
            sl = idx[s:s + args.batch]
            batch = {k: v.to(device) for k, v in data.tensors(sl, torch).items()}
            pu, pd = accumulate_p(model(batch))
            ups.append(pu.cpu().numpy())
            dns.append(pd.cpu().numpy())
        return np.concatenate(ups), np.concatenate(dns)

    p_up_ca, p_dn_ca = predict_split(ca) if len(ca) else (np.array([]), np.array([]))
    p_up_te, p_dn_te = predict_split(te) if len(te) else (np.array([]), np.array([]))
    y_ca, y_te = data.y[ca], data.y[te]
    t_ca, t_te = data.t[ca], data.t[te]
    cal_up = v1.IsotonicCalibrator().fit(p_up_ca, (y_ca == 1).astype(float)) if len(ca) > 80 else v1.IsotonicCalibrator()
    cal_dn = v1.IsotonicCalibrator().fit(p_dn_ca, (y_ca == -1).astype(float)) if len(ca) > 80 else v1.IsotonicCalibrator()
    if len(ca) > 80:
        p_up_ca, p_dn_ca = cal_up.transform(p_up_ca), cal_dn.transform(p_dn_ca)
        p_up_te, p_dn_te = cal_up.transform(p_up_te), cal_dn.transform(p_dn_te)

    up_best, _ = v1.choose_threshold(p_up_ca, (y_ca == 1).astype(np.int32), t_ca, args.target_ppv, args.horizon) if len(ca) else (None, [])
    dn_best, _ = v1.choose_threshold(p_dn_ca, (y_ca == -1).astype(np.int32), t_ca, args.target_ppv, args.horizon) if len(ca) else (None, [])

    out_dir = v1.PYTOOL / "models" / "tcn_deeplob"
    out_dir.mkdir(parents=True, exist_ok=True)
    arch = {
        "n_feat": len(keys),
        "lookback": lookback,
        "n_bins": data.n_bins,
        "use_vol": data.has_vol,
        "use_ofi": data.has_ofi,
        "n_buckets": data.buckets,
        "hidden": args.hidden,
        "grid_ms": args.grid_ms,
        "hazard_ms": args.hazard_ms,
        "lookback_s": args.lookback,
    }
    torch.save(
        {
            "state_dict": model.state_dict(),
            "arch": arch,
            "feat_names": keys,
            "feat_median": data.feat_median,
            "feat_iqr": data.feat_iqr,
        },
        out_dir / "model.pt",
    )
    meta = {
        "kind": "tcn_deeplob_competing_risk",
        "horizon": args.horizon,
        "theta": args.theta,
        "signal": signal_sym,
        "exec": trade_sym,
        "days": day_keys,
        "feat_names": keys,
        "deeplob_enabled": data.has_vol,
        "ofi_seq_enabled": data.has_ofi,
        "tau_up": None if up_best is None else up_best[0],
        "tau_dn": None if dn_best is None else dn_best[0],
        **arch,
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    with (out_dir / "calib.pkl").open("wb") as f:
        pickle.dump({"cal_up": cal_up, "cal_dn": cal_dn}, f)

    result_dir = v1.PYTOOL / "result"
    result_dir.mkdir(parents=True, exist_ok=True)
    result_path = result_dir / (datetime.now().strftime("%Y%m%d%H%M%S") + "_tcn")
    rf = result_path.open("w", encoding="utf-8")
    orig = sys.stdout
    sys.stdout = v1.Tee(orig, rf)
    try:
        print("========== TCN / DeepLOB 竞争风险 ==========")
        print(json.dumps(meta, ensure_ascii=False, indent=2))
        print(f"P(hit) train={float(np.mean(data.y[tr] != 0)):.3f} test={float(np.mean(y_te != 0)):.3f}")
        if len(te):
            print(f"test AUC up={v1.auc(p_up_te, y_te == 1):.3f}  dn={v1.auc(p_dn_te, y_te == -1):.3f}")
        for name, best, p, y_pos, t in (
            ("up", up_best, p_up_te, (y_te == 1).astype(np.int32), t_te),
            ("dn", dn_best, p_dn_te, (y_te == -1).astype(np.int32), t_te),
        ):
            if best is None or len(te) == 0:
                print(f"{name}: no threshold")
                continue
            n_s, ppv, cov = v1.ppv_coverage(p, y_pos, t, best[0], args.horizon)
            print(f"{name} c={best[0]:.4f}  n={n_s}  PPV={100 * ppv:5.1f}%  cover={100 * cov:.3f}%")
        print(f"wrote {out_dir}")
        print(f"  model.pt   网络权重 state_dict + 归一化")
        print(f"  meta.json  结构/标签/阈值（不是线性 T_Param）")
        print(f"  calib.pkl  p+/p- isotonic")
        if not data.has_vol:
            print("DeepLOB CNN 未启用：没有 vol 张量。打开 enable_training_capture 后会单独写 YYYYMMDD.lob.jsonl。")
    finally:
        sys.stdout = orig
        rf.close()
    print(f"wrote {result_path}")


def main():
    ap = argparse.ArgumentParser(description="PDF 第二版 TCN/DeepLOB + 竞争风险")
    ap.add_argument("--horizon", type=float, default=v1.HORIZON_S)
    ap.add_argument("--theta", type=float, default=v1.THETA)
    ap.add_argument("--grid-ms", type=int, default=v1.GRID_MS)
    ap.add_argument("--hazard-ms", type=int, default=HAZARD_MS)
    ap.add_argument("--lookback", type=float, default=LOOKBACK_S)
    ap.add_argument("--epochs", type=int, default=EPOCHS)
    ap.add_argument("--batch", type=int, default=BATCH)
    ap.add_argument("--lr", type=float, default=LR)
    ap.add_argument("--hidden", type=int, default=HIDDEN)
    ap.add_argument("--stride", type=int, default=STRIDE)
    ap.add_argument("--target-ppv", type=float, default=v1.TARGET_PPV)
    ap.add_argument("--pack", action="store_true", help="只把 jsonl 打成 .seq.npz，不训练")
    args = ap.parse_args()
    if args.pack:
        load_days(args.horizon, args.theta, args.grid_ms, pack=True)
        return
    train(args)


if __name__ == "__main__":
    main()
