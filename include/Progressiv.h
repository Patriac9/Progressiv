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
};
#endif
