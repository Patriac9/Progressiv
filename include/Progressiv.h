//
// Created by zagym on 08/08/2026.
//

#ifndef PROGRESSIV_PROGRESSIV_H
#define PROGRESSIV_PROGRESSIV_H
#include <exception>
#include <string>
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
    std::vector<orderbook_level>asks;
    std::vector<orderbook_level> bids;
    uint64_t current_tick;
    uint64_t second_tick;
    std::string message_time;
    std::string transaction_time;

    orderbook_script* script;

    [[nodiscard]] inline bool get_enable_live_action() const{return enable_live_execution;}

private:
    bool enable_training_capture = false;
    bool enable_live_execution = false;
    bool use_testnet = false;
    float trade_tick_size_ = 0.f;

    void load_app_cfg(const std::string& content);
    void capture_signal_sample(const std::string& day);
    void capture_trade_sample(const std::string& day, const std::string& signal_timestamp);
};
#endif //PROGRESSIV_PROGRESSIV_H
