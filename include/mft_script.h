//
// Created by zagym on 22/08/2026.
//

#ifndef PROGRESSIV_MFT_SCRIPT_H
#define PROGRESSIV_MFT_SCRIPT_H

#include "script.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

typedef struct output
{
    long long t_unix_ms = 0;
    uint64_t tick = 0;
    float bid = 0.f;
    float ask = 0.f;
    float mid = 0.f;
    float bid_qty = 0.f;
    float ask_qty = 0.f;
    float mark = 0.f;
    float index = 0.f;
    float oi = 0.f;

    float rv_15s = 0.f, rv_30s = 0.f, rv_1m = 0.f, rv_2m = 0.f;
    float rv_5m = 0.f, rv_15m = 0.f, rv_30m = 0.f;
    float sigma_15s = 0.f, sigma_30s = 0.f, sigma_1m = 0.f, sigma_2m = 0.f;
    float sigma_5m = 0.f, sigma_15m = 0.f, sigma_30m = 0.f;
    float z_tp_60 = 0.f, z_sl_60 = 0.f;
    float z_tp_180 = 0.f, z_sl_180 = 0.f;
    float z_tp_300 = 0.f, z_sl_300 = 0.f;
    float rs_plus_1m = 0.f, rs_minus_1m = 0.f, rs_imb_1m = 0.f;
    float rs_plus_5m = 0.f, rs_minus_5m = 0.f, rs_imb_5m = 0.f;
    float hl_1m = 0.f, hl_5m = 0.f, hl_15m = 0.f;
    float vvol_15m = 0.f, vvol_30m = 0.f;
    float vol_q = 0.f;
    float drv = 0.f;
    float r_5s = 0.f, r_15s = 0.f, r_30s = 0.f, r_1m = 0.f;
    float r_2m = 0.f, r_5m = 0.f, r_10m = 0.f, r_30m = 0.f;
    float t_30s = 0.f, t_1m = 0.f, t_5m = 0.f, t_15m = 0.f;
    float dt_1m_5m = 0.f;
    float r_acc_30s = 0.f;
    float er_1m = 0.f, er_5m = 0.f, er_15m = 0.f;
    float pos_hl_5m = 0.f, pos_hl_15m = 0.f, pos_hl_30m = 0.f;
    float dist_hi_5m_bps = 0.f, dist_lo_5m_bps = 0.f;
    float dist_hi_15m_bps = 0.f, dist_lo_15m_bps = 0.f;
    float dist_hi_30m_bps = 0.f, dist_lo_30m_bps = 0.f;
    float d_vwap_1m = 0.f, d_vwap_5m = 0.f, d_vwap_30m = 0.f;
    float clv_1m = 0.f, clv_5m = 0.f;
    float wick_up_1m = 0.f, wick_dn_1m = 0.f;
    float wick_up_5m = 0.f, wick_dn_5m = 0.f;
    float tfi_5s = 0.f, tfi_15s = 0.f, tfi_30s = 0.f;
    float tfi_1m = 0.f, tfi_2m = 0.f, tfi_5m = 0.f;
    int run_len = 0;
    float beta_cvd_1m = 0.f, beta_cvd_5m = 0.f;
    float t_cvd_1m = 0.f, t_cvd_5m = 0.f;
    float div_1m = 0.f, div_5m = 0.f;
    float corr_px_cvd_5m = 0.f;
    float vol_30s = 0.f, vol_1m = 0.f, vol_5m = 0.f;
    int n_30s = 0, n_1m = 0, n_5m = 0;
    float nps_30s = 0.f, nps_1m = 0.f, nps_5m = 0.f;
    float vol_acc = 0.f;
    float spread_frac = 0.f;
    float amihud_1m = 0.f, amihud_5m = 0.f;
    float lambda_1m = 0.f, lambda_5m = 0.f;
    float basis = 0.f, basis_z = 0.f, d_basis = 0.f;
    float funding = 0.f, t_to_fund_s = 0.f;
    int utc_minute = 0, utc_hour = 0;
    float is_m5 = 0.f, is_m15 = 0.f;
    float doi_1m = 0.f, doi_5m = 0.f, px_oi_1m = 0.f;
    float liq_imb_30s = 0.f, liq_imb_5m = 0.f, liq_acc = 0.f;
} data_stream;

class mft_script: public script
{
public:
    mft_script();
    void init(std::string instId) override;
    void run() override;
    void calculate_factors();
    void calculate_factor() { calculate_factors(); }
    const data_stream& factors() const { return out; }

private:
    std::string trade_id;
    data_stream out{};
    float funding_ = 0.f;

    struct SecBar
    {
        long long t_ms = 0;
        float o = 0.f;
        float h = 0.f;
        float l = 0.f;
        float c = 0.f;
        float v = 0.f;
        float vp = 0.f; // taker buy
        float vn = 0.f; // taker sell
        float cvd = 0.f;
        int n = 0;
    };

    static constexpr int kMaxBars = 1860;          // 30m+ of 1s bars so RV_30m has 1800 returns
    static constexpr float kEps = 1e-12f;
    static constexpr float kG = 0.001f;            // TP barrier g
    static constexpr float kL = 0.001f;            // SL barrier l

    std::vector<SecBar> bars_;
    std::deque<float> basis_1s_;
    std::deque<float> rv5m_hist_;
    long long last_trade_ms_ = 0;
    int last_same_ms_n_ = 0;
    long long now_ms_ = 0;
    long long last_basis_sec_ = -1;
    long long last_rvq_ms_ = 0;
    float last_basis_ = 0.f;

    float bid_ = 0.f;
    float ask_ = 0.f;
    float mid_ = 0.f;
    float bid_qty_ = 0.f;
    float ask_qty_ = 0.f;
    float mark_ = 0.f;
    float index_ = 0.f;
    float oi_ = 0.f;
    float liq_long_30s_ = 0.f;
    float liq_short_30s_ = 0.f;
    float liq_long_5m_ = 0.f;
    float liq_short_5m_ = 0.f;
    float doi_1m_raw_ = 0.f;
    float doi_5m_raw_ = 0.f;
    long long next_funding_time_ = 0;

    void update_book();
    void update_funding_oi_liq();
    void ingest_new_trades();
    void add_trade(const agg_trade_info& tr);
    void ensure_bar(long long bucket_ms, float px);
    void prune_bars();
    void push_basis_sample(float b);
    void push_rv5m_sample(float rv);

    bool ready(int sec) const;
    size_t win0(int sec) const;
    float last_close() const;
    float rv_w(int sec) const;
    void rs_w(int sec, float& plus, float& minus) const;
    float hl_ln(int sec) const;
    float ret_w(int sec) const;
    float trend_w(int sec) const;
    float er_w(int sec) const;
    void pos_w(int sec, float& pos, float& dhi_bps, float& dlo_bps) const;
    float vwap_d(int sec) const;
    void shape_w(int sec, float& clv, float& wu, float& wd) const;
    float tfi_w(int sec) const;
    int tfi_run_len() const;
    void cvd_w(int sec, float& beta, float& T) const;
    float corr_px_cvd(int sec) const;
    void act_w(int sec, float& vol, int& n, float& nps) const;
    float amihud_w(int sec) const;
    float lambda_w(int sec) const;
    float vvol_w(int minutes) const;
    static float sgn(float x);
};

#endif //PROGRESSIV_MFT_SCRIPT_H
