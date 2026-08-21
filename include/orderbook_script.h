//
// Created by zagym on 09/08/2026.
//
#include "script.h"

#ifndef PROGRESSIV_ORDERBOOK_SCRIPT_H
#define PROGRESSIV_ORDERBOOK_SCRIPT_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

typedef struct output
{
    float obi;
    float mid_price;
    float ml_ofi_1s;
    float ml_ofi_5s;
    float ml_ofi_15s;
    float ml_ofi_30s;
    float ml_ofi_60s;
    float z_ml_ofi_1s;
    float z_ml_ofi_5s;
    float z_ml_ofi_15s;
    float z_ml_ofi_30s;
    float z_ml_ofi_60s;
    float aggressor_imb_1s;
    float aggressor_imb_5s;
    float spread_ticks;
    float bid_gap_ticks;
    float ask_gap_ticks;
    float gap_imb;      // (ask_gap - bid_gap) / (sum+1)，ask 缺口多为正
    float bid_slope;
    float ask_slope;
    float slope_imb;    // (bid_slope - ask_slope) / (|b|+|a|+eps)，买盘更厚为正
    // Cont 冲击系数 λ: Δmid ≈ λ · OFI（无截距 OLS）
    float impact_ofi_1s;
    float impact_ofi_5s;
    float impact_ofi_15s;
    float impact_ofi_30s;
    float impact_ofi_60s;
    // Kyle λ: Δmid ≈ λ · aggressor_net（同窗口）
    float impact_trade_1s;
    float impact_trade_5s;
    float impact_trade_15s;
    float impact_trade_30s;
    float impact_trade_60s;
    // 过去窗口 mid 变动次数、实现波动 RV = sqrt(Σ(Δmid)^2)
    float n_mid_moves_30s;
    float n_mid_moves_60s;
    float rv_30s;
    float rv_60s;
} output;


// 本 tick 实时因子 x，不是模型系数
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

// T_Param / alpha_Param 文件里的模型系数 w
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
    bool close_flag = false;   // horizon / flip：追反方向一档平仓
    bool sl_hit = false;       // 触及对称止损，挂在 sl 价
    float time = 0.f;          // 建仓时刻（秒）
    float direction = 0.f;     // +1 多，-1 空
    float tp_offset = 0.f;     // 开仓时锁定的止盈/止损距离
};

class orderbook_script : public script
{
public:
    orderbook_script();
    ~orderbook_script();
    void init(std::string instId) override;
    void run() override;
    void destroy() override;

    output output_;
private:
    static constexpr int kImbBins = 10;      // imbalance 分桶数
    static constexpr int kHorizon = 6;       // Stoikov 级数截断阶数
    static constexpr float kMidEps = 1e-8f;  // 判定 mid 未变的阈值
    float horizon = 13.f; // 可由 model/param.mod 覆盖
    bool enable_dynamic_risk_management = true;
    float tp_offset = 0.08f; // 静态止盈距离；可由 param.mod 覆盖
    float q_ord = 600.f; // USDC 名义；可由 param.mod 覆盖
    float tau = 0.407647f; // 从 T_Param.tau 加载，缺省回退
    position_manager position_;

    float tick_size = 0.f;
    float trade_tick_size_ = 0.f;
    int trade_price_decimals_ = 2;
    uint32_t depth = 15;
    float obi = 0.f;
    float weighed_obi = 0.f;
    float mid_price = 0.f;
    float weighed_mid_price = 0.f;
    float microprice = 0.f;
    float micro_price_offset;

    float ofi_l1 = 0.f;
    static constexpr int kMlOfiLevels = 10;   // multilevel OFI 只用前 10 档
    static constexpr long long kMlOfiMaxMs = 60000;  // 保留至 60s 窗口
    static constexpr long long kZOfiWindowMs = 60000; // z-score 需滚动满 1min
    std::vector<float> ml_ofi;               // 最新在前端
    std::vector<long long> ml_ofi_time_ms_;  // 与 ml_ofi 对齐的毫秒时间戳
    long long ml_ofi_start_ms_ = 0;          // 首次采样时间（冷启动用）
    float ml_ofi_1s = 0.f;
    float ml_ofi_5s = 0.f;
    float ml_ofi_15s = 0.f;
    float ml_ofi_30s = 0.f;
    float ml_ofi_60s = 0.f;

