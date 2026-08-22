//
// Created by zagym on 09/08/2026.
//

#ifndef PROGRESSIV_BINANCE_INTERFACE_H
#define PROGRESSIV_BINANCE_INTERFACE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct asset_balance
{
    std::string asset;
    std::string free;
    std::string locked;
};

struct orderbook_level
{
    float price = 0.f;
    float quantity = 0.f;
};

struct orderbook_info
{
    std::string symbol;
    long long last_update_id = 0;
    long long first_update_id = 0; // WS 增量 U（partial 流通常为 0）
    long long prev_update_id = 0;  // WS 增量 pu
    long long transact_time = 0;
    long long message_time = 0;
    std::vector<orderbook_level> bids;
    std::vector<orderbook_level> asks;
};

struct funding_info
{
    std::string symbol;
    float funding_rate = 0.f;
    float mark_price = 0.f;
    float index_price = 0.f;
    long long next_funding_time = 0;
    long long event_time = 0;
    long long message_time = 0;
};

struct agg_trade_info
{
    std::string symbol;
    float price = 0.f;
    float quantity = 0.f;
    bool buyer_is_maker = false; // m=true → 主动卖；m=false → 主动买
    long long trade_time = 0;
    long long message_time = 0;
};

struct aggressor_flow
{
    float buy_qty = 0.f;   // taker buy
    float sell_qty = 0.f;  // taker sell
    float net_qty = 0.f;   // buy - sell
    float imbalance = 0.f; // (buy-sell)/(buy+sell)
};

// USD-M 强平：@forceOrder。S=SELL 表示多头被强平，S=BUY 表示空头被强平
struct force_order_info
{
    std::string symbol;
    std::string side; // BUY / SELL
    float orig_qty = 0.f;
    float last_qty = 0.f;
    float filled_qty = 0.f;
    float price = 0.f;
    float avg_price = 0.f;
    long long trade_time = 0;
    long long event_time = 0;
    long long message_time = 0;
};

struct liquidation_flow
{
    float long_qty = 0.f;  // 多头强平（force SELL）
    float short_qty = 0.f; // 空头强平（force BUY）
    float net_qty = 0.f;   // long - short
    float imbalance = 0.f;
    int count = 0;
};

// /fapi/v1/openInterest：币安没有 OI websocket，后台线程轮询当流用
struct open_interest_info
{
    std::string symbol;
    float open_interest = 0.f;
    long long exchange_time = 0;
    long long message_time = 0;
};

struct order_request
{
    std::string symbol;
    std::string side;
    std::string type;
    std::string time_in_force;
    std::string price;
    std::string stop_price;   // STOP_MARKET / TAKE_PROFIT_MARKET 等
    std::string quantity;
    std::string quote_order_qty;
    std::string client_order_id;
    bool reduce_only = false; // 合约只减仓
    bool close_position = false; // 触发后平掉该方向全部仓位
};

struct order_info
{
    std::string symbol;
    long long order_id = 0;
    long long order_list_id = -1;
    std::string client_order_id;
    std::string status;
    std::string side;
    std::string type;
    std::string time_in_force;
    std::string price;
    std::string stop_price;
    std::string orig_qty;
    std::string executed_qty;
    std::string cummulative_quote_qty;
    std::string position_side; // 合约: BOTH / LONG / SHORT
    long long transact_time = 0;
    std::string raw_json;
};

// USD-M 合约持仓
struct position_info
{
    std::string symbol;
    float position_amt = 0.f;       // >0 多，<0 空；对冲模式看 position_side
    float entry_price = 0.f;
    float mark_price = 0.f;
    float unrealized_profit = 0.f;
    float liquidation_price = 0.f;
    float leverage = 0.f;
    float notional = 0.f;
    float isolated_margin = 0.f;
    std::string margin_type;        // cross / isolated
    std::string position_side;      // BOTH / LONG / SHORT
    long long update_time = 0;
    std::string raw_json;
};

class binance_interface
{
public:
    binance_interface();
    ~binance_interface();

    binance_interface(const binance_interface&) = delete;
    binance_interface& operator=(const binance_interface&) = delete;

    void init(std::string credential_path);
    void set_credentials(const std::string& api_key, const std::string& ed25519_private_key_pem);

    // instId.cfg：只读第 1 行，行情与下单同一 symbol
    static std::pair<std::string, std::string> parse_inst_id(const std::string& content);

