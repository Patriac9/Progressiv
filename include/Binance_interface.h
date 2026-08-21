//
// Created by zagym on 09/08/2026.
//

#ifndef PROGRESSIV_BINANCE_INTERFACE_H
#define PROGRESSIV_BINANCE_INTERFACE_H

#include <cstdint>
#include <map>
#include <memory>
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

struct order_request
{
    std::string symbol;
    std::string side;
    std::string type;
    std::string time_in_force;
    std::string price;
    std::string quantity;
    std::string quote_order_qty;
    std::string client_order_id;
    bool reduce_only = false; // 合约只减仓
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

    // instId.cfg：第 1 行行情对象，第 2 行买卖对象；只有一行则两者相同
    static std::pair<std::string, std::string> parse_inst_id(const std::string& content);

    // 启动三路长连接线程：行情 / 余额 / 订单
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
    aggressor_flow get_ws_aggressor_flow(long long window_ms = 1000);
    // 经余额线程长连接发起 account.status
    std::vector<asset_balance> get_ws_balance(bool omit_zero_balances = true);

    // 查询交易对 tickSize（PRICE_FILTER）；symbol 为空时读 instId.cfg
    float get_tick_size(std::string symbol = {});

    // 经 USD-M 合约 WS API（ws-fapi）：下单 / 改单 / 查单 / 撤单 / 持仓
    order_info ws_place_order(const order_request& req);
    order_info ws_modify_order(long long order_id, const order_request& req);
    order_info ws_modify_order(const std::string& client_order_id, const order_request& req);
    order_info ws_get_order(long long order_id, std::string symbol = {});
    order_info ws_get_order(const std::string& client_order_id, std::string symbol = {});

    // USD-M 合约：未成交挂单（openOrders.status）；symbol 空则查全部
    std::vector<order_info> ws_open_orders(std::string symbol = {});
    // USD-M 合约：持仓（account.position）；only_open=true 时过滤 |positionAmt|>0
    std::vector<position_info> ws_get_positions(std::string symbol = {}, bool only_open = true);
    // USD-M 合约：取消单笔挂单（order.cancel）
    order_info ws_cancel_order(long long order_id, std::string symbol = {});
    order_info ws_cancel_order(const std::string& client_order_id, std::string symbol = {});
    // USD-M 合约：取消该交易对全部挂单（openOrders.cancelAll）；symbol 空则用买卖对象
    std::vector<order_info> ws_cancel_all_open_orders(std::string symbol = {});

private:
    using param_map = std::map<std::string, std::string>;

    struct RpcChannel;
    struct MarketChannel;
    struct FundingChannel;
    struct AggTradeChannel;

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

    order_info modify_order_ws(param_map cancel_id_params, order_request req);
    std::string ed25519_sign_base64(const std::string& message) const;
    std::string signed_ws_call(RpcChannel& channel, const std::string& method, param_map params) const;
    std::string resolve_symbol(std::string symbol) const;

    static std::vector<asset_balance> parse_balances(const std::string& json, bool omit_zero);
    static orderbook_info parse_orderbook(const std::string& json, const std::string& symbol);
    static funding_info parse_funding(const std::string& json, const std::string& symbol);
    static agg_trade_info parse_agg_trade(const std::string& json, const std::string& symbol);
    static order_info parse_order(const std::string& json);
    static std::vector<order_info> parse_open_orders(const std::string& json);
    static std::vector<position_info> parse_positions(const std::string& json, bool only_open);
    static std::string extract_result_object(const std::string& json);
    static void ensure_ws_ok(const std::string& json, const char* what);
    static long long now_ms();
};

#endif //PROGRESSIV_BINANCE_INTERFACE_H