    std::vector<long long> z_ofi_time_ms_;
    long long z_ofi_start_ms_ = 0;           // z-score 首次采样时间
    std::vector<float> z_hist_1s_;
    std::vector<float> z_hist_5s_;
    std::vector<float> z_hist_15s_;
    std::vector<float> z_hist_30s_;
    std::vector<float> z_hist_60s_;
    float z_ml_ofi_1s = 0.f;
    float z_ml_ofi_5s = 0.f;
    float z_ml_ofi_15s = 0.f;
    float z_ml_ofi_30s = 0.f;
    float z_ml_ofi_60s = 0.f;
    float aggressor_buy_1s = 0.f;
    float aggressor_sell_1s = 0.f;
    float aggressor_net_1s = 0.f;
    float aggressor_imb_1s = 0.f;
    float aggressor_imb_5s = 0.f;
    float spread_ticks = 0.f;
    float bid_gap_ticks = 0.f;
    float ask_gap_ticks = 0.f;
    float gap_imb = 0.f;
    float bid_slope = 0.f;
    float ask_slope = 0.f;
    float slope_imb = 0.f;

    // Cont / Kyle 冲击系数
    static constexpr long long kImpactMaxMs = 60000;
    long long impact_start_ms_ = 0;
    std::vector<long long> impact_time_ms_;
    std::vector<float> impact_ofi_x_;   // Cont: ofi_l1
    std::vector<float> impact_dmid_;    // mid_t - mid_{t-1}
    std::vector<long long> mid_hist_time_ms_;
    std::vector<float> mid_hist_;
    float impact_ofi_1s = 0.f;
    float impact_ofi_5s = 0.f;
    float impact_ofi_15s = 0.f;
    float impact_ofi_30s = 0.f;
    float impact_ofi_60s = 0.f;
    float impact_trade_1s = 0.f;
    float impact_trade_5s = 0.f;
    float impact_trade_15s = 0.f;
    float impact_trade_30s = 0.f;
    float impact_trade_60s = 0.f;

    long long mid_hist_start_ms_ = 0;
    float n_mid_moves_30s = 0.f;
    float n_mid_moves_60s = 0.f;
    float rv_30s = 0.f;
    float rv_60s = 0.f;

    // Stoikov 在线估计：状态 = imbalance 分桶
    bool has_prev_ = false;
    float prev_mid_ = 0.f;
    int prev_imb_bin_ = 0;
    float prev_bid_quantity =0.f;
    float prev_ask_quantity = 0.f;
    std::vector<orderbook_level> prev_asks;
    std::vector<orderbook_level> prev_bids;


    std::array<std::array<double, kImbBins>, kImbBins> q_count_{};   // mid 不变的转移
    std::array<std::array<double, kImbBins>, kImbBins> r2_count_{};  // mid 变化后的 imbalance 转移
    std::array<double, kImbBins> absorb_dm_sum_{};                   // 从该状态触发 mid 变化的 ΔM 累计
    std::array<double, kImbBins> row_total_{};                       // 各状态出发次数

    std::array<double, kImbBins> g1_{};
    std::array<double, kImbBins> adjustment_{};
    bool model_ready_ = false;

    float predicted_movement = 0.f;
    float alpha = 0.f;
    T_coef t_coef_{};
    alpha_coef alpha_coef_{};

    static int imb_bin(float imbalance);
    void compute_book_shape();
    void observe_transition(int from, int to, float d_mid);
    void recompute_stoikov_model();
    static bool solve_linear(std::array<std::array<double, kImbBins>, kImbBins> a,
                             std::array<double, kImbBins> b,
                             std::array<double, kImbBins>& x);
    void load_model_params();
    float T(const T_parameter& param_);
    float calculate_alpha(const alpha_parameter& param_);

};

#endif //PROGRESSIV_ORDERBOOK_SCRIPT_H
