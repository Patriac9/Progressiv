//
// Created by zagym on 08/08/2026.
//

#ifndef PROGRESSIV_PROGRESSIV_H
#define PROGRESSIV_PROGRESSIV_H
#include <atomic>
#include <exception>
#include <string>
#include <thread>
#include "mft_script.h"
#include "Binance_interface.h"

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
    uint64_t current_tick = 0;
    std::string message_time;
    std::string transaction_time;
    mft_script* mft_script_ = nullptr;

    [[nodiscard]] inline bool get_enable_live_action() const { return enable_live_execution; }

private:
    bool enable_training_capture = false;
    bool enable_live_execution = false;
    bool use_testnet = false;

    std::atomic<bool> running_{false};
    std::thread data_thread_;

    void load_app_cfg(const std::string& content);
    void data_loop();
    void write_factors(const std::string& day, const std::string& signal_timestamp);

    long long last_factor_write_sec_ = -1;
};
#endif
