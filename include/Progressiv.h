//
// Created by zagym on 08/08/2026.
//

#ifndef PROGRESSIV_PROGRESSIV_H
#define PROGRESSIV_PROGRESSIV_H
#include <atomic>
#include <exception>
#include <string>
#include <thread>
#include "Binance_interface.h"

class orderbook_script;

class Progressiv
{
public:
    Progressiv();
    void init();
    void run();
    void destroy();
    ~Progressiv();

    binance_interface* interface_ = nullptr;
    orderbook_info current_orderbook;
    orderbook_info second_orderbook;
    std::vector<orderbook_level> asks;
    std::vector<orderbook_level> bids;
    uint64_t current_tick = 0;
    uint64_t second_tick = 0;
    std::string message_time;
    std::string transaction_time;

    orderbook_script* script = nullptr;

    [[nodiscard]] inline bool get_enable_live_action() const { return enable_live_execution; }

private:
    bool enable_training_capture = false;
    bool enable_live_execution = false;
    bool use_testnet = false;
    float trade_tick_size_ = 0.f;

    std::atomic<bool> running_{false};
    std::thread signal_thread_;
    std::thread exec_thread_;

    void load_app_cfg(const std::string& content);
    void signal_loop();
    void execution_loop();
    void capture_signal_sample(const std::string& day);
    void capture_trade_sample(const std::string& day, const std::string& signal_timestamp);
    void capture_lob_grid(const std::string& day, const orderbook_info& book);
    void flush_lob_bin();

    // DeepLOB：用 WS 20 档，100ms 网格、相对 mid 的 1/2/5/10bp 桶
    struct lob_acc
    {
        long long bin_ms = -1;
        float mid = 0.f;
        float vol_bid[4]{};
        float vol_ask[4]{};
        float ofi_bid[4]{};
        float ofi_ask[4]{};
        bool has_prev = false;
        std::string day;
        std::vector<orderbook_level> prev_bids;
        std::vector<orderbook_level> prev_asks;
    };
    lob_acc lob_{};
};
#endif
