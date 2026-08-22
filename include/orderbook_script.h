//
// Created by zagym on 09/08/2026.
//
#include "script.h"

#ifndef PROGRESSIV_ORDERBOOK_SCRIPT_H
#define PROGRESSIV_ORDERBOOK_SCRIPT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// 训练落盘：T/alpha 因子 + 相对 mid ±0.1% 障碍微观结构（字段名仍带 _10bp，与 10bp 同义）
typedef struct output
{
    float obi = 0.f;
    float mid_price = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
    float ml_ofi_30s = 0.f;
    float ml_ofi_60s = 0.f;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;
    float aggressor_imb_5s = 0.f;
    float d_ask_10bp = 0.f;
    float d_bid_10bp = 0.f;
    float d_imb_10bp = 0.f;
    float cover_ask_10bp = 0.f;
    float cover_bid_10bp = 0.f;
    float r_ask = 0.f;
    float r_bid = 0.f;
    float t_clear_ask = 0.f;
    float t_clear_bid = 0.f;
    float t_clear_ask_h = 0.f;
    float t_clear_bid_h = 0.f;
    float d_ask_chg_1s = 0.f;
    float d_bid_chg_1s = 0.f;
    float gap_ask = 0.f;
    float gap_bid = 0.f;
    float gap_max_ask = 0.f;
    float gap_max_bid = 0.f;
    float gap_imb = 0.f;
    float bid_slope = 0.f;
    float ask_slope = 0.f;
    float slope_imb = 0.f;
    float refill_ask_50ms = 0.f;
    float refill_ask_100ms = 0.f;
    float refill_ask_250ms = 0.f;
    float refill_bid_50ms = 0.f;
    float refill_bid_100ms = 0.f;
    float refill_bid_250ms = 0.f;
    float hawkes_buy_mo = 0.f;
    float hawkes_sell_mo = 0.f;
    float hawkes_ask_add = 0.f;
    float hawkes_bid_add = 0.f;
    float hawkes_ask_cancel = 0.f;
    float hawkes_bid_cancel = 0.f;
    // dir：短窗 / 带符号微观结构
    float ml_ofi_250ms = 0.f;
    float ml_ofi_1s = 0.f;
    float ml_ofi_2s = 0.f;
    float ofi_tick = 0.f;
    float l1_ofi_tick = 0.f;
    float ret_100ms = 0.f;
    float ret_250ms = 0.f;
    float ret_1s = 0.f;
    float ret_5s = 0.f;
    float aggressor_imb_250ms = 0.f;
    float aggressor_imb_1s = 0.f;
    float aggressor_net_1s = 0.f;
    float last_trade_sign = 0.f;
    float last_trade_mid_bps = 0.f;
    float last_trade_age_ms = 0.f;
    float sig_l1_imb = 0.f;
    float sig_l3_imb = 0.f;
    float sig_micro_off = 0.f;
    float sig_spread_ticks = 0.f;
    float hawkes_mo_imb = 0.f;
    float hawkes_add_imb = 0.f;
    float hawkes_cancel_imb = 0.f;
    float r_imb = 0.f;
    float d_chg_imb_1s = 0.f;
    float refill_imb_100ms = 0.f;
    float cover_imb_10bp = 0.f;
    float exec_aggressor_imb_250ms = 0.f;
    float exec_aggressor_imb_1s = 0.f;
    float basis = 0.f;
    float basis_chg_1s = 0.f;
    float T = 0.f;
    float alpha = 0.f;
} output;

struct T_parameter
{
    float obi = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
    float ml_ofi_30s = 0.f;
    float ml_ofi_60s = 0.f;
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
    float ml_ofi_30s = 0.f;
    float ml_ofi_60s = 0.f;
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
    float open_price = 0.f; // 开仓锁定均价；TP/SL = open ± dir*tp_offset
    float tp_offset = 0.f;
    int empty_pos_streak = 0;
    bool awaiting_entry = false; // 已挂开仓单、尚未确认持仓：禁止再开同向
    long long reduce_reject_until_ms = 0; // -2022 后短冷却
    const char* exit_reason = nullptr; // tp / sl / horizon / flip
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
    void run_execution(const signal_snapshot& snap);

