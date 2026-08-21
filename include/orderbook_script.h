//
// Created by zagym on 09/08/2026.
//
#include "script.h"

#ifndef PROGRESSIV_ORDERBOOK_SCRIPT_H
#define PROGRESSIV_ORDERBOOK_SCRIPT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 训练落盘 / 观测用的精简输出（仅 T/alpha 实际用到的因子）
typedef struct output
{
    float obi = 0.f;
    float mid_price = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;
    float T = 0.f;
    float alpha = 0.f;
} output;

struct T_parameter
{
    float obi = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
};

struct alpha_parameter
{
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;
    float abs_T = 0.f;
};

struct T_coef
{
    float intercept = 0.f;
    float obi = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
};

struct alpha_coef
{
    bool is_exp = false;
    float intercept = 0.f;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;
    float abs_T = 0.f;
    float scale = 1.f;
    float clip_min = 0.f;
    float clip_max = 1.e9f;
};

struct position_manager
{
    bool close_flag = false;
    bool sl_hit = false;
    float time = 0.f;
    float direction = 0.f;
    float tp_offset = 0.f;
    int empty_pos_streak = 0;
};

// 信号线程 → 执行线程 的无锁拷贝快照
struct signal_snapshot
{
    uint64_t seq = 0;
    uint64_t tick = 0;
    long long message_time_ms = 0;
    long long transact_time_ms = 0;
    float T = 0.f;
    float alpha = 0.f;
    float tau = 0.f;
    float horizon = 0.f;
    float tp_offset = 0.f;
    float q_ord = 0.f;
    bool enable_dynamic_risk_management = true;
    output features{};
};

class orderbook_script : public script
{
public:
    orderbook_script();
    ~orderbook_script();
    void init(std::string instId) override;
    void run() override; // 兼容：仅跑信号（单线程调试用）
    void destroy() override;

    void run_signal();     // 信号线程：算因子 / T / alpha
    void run_execution();  // 执行线程：下单 / 平仓

    signal_snapshot latest_signal() const;
    uint64_t signal_seq() const { return signal_seq_.load(); }
    // 执行线程：等到新信号或超时；返回是否有新 seq
    bool wait_for_signal(uint64_t& last_seq, int timeout_ms);

    output output_;

private:
    static constexpr float kMidEps = 1e-8f;
    static constexpr int kMlOfiLevels = 10;
    static constexpr long long kMlOfiMaxMs = 15000; // T 只用到 15s
    static constexpr long long kVolMaxMs = 60000;

    float horizon = 13.f;
    bool enable_dynamic_risk_management = true;
    float tp_offset = 0.08f;
    float q_ord = 600.f;
    float tau = 0.407647f;
    position_manager position_;

    float tick_size = 0.f;
    float trade_tick_size_ = 0.f;
    int trade_price_decimals_ = 2;
    uint32_t depth = 15;

    float obi = 0.f;
    float mid_price = 0.f;

    std::vector<float> ml_ofi;
    std::vector<long long> ml_ofi_time_ms_;
    long long ml_ofi_start_ms_ = 0;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;

    std::vector<long long> mid_hist_time_ms_;
    std::vector<float> mid_hist_;
    long long mid_hist_start_ms_ = 0;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;

    bool has_prev_ = false;
    float prev_bid_quantity = 0.f;
    float prev_ask_quantity = 0.f;
    std::vector<orderbook_level> prev_asks;
    std::vector<orderbook_level> prev_bids;

    float predicted_movement = 0.f;
    float alpha = 0.f;
    T_coef t_coef_{};
    alpha_coef alpha_coef_{};

    mutable std::mutex signal_mu_;
    mutable std::condition_variable signal_cv_;
    signal_snapshot published_{};
    std::atomic<uint64_t> signal_seq_{0};

    void publish_signal(uint64_t tick, long long msg_ms, long long txn_ms);
    void load_model_params();
    float T(const T_parameter& param_) const;
    float calculate_alpha(const alpha_parameter& param_) const;
};

#endif
