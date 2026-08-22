//
// Created by zagym on 09/08/2026.
//

#include "Progressiv.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "Binance_interface.h"
#include "async_log.h"
#include "loader.h"
#include "script.h"
#include "orderbook_script.h"

namespace
{
    void trim_inplace(std::string& s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
            ++start;
        if (start > 0)
            s.erase(0, start);
    }

    bool parse_bool_value(std::string value)
    {
        trim_inplace(value);
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    void apply_app_cfg_line(std::string key, std::string value,
                            bool& enable_training_capture,
                            bool& enable_live_execution,
                            bool& use_testnet)
    {
        trim_inplace(key);
        trim_inplace(value);
        if (key == "enable_training_capture")
            enable_training_capture = parse_bool_value(value);
        else if (key == "enable_live_execution" || key == "enable_live_action")
            enable_live_execution = parse_bool_value(value);
        else if (key == "use_testnet")
            use_testnet = parse_bool_value(value);
    }

    std::string format_ms_timestamp(long long ms)
    {
        if (ms <= 0)
            return "-";

        using namespace std::chrono;
        const auto tp = system_clock::time_point{milliseconds{ms}};
        const std::time_t sec = system_clock::to_time_t(tp);
        const int millis = static_cast<int>(ms % 1000);

        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &sec);
#else
        localtime_r(&sec, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d%H%M%S") << '.'
            << std::setw(3) << std::setfill('0') << millis;
        return oss.str();
    }

    float sum_level_qty(const std::vector<orderbook_level>& levels, size_t n)
    {
        float sum = 0.f;
        const size_t count = std::min(n, levels.size());
        for (size_t i = 0; i < count; ++i)
            sum += levels[i].quantity;
        return sum;
    }

    void append_jsonl(const std::string& symbol, const std::string& day, const std::string& line)
    {
        if (symbol.empty() || day.empty() || day == "-")
            return;
        const std::string dir = "training_data/" + symbol;
        std::filesystem::create_directories(dir);
        async_log::instance().write_file(dir + "/" + day + ".jsonl", line);
    }

    void append_lob_jsonl(const std::string& symbol, const std::string& day, const std::string& line)
    {
        if (symbol.empty() || day.empty() || day == "-")
            return;
        const std::string dir = "training_data/" + symbol;
        std::filesystem::create_directories(dir);
        async_log::instance().write_file(dir + "/" + day + ".lob.jsonl", line);
    }

    void append_farr(std::ostringstream& oss, const char* key, const float* a, int n)
    {
        oss << '"' << key << "\":[";
        for (int i = 0; i < n; ++i)
        {
            if (i)
                oss << ',';
            oss << a[i];
        }
        oss << ']';
    }

    constexpr int kLobBuckets = 4;
    constexpr float kLobEdge[4] = {0.0001f, 0.0002f, 0.0005f, 0.001f};

    int lob_bucket(float px, float mid, bool is_ask)
    {
        if (!(mid > 0.f) || !(px > 0.f))
            return -1;
        const float d = is_ask ? (px - mid) / mid : (mid - px) / mid;
        if (d < 0.f)
            return 0;
        for (int i = 0; i < kLobBuckets; ++i)
        {
            if (d <= kLobEdge[i] + 1e-12f)
                return i;
        }
        return -1;
    }

    void lob_vol(const std::vector<orderbook_level>& bids,
                 const std::vector<orderbook_level>& asks,
                 float mid, float* vb, float* va)
    {
        for (int i = 0; i < kLobBuckets; ++i)
            vb[i] = va[i] = 0.f;
        for (const auto& lv : bids)
        {
            const int b = lob_bucket(lv.price, mid, false);
            if (b >= 0)
                vb[b] += lv.quantity;
        }
        for (const auto& lv : asks)
        {
            const int b = lob_bucket(lv.price, mid, true);
            if (b >= 0)
                va[b] += lv.quantity;
        }
    }

    void lob_ofi_side(const std::vector<orderbook_level>& prev,
                      const std::vector<orderbook_level>& cur,
                      float mid, bool is_ask, float* ofi)
    {
        std::map<long long, float> a;
        std::map<long long, float> b;
        for (const auto& lv : prev)
            a[std::llround(static_cast<double>(lv.price) * 1e8)] = lv.quantity;
        for (const auto& lv : cur)
            b[std::llround(static_cast<double>(lv.price) * 1e8)] = lv.quantity;

        auto acc = [&](long long key, float dqty) {
            if (dqty == 0.f)
                return;
            const float px = static_cast<float>(static_cast<double>(key) / 1e8);
            const int bucket = lob_bucket(px, mid, is_ask);
            if (bucket < 0)
                return;
            ofi[bucket] += is_ask ? -dqty : dqty;
        };

        for (const auto& kv : a)
        {
            const auto it = b.find(kv.first);
            acc(kv.first, (it == b.end() ? 0.f : it->second) - kv.second);
        }
        for (const auto& kv : b)
        {
            if (!a.count(kv.first))
                acc(kv.first, kv.second);
        }
    }
}

Progressiv::Progressiv()
{
    interface_ = new binance_interface();
    script = new orderbook_script();
}

void Progressiv::destroy()
{
    running_ = false;
    if (interface_)
        interface_->stop();
    if (signal_thread_.joinable())
        signal_thread_.join();
    if (exec_thread_.joinable())
        exec_thread_.join();
    if (enable_training_capture)
        flush_lob_bin();
    async_log::instance().stop();
}

Progressiv::~Progressiv()
{
    destroy();
    delete script;
    script = nullptr;
    delete interface_;
    interface_ = nullptr;
}

void Progressiv::load_app_cfg(const std::string& content)
{
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
        trim_inplace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        apply_app_cfg_line(line.substr(0, eq), line.substr(eq + 1),
                           enable_training_capture, enable_live_execution, use_testnet);
    }
}