    // 启动三路长连接线程：行情 / 余额 / 订单
    // 盘口只用 WS partial：@depth5/10/20@0ms（>20 钳成 20，不走 REST）
    void start(uint32_t depth_levels = 20);
    void stop();

    const std::string& signal_symbol() const { return signal_symbol_; }
    const std::string& trade_symbol() const { return trade_symbol_; }
    bool split_trade_market() const { return split_trade_market_; }

    // 读行情缓存（由行情线程持续更新；等待新快照）
    orderbook_info get_ws_orderbook(uint32_t data_amount = 20);
    // 买卖对象盘口（不阻塞；与行情对象相同时返回同一缓存）
    orderbook_info get_ws_trade_orderbook();
    // 最近一笔 aggTrade；trade_market=true 时读买卖对象
    agg_trade_info get_ws_last_agg_trade(bool trade_market = false);
    // 读资金费率缓存（markPrice 流；不阻塞等待新包）
    funding_info get_ws_funding_rate();
    // 读主动成交流（aggTrade 滚动窗口汇总）
    aggressor_flow get_ws_aggressor_flow(long long window_ms = 1000, bool trade_market = false);
    // 窗内逐笔（时间正序）。窗上限 30 分钟，供 CVD/VWAP/1s bar
    std::vector<agg_trade_info> get_ws_agg_trades(long long window_ms, bool trade_market = false);
    // 强平：@forceOrder（/market 路由）；window 上限 30 分钟
    force_order_info get_ws_last_force_order(bool trade_market = false);
    liquidation_flow get_ws_liquidation_flow(long long window_ms = 60000, bool trade_market = false);
    std::vector<force_order_info> get_ws_force_orders(long long window_ms, bool trade_market = false);
    // 持仓量：REST 轮询缓存。change = (OI_now - OI_t-W) / OI_t-W
    open_interest_info get_ws_open_interest(bool trade_market = false);
    float get_ws_open_interest_change(long long window_ms, bool trade_market = false);
    // 经余额线程长连接发起 account.status
    std::vector<asset_balance> get_ws_balance(bool omit_zero_balances = true);

    struct tick_filter
    {
        double tick_size = 0.01;
        int decimals = 2; // tickSize 去掉尾零后的小数位，例如 "0.10" → 1
    };

    // 查询交易对 tickSize（PRICE_FILTER）；symbol 为空时读 instId.cfg
    float get_tick_size(std::string symbol = {});
    tick_filter get_tick_filter(std::string symbol = {});

    // 经 USD-M 合约 WS API（ws-fapi）：下单 / 改单 / 查单 / 撤单 / 持仓
    order_info ws_place_order(const order_request& req);
    order_info ws_modify_order(long long order_id, const order_request& req);
    order_info ws_modify_order(const std::string& client_order_id, const order_request& req);
    order_info ws_get_order(long long order_id, std::string symbol = {});
    order_info ws_get_order(const std::string& client_order_id, std::string symbol = {});

    // USD-M 合约：未成交挂单（User Data Stream 维护缓存；不再热路径 REST 轮询）
    std::vector<order_info> ws_open_orders(std::string symbol = {});
    // USD-M 合约：持仓（account.position，带短缓存）；only_open=true 时过滤 |positionAmt|>0
    std::vector<position_info> ws_get_positions(std::string symbol = {}, bool only_open = true);
    // 热路径：只读本地缓存，绝不发起 account.position（由脏标记异步刷新）
    std::vector<position_info> ws_peek_positions(std::string symbol = {}, bool only_open = true);
    void refresh_positions_if_dirty(std::string symbol = {}, bool only_open = true);
    void invalidate_positions_cache();
    // User Data Stream ACCOUNT_UPDATE → 立刻改本地持仓缓存（避免 peek 仍空而同向再开仓）
    void apply_account_update(const std::string& msg);
    // USD-M 合约：取消单笔挂单（order.cancel）
    order_info ws_cancel_order(long long order_id, std::string symbol = {});
    order_info ws_cancel_order(const std::string& client_order_id, std::string symbol = {});
    // USD-M 合约：取消该交易对全部挂单（openOrders.cancelAll）；symbol 空则用买卖对象
    std::vector<order_info> ws_cancel_all_open_orders(std::string symbol = {});
    // 从本地挂单缓存移除（订单已成交/不存在时用）
    void forget_open_order(long long order_id);
    static bool is_missing_order_error(const std::string& message);
    static bool is_reduce_only_rejected(const std::string& message);
    static long long now_ms();

private:
    using param_map = std::map<std::string, std::string>;

