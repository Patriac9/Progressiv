//
// Created by zagym on 09/08/2026.
//

#include "Progressiv.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
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
    interface_->start(30);
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
            transaction_time = format_ms_timestamp(current_orderbook.transact_time);
            message_time = format_ms_timestamp(current_orderbook.message_time);
            current_tick = current_orderbook.last_update_id;
            asks = current_orderbook.asks;
            bids = current_orderbook.bids;
            script->set_asks(asks);
            script->set_bids(bids);
            script->run_signal();
            const auto compute_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - compute_t0).count();

            const long long exch_t = current_orderbook.transact_time;
            const long long recv_t = current_orderbook.message_time;
            const long long exch_lag_ms = (exch_t > 0 && recv_t > 0) ? (recv_t - exch_t) : -1;
            async_log::instance().set_loop_metrics(
                exch_lag_ms,
                static_cast<double>(wait_us) / 1000.0,
                static_cast<double>(compute_us) / 1000.0);

            if (enable_training_capture)
            {
                const std::string day = (message_time.size() >= 8) ? message_time.substr(0, 8) : std::string{};
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
    while (running_)
    {
        try
        {
            if (!script->wait_for_signal(last_seq, 200))
                continue;
            script->run_execution();
        }
        catch (const std::exception& e)
        {
            async_log::instance().error(std::string("exec loop: ") + e.what());
            if (!running_)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
         << ",\"n_mid_moves_30s\":" << o.n_mid_moves_30s
         << ",\"n_mid_moves_60s\":" << o.n_mid_moves_60s
         << ",\"rv_30s\":" << o.rv_30s
         << ",\"rv_60s\":" << o.rv_60s
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