void Progressiv::init()
{
    async_log::instance().start();
    load_app_cfg(loader::load_file("app.cfg"));
    if (use_testnet) interface_->init("demo_credential.cfg");
    else interface_->init("credential.cfg");
    interface_->start(20);
    script->set_controller(this);
    script->init(interface_->signal_symbol());
    if (interface_->split_trade_market())
        trade_tick_size_ = interface_->get_tick_size(interface_->trade_symbol());
    else
        trade_tick_size_ = 0.f;
    async_log::instance().info("Progressiv init OK");
}

void Progressiv::signal_loop()
{
    while (running_)
    {
        try
        {
            const auto wait_t0 = std::chrono::steady_clock::now();
            current_orderbook = interface_->get_ws_orderbook();
            const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_t0).count();
            if (!running_)
                break;

            const auto compute_t0 = std::chrono::steady_clock::now();
            current_tick = current_orderbook.last_update_id;
            std::string day;
            if (enable_training_capture)
            {
                transaction_time = format_ms_timestamp(current_orderbook.transact_time);
                message_time = format_ms_timestamp(current_orderbook.message_time);
                day = (message_time.size() >= 8) ? message_time.substr(0, 8) : std::string{};
                capture_lob_grid(day, current_orderbook);
            }
            // 直接 move，避免 asks/bids 二次拷贝；lob 必须在 move 之前写完
            script->set_asks(std::move(current_orderbook.asks));
            script->set_bids(std::move(current_orderbook.bids));
            script->run_signal();
            const auto compute_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - compute_t0).count();

            const long long exch_t = current_orderbook.transact_time;
            const long long recv_t = current_orderbook.message_time;
            const long long exch_lag_ms = (exch_t > 0 && recv_t > 0) ? (recv_t - exch_t) : -1;
            async_log::instance().set_loop_metrics(
                exch_lag_ms,
                static_cast<double>(wait_us) / 1000.0,
                static_cast<double>(compute_us) / 1000.0,
                script->latest_signal().T);

            if (enable_training_capture)
            {
                capture_signal_sample(day);
                if (interface_->split_trade_market())
                    capture_trade_sample(day, message_time);
            }
        }
        catch (const std::exception& e)
        {
            if (!running_)
                break;
            async_log::instance().error(std::string("signal loop: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void Progressiv::execution_loop()
{
    uint64_t last_seq = 0;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    while (running_)
    {
        try
        {
            signal_snapshot snap;
            if (!script->wait_take_signal(last_seq, snap, 50))
                continue;
            // 暂时不跑 run_execution：只算因子 / 落盘
        }
        catch (const std::exception& e)
        {
            async_log::instance().error(std::string("exec loop: ") + e.what());
            if (!running_)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

void Progressiv::run()
{
    running_ = true;
    async_log::instance().info("entering signal+exec threads");
    signal_thread_ = std::thread([this] { signal_loop(); });
    exec_thread_ = std::thread([this] { execution_loop(); });
    if (signal_thread_.joinable())
        signal_thread_.join();
    running_ = false;
    if (exec_thread_.joinable())
        exec_thread_.join();
    async_log::instance().stop();
}

void Progressiv::capture_signal_sample(const std::string& day)
{
    const auto& o = script->output_;
    std::ostringstream line;
    line << std::setprecision(9)
         << "{\"timestamp\":\"" << message_time << "\""
         << ",\"transaction_time\":\"" << transaction_time << "\""
         << ",\"tick\":" << current_tick
         << ",\"obi\":" << o.obi
         << ",\"mid_price\":" << o.mid_price
         << ",\"ml_ofi_5s\":" << o.ml_ofi_5s
         << ",\"ml_ofi_15s\":" << o.ml_ofi_15s
         << ",\"ml_ofi_30s\":" << o.ml_ofi_30s
         << ",\"ml_ofi_60s\":" << o.ml_ofi_60s
         << ",\"n_mid_moves_30s\":" << o.n_mid_moves_30s
         << ",\"n_mid_moves_60s\":" << o.n_mid_moves_60s
         << ",\"rv_30s\":" << o.rv_30s
         << ",\"rv_60s\":" << o.rv_60s
         << ",\"aggressor_imb_5s\":" << o.aggressor_imb_5s
         << ",\"d_ask_10bp\":" << o.d_ask_10bp
         << ",\"d_bid_10bp\":" << o.d_bid_10bp
         << ",\"d_imb_10bp\":" << o.d_imb_10bp
         << ",\"cover_ask_10bp\":" << o.cover_ask_10bp
         << ",\"cover_bid_10bp\":" << o.cover_bid_10bp
         << ",\"r_ask\":" << o.r_ask
         << ",\"r_bid\":" << o.r_bid
         << ",\"t_clear_ask\":" << o.t_clear_ask
         << ",\"t_clear_bid\":" << o.t_clear_bid
         << ",\"t_clear_ask_h\":" << o.t_clear_ask_h
         << ",\"t_clear_bid_h\":" << o.t_clear_bid_h
         << ",\"d_ask_chg_1s\":" << o.d_ask_chg_1s
         << ",\"d_bid_chg_1s\":" << o.d_bid_chg_1s
         << ",\"gap_ask\":" << o.gap_ask
         << ",\"gap_bid\":" << o.gap_bid
         << ",\"gap_max_ask\":" << o.gap_max_ask
         << ",\"gap_max_bid\":" << o.gap_max_bid
         << ",\"gap_imb\":" << o.gap_imb
         << ",\"bid_slope\":" << o.bid_slope
         << ",\"ask_slope\":" << o.ask_slope
         << ",\"slope_imb\":" << o.slope_imb
         << ",\"refill_ask_50ms\":" << o.refill_ask_50ms
         << ",\"refill_ask_100ms\":" << o.refill_ask_100ms
         << ",\"refill_ask_250ms\":" << o.refill_ask_250ms
         << ",\"refill_bid_50ms\":" << o.refill_bid_50ms
         << ",\"refill_bid_100ms\":" << o.refill_bid_100ms
         << ",\"refill_bid_250ms\":" << o.refill_bid_250ms
         << ",\"hawkes_buy_mo\":" << o.hawkes_buy_mo
         << ",\"hawkes_sell_mo\":" << o.hawkes_sell_mo
         << ",\"hawkes_ask_add\":" << o.hawkes_ask_add
         << ",\"hawkes_bid_add\":" << o.hawkes_bid_add
         << ",\"hawkes_ask_cancel\":" << o.hawkes_ask_cancel
         << ",\"hawkes_bid_cancel\":" << o.hawkes_bid_cancel
         << ",\"ml_ofi_250ms\":" << o.ml_ofi_250ms
         << ",\"ml_ofi_1s\":" << o.ml_ofi_1s
         << ",\"ml_ofi_2s\":" << o.ml_ofi_2s
         << ",\"ofi_tick\":" << o.ofi_tick
         << ",\"l1_ofi_tick\":" << o.l1_ofi_tick
         << ",\"ret_100ms\":" << o.ret_100ms
         << ",\"ret_250ms\":" << o.ret_250ms
         << ",\"ret_1s\":" << o.ret_1s
         << ",\"ret_5s\":" << o.ret_5s
         << ",\"aggressor_imb_250ms\":" << o.aggressor_imb_250ms
         << ",\"aggressor_imb_1s\":" << o.aggressor_imb_1s
         << ",\"aggressor_net_1s\":" << o.aggressor_net_1s
         << ",\"last_trade_sign\":" << o.last_trade_sign
         << ",\"last_trade_mid_bps\":" << o.last_trade_mid_bps
         << ",\"last_trade_age_ms\":" << o.last_trade_age_ms
         << ",\"sig_l1_imb\":" << o.sig_l1_imb
         << ",\"sig_l3_imb\":" << o.sig_l3_imb
         << ",\"sig_micro_off\":" << o.sig_micro_off
         << ",\"sig_spread_ticks\":" << o.sig_spread_ticks
         << ",\"hawkes_mo_imb\":" << o.hawkes_mo_imb
         << ",\"hawkes_add_imb\":" << o.hawkes_add_imb
         << ",\"hawkes_cancel_imb\":" << o.hawkes_cancel_imb
         << ",\"r_imb\":" << o.r_imb
         << ",\"d_chg_imb_1s\":" << o.d_chg_imb_1s
         << ",\"refill_imb_100ms\":" << o.refill_imb_100ms
         << ",\"cover_imb_10bp\":" << o.cover_imb_10bp
         << ",\"exec_aggressor_imb_250ms\":" << o.exec_aggressor_imb_250ms
         << ",\"exec_aggressor_imb_1s\":" << o.exec_aggressor_imb_1s
         << ",\"basis\":" << o.basis
         << ",\"basis_chg_1s\":" << o.basis_chg_1s
         << ",\"T\":" << o.T
         << ",\"alpha\":" << o.alpha
         << "}\n";
    append_jsonl(interface_->signal_symbol(), day, line.str());
}

void Progressiv::capture_trade_sample(const std::string& day, const std::string& signal_timestamp)
{
    try
    {
        second_orderbook = interface_->get_ws_trade_orderbook();
    }
    catch (const std::exception& e)
    {
        async_log::instance().error(std::string("trade orderbook not ready: ") + e.what());
        return;
    }

    second_tick = second_orderbook.last_update_id;
    const auto& trade_bids = second_orderbook.bids;
    const auto& trade_asks = second_orderbook.asks;
    if (trade_bids.empty() || trade_asks.empty())
        return;

    const float bid = trade_bids[0].price;
    const float ask = trade_asks[0].price;
    const float bid_qty = trade_bids[0].quantity;
    const float ask_qty = trade_asks[0].quantity;
    const float mid = 0.5f * (bid + ask);
    const float tick_size = trade_tick_size_ > 0.f ? trade_tick_size_ : 0.01f;
    const float spread_ticks = tick_size > 0.f ? (ask - bid) / tick_size : 0.f;
    const agg_trade_info last_trade = interface_->get_ws_last_agg_trade(true);
    const bool has_trade = last_trade.price > 0.f && last_trade.quantity > 0.f;

    std::ostringstream line;
    line << std::setprecision(9)
         << "{\"timestamp\":\"" << format_ms_timestamp(second_orderbook.message_time) << "\""
         << ",\"transaction_time\":\"" << format_ms_timestamp(second_orderbook.transact_time) << "\""
         << ",\"signal_timestamp\":\"" << signal_timestamp << "\""
         << ",\"tick\":" << second_tick
         << ",\"tick_size\":" << tick_size
         << ",\"bid\":" << bid
         << ",\"ask\":" << ask
         << ",\"bid_qty\":" << bid_qty
         << ",\"ask_qty\":" << ask_qty
         << ",\"mid_price\":" << mid
         << ",\"spread_ticks\":" << spread_ticks
         << ",\"bid_qty_5\":" << sum_level_qty(trade_bids, 5)
         << ",\"ask_qty_5\":" << sum_level_qty(trade_asks, 5)
         << ",\"bid_qty_10\":" << sum_level_qty(trade_bids, 10)
         << ",\"ask_qty_10\":" << sum_level_qty(trade_asks, 10)
         << ",\"has_trade\":" << (has_trade ? "true" : "false");
    if (has_trade)
    {
        line << ",\"trade_price\":" << last_trade.price
             << ",\"trade_qty\":" << last_trade.quantity
             << ",\"trade_buyer_is_maker\":" << (last_trade.buyer_is_maker ? "true" : "false")
             << ",\"trade_time\":\"" << format_ms_timestamp(last_trade.trade_time) << "\"";
    }
    line << "}\n";
    append_jsonl(interface_->trade_symbol(), day, line.str());
}

void Progressiv::flush_lob_bin()
{
    if (lob_.bin_ms < 0 || lob_.day.empty() || !interface_)
        return;
    std::ostringstream line;
    line << std::setprecision(9)
         << "{\"timestamp\":\"" << format_ms_timestamp(lob_.bin_ms) << "\""
         << ",\"t_unix_ms\":" << lob_.bin_ms
         << ",\"grid_ms\":100"
         << ",\"theta\":0.001"
         << ",\"mid\":" << lob_.mid
         << ",\"bucket_bp\":[1,2,5,10],";
    append_farr(line, "vol_bid", lob_.vol_bid, kLobBuckets);
    line << ',';
    append_farr(line, "vol_ask", lob_.vol_ask, kLobBuckets);
    line << ',';
    append_farr(line, "ofi_bid", lob_.ofi_bid, kLobBuckets);
    line << ',';
    append_farr(line, "ofi_ask", lob_.ofi_ask, kLobBuckets);
    line << "}\n";
    append_lob_jsonl(interface_->signal_symbol(), lob_.day, line.str());
}

void Progressiv::capture_lob_grid(const std::string& day, const orderbook_info& book)
{
    if (book.bids.empty() || book.asks.empty() || day.empty())
        return;
    const float mid = 0.5f * (book.bids[0].price + book.asks[0].price);
    const long long now = book.message_time > 0 ? book.message_time : book.transact_time;
    if (now <= 0 || !(mid > 0.f))
        return;

    constexpr long long kGrid = 100;
    const long long bin = (now / kGrid) * kGrid;

    float d_ofi_b[kLobBuckets]{};
    float d_ofi_a[kLobBuckets]{};
    if (lob_.has_prev)
    {
        lob_ofi_side(lob_.prev_bids, book.bids, mid, false, d_ofi_b);
        lob_ofi_side(lob_.prev_asks, book.asks, mid, true, d_ofi_a);
    }

    if (lob_.bin_ms >= 0 && bin != lob_.bin_ms)
    {
        flush_lob_bin();
        for (int i = 0; i < kLobBuckets; ++i)
            lob_.ofi_bid[i] = lob_.ofi_ask[i] = 0.f;
    }

    lob_.bin_ms = bin;
    lob_.day = day;
    lob_.mid = mid;
    for (int i = 0; i < kLobBuckets; ++i)
    {
        lob_.ofi_bid[i] += d_ofi_b[i];
        lob_.ofi_ask[i] += d_ofi_a[i];
    }
    lob_vol(book.bids, book.asks, mid, lob_.vol_bid, lob_.vol_ask);
    lob_.prev_bids = book.bids;
    lob_.prev_asks = book.asks;
    lob_.has_prev = true;
}