    struct RpcChannel;
    struct MarketChannel;
    struct FundingChannel;
    struct AggTradeChannel;
    struct ForceOrderChannel;
    struct OpenInterestChannel;
    struct UserDataChannel;
    friend struct UserDataChannel;

    std::string api_key_;
    std::string private_key_pem_;
    bool use_testnet_ = false;
    std::string ws_api_host_ = "ws-api.binance.com";
    // USD-M 合约行情（现货为 data-stream.binance.vision）
    std::string ws_stream_host_ = "fstream.binance.com";
    // USD-M 合约 WebSocket API（挂单/持仓）
    std::string ws_fapi_host_ = "ws-fapi.binance.com";
    bool started_ = false;
    std::string signal_symbol_;
    std::string trade_symbol_;
    bool split_trade_market_ = false;

    std::unique_ptr<RpcChannel> balance_channel_;
    std::unique_ptr<RpcChannel> order_channel_;
    std::unique_ptr<RpcChannel> futures_channel_; // ws-fapi：下单 / 挂单 / 持仓
    std::unique_ptr<MarketChannel> market_channel_;
    std::unique_ptr<MarketChannel> trade_market_channel_;
    std::unique_ptr<FundingChannel> funding_channel_;
    std::unique_ptr<AggTradeChannel> agg_trade_channel_;
    std::unique_ptr<AggTradeChannel> trade_agg_trade_channel_;
    std::unique_ptr<ForceOrderChannel> force_order_channel_;
    std::unique_ptr<ForceOrderChannel> trade_force_order_channel_;
    std::unique_ptr<OpenInterestChannel> open_interest_channel_;
    std::unique_ptr<OpenInterestChannel> trade_open_interest_channel_;
    std::unique_ptr<UserDataChannel> user_data_channel_;

    order_info modify_order_ws(param_map cancel_id_params, order_request req);
    std::string ed25519_sign_base64(const std::string& message) const;
    std::string signed_ws_call(RpcChannel& channel, const std::string& method, param_map params) const;
    std::string signed_fapi_rest(const std::string& http_method, const std::string& path, param_map params) const;
    std::string resolve_symbol(std::string symbol) const;

    void upsert_open_order_cache(const order_info& order);
    void erase_open_order_cache(long long order_id);
    void replace_open_order_cache(std::vector<order_info> orders);
    std::vector<order_info> snapshot_open_order_cache(const std::string& symbol) const;
    void refresh_open_order_cache_rest(const std::string& symbol); // 仅启动时可选 bootstrap

    static std::vector<asset_balance> parse_balances(const std::string& json, bool omit_zero);
    static orderbook_info parse_orderbook(const std::string& json, const std::string& symbol);
    static funding_info parse_funding(const std::string& json, const std::string& symbol);
    static agg_trade_info parse_agg_trade(const std::string& json, const std::string& symbol);
    static force_order_info parse_force_order(const std::string& json, const std::string& symbol);
    static open_interest_info parse_open_interest(const std::string& json, const std::string& symbol);
    static order_info parse_order(const std::string& json);
    static std::vector<order_info> parse_open_orders(const std::string& json);
    static std::vector<position_info> parse_positions(const std::string& json, bool only_open);
    static order_info parse_user_stream_order(const std::string& order_obj);
    static std::string extract_result_object(const std::string& json);
    static void ensure_ws_ok(const std::string& json, const char* what);

    static bool is_terminal_order_status(const std::string& status);

    mutable std::mutex open_orders_mutex_;
    mutable std::vector<order_info> open_orders_cache_;
    mutable long long open_orders_cache_ms_ = 0;

    mutable std::mutex positions_mutex_;
    mutable std::vector<position_info> positions_cache_;
    mutable std::string positions_cache_symbol_;
    mutable bool positions_cache_only_open_ = true;
    mutable long long positions_cache_ms_ = 0;
    mutable bool positions_dirty_ = true;
    mutable bool positions_loaded_ = false;
    static constexpr long long kPositionsRefreshMs = 1000; // 非热路径刷新间隔
};

#endif //PROGRESSIV_BINANCE_INTERFACE_H