    signal_snapshot latest_signal() const;
    uint64_t signal_seq() const { return signal_seq_.load(std::memory_order_acquire); }
    // 自旋 + cv；一次拿到快照，避免二次加锁
    bool wait_take_signal(uint64_t& last_seq, signal_snapshot& out, int timeout_ms);

    output output_;

private:
    static constexpr float kMidEps = 1e-8f;
    static constexpr float kBarrierTheta = 0.001f; // 相对 mid ±0.1%（=10bp）
    static constexpr float kHawkesBeta = 1.f;      // 指数核 1/s
    static constexpr int kMlOfiLevels = 10;
    static constexpr long long kMlOfiMaxMs = 60000;
    static constexpr long long kVolMaxMs = 60000;
    static constexpr long long kFlowKeepMs = 5000;
    static constexpr long long kDepleteMs = 1000;

    struct FlowSample
    {
        long long t_ms = 0;
        float d_ask = 0.f;
        float d_bid = 0.f;
        float ask_net = 0.f;
        float bid_net = 0.f;
    };

    struct ClearEvent
    {
        long long t_ms = 0;
        float price = 0.f;
        float qty_before = 0.f;
        bool is_ask = true;
        unsigned mask = 0; // bit0=50ms bit1=100ms bit2=250ms
    };

    float horizon = 13.f;
    bool enable_dynamic_risk_management = true;
    float tp_offset = 0.08f;
    float q_ord = 600.f;
    float tau = 0.407647f;
    position_manager position_;

    float tick_size = 0.f;
    float trade_tick_size_ = 0.f;
    int trade_price_decimals_ = 2;
    uint32_t depth = 20; // 与 WS @depth20 一致；MLOFI 仍只用前 10 档

    float obi = 0.f;
    float mid_price = 0.f;

    std::deque<float> ml_ofi;
    std::deque<long long> ml_ofi_time_ms_;
    long long ml_ofi_start_ms_ = 0;
    float ml_ofi_250ms = 0.f;
    float ml_ofi_1s = 0.f;
    float ml_ofi_2s = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
    float ml_ofi_30s = 0.f;
    float ml_ofi_60s = 0.f;

    std::deque<long long> mid_hist_time_ms_;
    std::deque<float> mid_hist_;
    long long mid_hist_start_ms_ = 0;
    std::deque<long long> exec_mid_time_ms_;
    std::deque<float> exec_mid_hist_;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;

    bool has_prev_ = false;
    std::vector<orderbook_level> prev_asks;
    std::vector<orderbook_level> prev_bids;

    long long feat_time_ms_ = 0;
    std::deque<FlowSample> flow_hist_;
    std::deque<ClearEvent> clears_;
    float hawkes_buy_mo_ = 0.f;
    float hawkes_sell_mo_ = 0.f;
    float hawkes_ask_add_ = 0.f;
    float hawkes_bid_add_ = 0.f;
    float hawkes_ask_cancel_ = 0.f;
    float hawkes_bid_cancel_ = 0.f;
    float refill_ask_50_ = 0.f;
    float refill_ask_100_ = 0.f;
    float refill_ask_250_ = 0.f;
    float refill_bid_50_ = 0.f;
    float refill_bid_100_ = 0.f;
    float refill_bid_250_ = 0.f;

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
    void update_ml_ofi_windows(long long now_ms);
    void update_barrier_features(long long now_ms);
    void update_dir_micro_features(long long now_ms, float ofi_tick, float l1_ofi_tick);
    float T(const T_parameter& param_) const;
    float calculate_alpha(const alpha_parameter& param_) const;
};

#endif
