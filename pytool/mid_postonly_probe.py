"""在当前 mid 上挂 Post-Only 最小单，成交立刻平仓，共 20 次，统计吃单概率与时长。

凭证格式与 C++ 一致（credential.cfg）：
  第 1 行  API Key
  中间     Ed25519 PEM 私钥
  最后一行 0=主网 / 1=测试网

默认交易 USD-M 永续（与 instId.cfg、行情流一致）。
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
import urllib.error
import urllib.parse
import urllib.request
from base64 import b64encode
from decimal import ROUND_CEILING, ROUND_HALF_EVEN, Decimal
from pathlib import Path

from cryptography.hazmat.primitives.serialization import load_pem_private_key

ROOT = Path(__file__).resolve().parent.parent
FILLED_STATUSES = {"FILLED"}
DEAD_STATUSES = {"CANCELED", "EXPIRED", "REJECTED"}


def load_credential(path: Path) -> tuple[str, str, bool]:
    text = path.read_text(encoding="utf-8")
    while text.endswith("\n") or text.endswith("\r"):
        text = text[:-1]
    first_end = text.find("\n")
    if first_end < 0:
        raise RuntimeError(f"invalid credential file: {path}")
    api_key = text[:first_end].rstrip("\r")
    remainder = text[first_end + 1 :]
    last_end = remainder.rfind("\n")
    if last_end < 0:
        testnet_line, pem = remainder, ""
    else:
        testnet_line = remainder[last_end + 1 :].rstrip("\r")
        pem = remainder[:last_end].rstrip("\r")
    use_testnet = testnet_line in {"1", "true", "TRUE"}
    if not api_key or "BEGIN" not in pem:
        raise RuntimeError(f"failed to parse API key / Ed25519 PEM from {path}")
    return api_key, pem, use_testnet


def load_symbol(path: Path) -> str:
    symbol = path.read_text(encoding="utf-8").strip().upper()
    if not symbol:
        raise RuntimeError(f"empty symbol in {path}")
    return symbol


def fmt_dec(value: Decimal) -> str:
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"


def quantize(value: Decimal, step: Decimal, rounding) -> Decimal:
    return (value / step).to_integral_value(rounding=rounding) * step


def median_or_none(values: list[float]) -> float | None:
    return float(statistics.median(values)) if values else None


class BinanceUmFutures:
    def __init__(self, api_key: str, pem: str, testnet: bool):
        self.api_key = api_key
        self.private_key = load_pem_private_key(pem.encode("utf-8"), password=None)
        self.base = (
            "https://testnet.binancefuture.com" if testnet else "https://fapi.binance.com"
        )
        self.time_offset_ms = 0

    def sync_time(self) -> None:
        server = int(self.public("GET", "/fapi/v1/time")["serverTime"])
        self.time_offset_ms = server - int(time.time() * 1000)

    def _timestamp(self) -> int:
        return int(time.time() * 1000) + self.time_offset_ms

    def public(self, method: str, path: str, params: dict | None = None):
        return self._request(method, path, params or {}, signed=False)

    def signed(self, method: str, path: str, params: dict | None = None):
        return self._request(method, path, params or {}, signed=True)

    def _request(self, method: str, path: str, params: dict, signed: bool):
        params = {k: str(v) for k, v in params.items() if v is not None}
        if signed:
            params["recvWindow"] = "5000"
            params["timestamp"] = str(self._timestamp())
            payload = urllib.parse.urlencode(params)
            signature = b64encode(self.private_key.sign(payload.encode("ASCII"))).decode("ascii")
            body = payload + "&signature=" + urllib.parse.quote(signature, safe="")
        else:
            body = urllib.parse.urlencode(params)

        url = self.base + path
        headers = {
            "X-MBX-APIKEY": self.api_key,
            "User-Agent": "Progressiv-mid-postonly-probe/1.0",
        }
        if method == "GET":
            if body:
                url = url + "?" + body
            req = urllib.request.Request(url, method="GET", headers=headers)
        else:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
            req = urllib.request.Request(
                url, data=body.encode("ascii"), method=method, headers=headers
            )

        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                raw = resp.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            try:
                data = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                data = {"code": exc.code, "msg": raw}
            raise BinanceApiError(data.get("code", exc.code), data.get("msg", raw), data) from exc

        data = json.loads(raw) if raw else {}
        if isinstance(data, dict) and "code" in data and data["code"] not in (0, 200, None):
            raise BinanceApiError(data["code"], data.get("msg", raw), data)
        return data


class BinanceApiError(RuntimeError):
    def __init__(self, code, msg, raw):
        super().__init__(f"Binance error {code}: {msg}")
        self.code = code
        self.msg = msg
        self.raw = raw


def load_filters(client: BinanceUmFutures, symbol: str) -> dict:
    info = client.public("GET", "/fapi/v1/exchangeInfo", {"symbol": symbol})
    symbols = info.get("symbols") or []
    if not symbols:
        raise RuntimeError(f"symbol not found in exchangeInfo: {symbol}")
    filters = {f["filterType"]: f for f in symbols[0]["filters"]}
    tick = Decimal(filters["PRICE_FILTER"]["tickSize"])
    step = Decimal(filters["LOT_SIZE"]["stepSize"])
    min_qty = Decimal(filters["LOT_SIZE"]["minQty"])
    min_notional = Decimal("0")
    if "MIN_NOTIONAL" in filters:
        min_notional = Decimal(str(filters["MIN_NOTIONAL"].get("notional", "0")))
    elif "NOTIONAL" in filters:
        min_notional = Decimal(str(filters["NOTIONAL"].get("minNotional", "0")))
    return {
        "tick": tick,
        "step": step,
        "min_qty": min_qty,
        "min_notional": min_notional,
        "hedge": bool(client.signed("GET", "/fapi/v1/positionSide/dual").get("dualSidePosition")),
    }


def book_mid(client: BinanceUmFutures, symbol: str) -> tuple[Decimal, Decimal, Decimal]:
    book = client.public("GET", "/fapi/v1/ticker/bookTicker", {"symbol": symbol})
    bid = Decimal(book["bidPrice"])
    ask = Decimal(book["askPrice"])
    if bid <= 0 or ask <= 0 or ask < bid:
        raise RuntimeError(f"invalid book: bid={bid} ask={ask}")
    mid = (bid + ask) / Decimal(2)
    return bid, ask, mid


def round_mid_price(mid: Decimal, tick: Decimal) -> Decimal:
    return quantize(mid, tick, ROUND_HALF_EVEN)


def pick_postonly_side(side_arg: str, price: Decimal, bid: Decimal, ask: Decimal) -> str | None:
    """GTX 不能吃单：买价必须 < ask，卖价必须 > bid。"""
    can_buy = price < ask
    can_sell = price > bid
    wanted = side_arg.upper()
    if wanted == "AUTO":
        if can_buy and not can_sell:
            return "BUY"
        if can_sell and not can_buy:
            return "SELL"
        if can_buy and can_sell:
            return "BUY"
        return None
    if wanted == "BUY":
        return "BUY" if can_buy else None
    if wanted == "SELL":
        return "SELL" if can_sell else None
    raise ValueError(f"invalid side: {side_arg}")


def min_qty_at(price: Decimal, filters: dict) -> Decimal:
    by_notional = Decimal(0)
    if filters["min_notional"] > 0 and price > 0:
        by_notional = quantize(filters["min_notional"] / price, filters["step"], ROUND_CEILING)
    qty = max(filters["min_qty"], by_notional)
    qty = quantize(qty, filters["step"], ROUND_CEILING)
    if qty < filters["min_qty"]:
        qty = filters["min_qty"]
    return qty


def position_side_for(hedge: bool, side: str) -> str | None:
    if not hedge:
        return None
    return "LONG" if side == "BUY" else "SHORT"


def close_side_params(hedge: bool, open_side: str, qty: Decimal) -> dict:
    close_side = "SELL" if open_side == "BUY" else "BUY"
    params = {
        "side": close_side,
        "type": "MARKET",
        "quantity": fmt_dec(qty),
        "newOrderRespType": "RESULT",
    }
    if hedge:
        params["positionSide"] = position_side_for(True, open_side)
    else:
        params["reduceOnly"] = "true"
    return params


def place_postonly(client: BinanceUmFutures, symbol: str, side: str, price: Decimal, qty: Decimal, hedge: bool):
    params = {
        "symbol": symbol,
        "side": side,
        "type": "LIMIT",
        "timeInForce": "GTX",
        "price": fmt_dec(price),
        "quantity": fmt_dec(qty),
        "newOrderRespType": "RESULT",
    }
    pos = position_side_for(hedge, side)
    if pos:
        params["positionSide"] = pos
    return client.signed("POST", "/fapi/v1/order", params)


def get_order(client: BinanceUmFutures, symbol: str, order_id) -> dict:
    return client.signed("GET", "/fapi/v1/order", {"symbol": symbol, "orderId": str(order_id)})


def cancel_order(client: BinanceUmFutures, symbol: str, order_id) -> None:
    try:
        client.signed("DELETE", "/fapi/v1/order", {"symbol": symbol, "orderId": str(order_id)})
    except BinanceApiError as exc:
        if exc.code in (-2011, -1013):
            return
        raise


def executed_qty(order: dict) -> Decimal:
    return Decimal(str(order.get("executedQty") or "0"))


def wait_fill(client: BinanceUmFutures, symbol: str, order_id, wait_s: float) -> tuple[dict, bool]:
    deadline = time.monotonic() + wait_s
    last = None
    while time.monotonic() < deadline:
        last = get_order(client, symbol, order_id)
        status = last.get("status")
        if status in FILLED_STATUSES or status in DEAD_STATUSES:
            return last, False
        time.sleep(0.05)
    cancel_order(client, symbol, order_id)
    return get_order(client, symbol, order_id), True


def fill_age_ms(order: dict, placed_exchange_ms: int | None) -> int | None:
    update = int(order.get("updateTime") or 0)
    created = int(order.get("time") or placed_exchange_ms or 0)
    if update <= 0 or created <= 0:
        return None
    return max(0, update - created)


def close_position(client: BinanceUmFutures, symbol: str, hedge: bool, open_side: str, qty: Decimal) -> dict | None:
    if qty <= 0:
        return None
    params = {"symbol": symbol, **close_side_params(hedge, open_side, qty)}
    try:
        return client.signed("POST", "/fapi/v1/order", params)
    except BinanceApiError as exc:
        if exc.code in (-2022, -1111, -4045, -4059):
            return None
        raise


def flatten_symbol(client: BinanceUmFutures, symbol: str, hedge: bool) -> None:
    risks = client.signed("GET", "/fapi/v2/positionRisk", {"symbol": symbol})
    if isinstance(risks, dict):
        risks = [risks]
    for pos in risks:
        amt = Decimal(str(pos.get("positionAmt") or "0"))
        if amt == 0:
            continue
        side = "BUY" if amt > 0 else "SELL"
        qty = abs(amt)
        params = {"symbol": symbol, **close_side_params(hedge, side, qty)}
        if hedge:
            params["positionSide"] = pos.get("positionSide") or params.get("positionSide")
        try:
            client.signed("POST", "/fapi/v1/order", params)
        except BinanceApiError as exc:
            print(f"  flatten warning: {exc}")


def summarize(rows: list[dict]) -> None:
    n = len(rows)
    filled = [r for r in rows if r["result"] == "FILLED"]
    rejected = [r for r in rows if r["result"] == "GTX_REJECT"]
    timeout = [r for r in rows if r["result"] == "TIMEOUT"]
    other = [r for r in rows if r["result"] not in {"FILLED", "GTX_REJECT", "TIMEOUT"}]
    ages = [r["fill_ms"] for r in filled if r.get("fill_ms") is not None]
    print("\n========== 统计 ==========")
    print(f"下单次数        : {n}")
    print(f"成交(吃单)      : {len(filled)}")
    print(f"GTX 无法挂上    : {len(rejected)}")
    print(f"等待超时撤单    : {len(timeout)}")
    if other:
        print(f"其他            : {len(other)}")
    print(f"成功概率        : {len(filled) / n * 100:.2f}%  ({len(filled)}/{n})")
    if ages:
        print(
            "吃单时长(ms)    : "
            f"n={len(ages)}  mean={statistics.mean(ages):.1f}  "
            f"median={median_or_none(ages):.1f}  "
            f"min={min(ages)}  max={max(ages)}"
        )
    else:
        print("吃单时长(ms)    : 无成交样本")
    print("==========================")


def run(args: argparse.Namespace) -> int:
    cred_path = Path(args.credential)
    api_key, pem, testnet = load_credential(cred_path)
    symbol = args.symbol or load_symbol(Path(args.inst_id))
    client = BinanceUmFutures(api_key, pem, testnet)
    client.sync_time()
    filters = load_filters(client, symbol)
    env = "testnet" if testnet else "mainnet"
    print(
        f"{symbol} {env}  hedge={filters['hedge']}  "
        f"tick={fmt_dec(filters['tick'])}  step={fmt_dec(filters['step'])}  "
        f"minQty={fmt_dec(filters['min_qty'])}  minNotional={fmt_dec(filters['min_notional'])}"
    )
    bid, ask, mid = book_mid(client, symbol)
    print(f"book  bid={fmt_dec(bid)}  ask={fmt_dec(ask)}  mid={fmt_dec(mid)}")

    if args.dry_run:
        price = round_mid_price(mid, filters["tick"])
        side = pick_postonly_side(args.side, price, bid, ask)
        qty = min_qty_at(price, filters)
        print(f"dry-run  side={side}  price={fmt_dec(price)}  qty={fmt_dec(qty)}")
        return 0

    rows: list[dict] = []
    try:
        for i in range(1, args.orders + 1):
            bid, ask, mid = book_mid(client, symbol)
            price = round_mid_price(mid, filters["tick"])
            side = pick_postonly_side(args.side, price, bid, ask)
            if side is None:
                row = {
                    "i": i,
                    "result": "GTX_REJECT",
                    "reason": "mid rounds onto opposite touch / locked book",
                    "mid": fmt_dec(mid),
                    "price": fmt_dec(price),
                    "fill_ms": None,
                }
                rows.append(row)
                print(
                    f"[{i:02d}/{args.orders}] mid={fmt_dec(mid)} px={fmt_dec(price)} "
                    f"bid={fmt_dec(bid)} ask={fmt_dec(ask)} -> GTX 无法挂上（价会吃单）"
                )
                continue

            qty = min_qty_at(price, filters)
            t0 = time.perf_counter()
            try:
                placed = place_postonly(client, symbol, side, price, qty, filters["hedge"])
            except BinanceApiError as exc:
                gtx = exc.code in (-5022, -2021) or "Post Only" in str(exc.msg) or "could not be executed as maker" in str(exc.msg)
                row = {
                    "i": i,
                    "result": "GTX_REJECT" if gtx else "ERROR",
                    "reason": str(exc),
                    "mid": fmt_dec(mid),
                    "price": fmt_dec(price),
                    "side": side,
                    "fill_ms": None,
                }
                rows.append(row)
                print(f"[{i:02d}/{args.orders}] {side} {fmt_dec(qty)}@{fmt_dec(price)} -> {row['result']}: {exc}")
                continue

            order_id = placed.get("orderId")
            status = placed.get("status")
            timed_out = False
            if status in FILLED_STATUSES or status in DEAD_STATUSES:
                final = placed
            else:
                final, timed_out = wait_fill(client, symbol, order_id, args.wait)

            exec_qty = executed_qty(final)
            if exec_qty > 0:
                close_position(client, symbol, filters["hedge"], side, exec_qty)
                fill_ms = fill_age_ms(final, int(placed.get("transactTime") or 0))
                if fill_ms is None:
                    fill_ms = int((time.perf_counter() - t0) * 1000)
                row = {
                    "i": i,
                    "result": "FILLED",
                    "side": side,
                    "mid": fmt_dec(mid),
                    "price": fmt_dec(price),
                    "qty": fmt_dec(exec_qty),
                    "fill_ms": fill_ms,
                    "status": final.get("status"),
                }
                rows.append(row)
                print(
                    f"[{i:02d}/{args.orders}] {side} {fmt_dec(exec_qty)}@{fmt_dec(price)} "
                    f"mid={fmt_dec(mid)} -> FILLED 吃单 {fill_ms} ms, 已市价平仓"
                )
            else:
                result = "TIMEOUT" if timed_out else "GTX_REJECT"
                if not timed_out and final.get("status") not in {"REJECTED", "EXPIRED"}:
                    result = final.get("status") or "TIMEOUT"
                row = {
                    "i": i,
                    "result": result,
                    "side": side,
                    "mid": fmt_dec(mid),
                    "price": fmt_dec(price),
                    "fill_ms": None,
                    "status": final.get("status"),
                }
                rows.append(row)
                print(
                    f"[{i:02d}/{args.orders}] {side} {fmt_dec(qty)}@{fmt_dec(price)} "
                    f"mid={fmt_dec(mid)} -> {row['result']} status={final.get('status')}"
                )
            time.sleep(args.pause)
    finally:
        try:
            flatten_symbol(client, symbol, filters["hedge"])
        except Exception as exc:  # noqa: BLE001
            print(f"exit flatten failed: {exc}")

    summarize(rows)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="mid-price post-only fill probe")
    parser.add_argument("--credential", default=str(ROOT / "credential.cfg"))
    parser.add_argument("--inst-id", default=str(ROOT / "instId.cfg"))
    parser.add_argument("--symbol", default="")
    parser.add_argument("--orders", type=int, default=20)
    parser.add_argument("--wait", type=float, default=8.0, help="等待成交秒数，超时撤单并计入失败")
    parser.add_argument("--pause", type=float, default=0.2, help="每轮之间的间隔秒")
    parser.add_argument("--side", default="auto", choices=["auto", "BUY", "SELL", "buy", "sell"])
    parser.add_argument("--dry-run", action="store_true", help="只读盘口和最小下单量，不下单")
    return parser.parse_args()


if __name__ == "__main__":
    raise SystemExit(run(parse_args()))
