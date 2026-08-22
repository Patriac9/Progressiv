//
// Created by zagym on 09/08/2026.
//
#include "orderbook_script.h"
#include "async_log.h"
#include "loader.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace
{
    std::string format_decimal(double x, int decimals)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(std::max(0, decimals)) << x;
        return oss.str();
    }

    long long pow10_ll(int n)
    {
        long long p = 1;
        for (int i = 0; i < n; ++i)
            p *= 10;
        return p;
    }

    std::string format_units(long long units, int decimals)
    {
        if (decimals <= 0)
            return std::to_string(units);
        const bool neg = units < 0;
        unsigned long long v = neg
            ? static_cast<unsigned long long>(-units)
            : static_cast<unsigned long long>(units);
        std::string s = std::to_string(v);
        if (static_cast<int>(s.size()) <= decimals)
            s.insert(0, static_cast<size_t>(decimals + 1 - static_cast<int>(s.size())), '0');
        s.insert(s.end() - decimals, '.');
        if (neg)
            s.insert(s.begin(), '-');
        return s;
    }

    long long tick_index(double x, double tick, bool round_up)
    {
        if (tick <= 0.0)
            return 0;
        if (round_up)
            return static_cast<long long>(std::ceil(x / tick - 1e-12));
        return static_cast<long long>(std::floor(x / tick + 1e-12));
    }

    // 按 tick 整数格格式化，小数位不超过 tickSize（避免 float 四舍五入多出位数）
    std::string format_tick_n(long long n, double tick, int decimals)
    {
        const long long scale = pow10_ll(std::max(0, decimals));
        const long long tick_u = std::llround(tick * static_cast<double>(scale));
        if (tick_u <= 0)
            return format_decimal(static_cast<double>(n) * tick, decimals);
        return format_units(n * tick_u, decimals);
    }

    std::string format_on_tick(double x, double tick, int decimals, bool round_up)
    {
        return format_tick_n(tick_index(x, tick, round_up), tick, decimals);
    }

    // 买单 floor 到买一，卖单 ceil 到卖一
    bool same_side_bbo(bool is_buy, double bid, double ask, double tick, int decimals,
                       std::string& px_str, double& px)
    {
        if (!(bid < ask) || bid <= 0.0 || ask <= 0.0 || tick <= 0.0)
            return false;
        const long long n = is_buy
            ? tick_index(bid, tick, false)
            : tick_index(ask, tick, true);
        if (n <= 0)
            return false;
        px = static_cast<double>(n) * tick;
        if (is_buy && !(px < ask))
            return false;
        if (!is_buy && !(px > bid))
            return false;
        px_str = format_tick_n(n, tick, decimals);
        return true;
    }

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

    std::string load_first(std::initializer_list<const char*> paths)
    {
        for (const char* path : paths)
        {
            std::string content = loader::load_file(path);
            if (!content.empty())
                return content;
        }
        return {};
    }

    template <typename Fn>
    void for_each_kv(const std::string& content, Fn&& on_kv)
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
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            trim_inplace(key);
            trim_inplace(value);
            if (!key.empty())
                on_kv(key, value);
        }
    }

    float parse_float(const std::string& s, float fallback = 0.f)
    {
        if (s.empty())
            return fallback;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str())
            return fallback;
        return v;
    }

    bool parse_is_exp(std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value == "exp";
    }

    bool parse_bool(std::string value, bool fallback = false)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (value == "1" || value == "true" || value == "yes" || value == "on")
            return true;
        if (value == "0" || value == "false" || value == "no" || value == "off")
            return false;
        return fallback;
    }

    constexpr float kTheta = 0.001f;
    constexpr float kRefillEwma = 0.2f;
    constexpr float kImbEps = 1e-12f;

    long long px_key(float px, float tick)
    {
        if (!(tick > 0.f))
            return 0;
        return std::llround(static_cast<double>(px) / static_cast<double>(tick));
    }

    float imb_of(float pos, float neg)
    {
        const float s = pos + neg;
        return std::fabs(s) > kImbEps ? (pos - neg) / s : 0.f;
    }

    float qty_at_key(const std::vector<orderbook_level>& book, long long key, float tick)
    {
        for (const auto& lv : book)
        {
            if (px_key(lv.price, tick) == key)
                return lv.quantity;
        }
        return 0.f;
    }

    bool has_px(const std::vector<orderbook_level>& book, long long key, float tick)
    {
        for (const auto& lv : book)
        {
            if (px_key(lv.price, tick) == key)
                return true;
        }
        return false;
    }

    struct BandFeat
    {
        float depth = 0.f;
        float cover = 0.f;
        float gap_ticks = 0.f;
        float gap_max = 0.f;
        float slope = 0.f;
    };

    BandFeat scan_band(const std::vector<orderbook_level>& levels, bool is_ask,
                       float mid, float bound, float tick)
    {
        BandFeat out;
        if (!(mid > 0.f) || levels.empty())
            return out;

        double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
        int n = 0;
        float first_px = 0.f;
        float last_px = 0.f;
        float prev_px = 0.f;
        float gap_max = 0.f;

        for (const auto& lv : levels)
        {
            if (!(lv.quantity > 0.f))
                continue;
            const float px = lv.price;
            if (is_ask)
            {
                if (px < mid)
                    continue;
                if (px > bound)
                    break;
            }
            else
            {
                if (px > mid)
                    continue;
                if (px < bound)
                    break;
            }

            if (n == 0)
                first_px = px;
            else if (tick > 0.f)
            {
                const float adj = std::fabs(px - prev_px) / tick - 1.f;
                if (adj > gap_max)
                    gap_max = adj;
            }
            last_px = px;
            prev_px = px;

            const float dist = tick > 0.f ? std::fabs(px - mid) / tick : static_cast<float>(n);
            sum_x += dist;
            sum_y += lv.quantity;
            sum_xx += static_cast<double>(dist) * dist;
            sum_xy += static_cast<double>(dist) * lv.quantity;
            out.depth += lv.quantity;
            ++n;
        }

        if (n <= 0)
            return out;

        const float need = mid * kTheta;
        const float got = is_ask ? (last_px - mid) : (mid - last_px);
        if (need > 0.f)
            out.cover = std::clamp(got / need, 0.f, 1.f);

        if (tick > 0.f && n >= 2)
        {
            const long long span = std::llabs(px_key(last_px, tick) - px_key(first_px, tick)) + 1;
            out.gap_ticks = static_cast<float>(std::max(0LL, span - n));
        }
        out.gap_max = std::max(0.f, gap_max);

        if (n >= 2)
        {
            const double nd = static_cast<double>(n);
            const double var_x = sum_xx - sum_x * sum_x / nd;
            if (var_x > 1e-12)
                out.slope = static_cast<float>((sum_xy - sum_x * sum_y / nd) / var_x);
        }
        return out;
    }

    void side_delta(const std::vector<orderbook_level>& prev,
                    const std::vector<orderbook_level>& cur,
                    bool is_ask, float mid, float bound, float tick,
                    float& add, float& reduce)
    {
        add = 0.f;
        reduce = 0.f;
        auto in_band = [&](float px)
        {
            return is_ask ? (px >= mid && px <= bound) : (px <= mid && px >= bound);
        };

        for (const auto& lv : cur)
        {
            if (!in_band(lv.price) || !(lv.quantity >= 0.f))
                continue;
            const float q0 = qty_at_key(prev, px_key(lv.price, tick), tick);
            const float d = lv.quantity - q0;
            if (d > 0.f)
                add += d;
            else if (d < 0.f)
                reduce -= d;
        }
        for (const auto& lv : prev)
        {
            if (!in_band(lv.price) || !(lv.quantity > 0.f))
                continue;
            if (!has_px(cur, px_key(lv.price, tick), tick))
                reduce += lv.quantity;
        }
    }

    void ewma_refill(float& acc, float x)
    {
        if (!(acc > 0.f) && acc != 0.f)
            acc = 0.f;
        acc = (1.f - kRefillEwma) * acc + kRefillEwma * std::clamp(x, 0.f, 5.f);
    }
}

orderbook_script::orderbook_script() = default;

orderbook_script::~orderbook_script() = default;

void orderbook_script::init(std::string instId)
{
    script::init(instId);
    has_prev_ = false;
    prev_asks.clear();
    prev_bids.clear();
    ml_ofi.clear();
    ml_ofi_time_ms_.clear();
    ml_ofi_start_ms_ = 0;
    ml_ofi_250ms = ml_ofi_1s = ml_ofi_2s = 0.f;
    ml_ofi_5s = ml_ofi_15s = ml_ofi_30s = ml_ofi_60s = 0.f;
    feat_time_ms_ = 0;
    flow_hist_.clear();
    clears_.clear();
    hawkes_buy_mo_ = hawkes_sell_mo_ = 0.f;
    hawkes_ask_add_ = hawkes_bid_add_ = 0.f;
    hawkes_ask_cancel_ = hawkes_bid_cancel_ = 0.f;
    refill_ask_50_ = refill_ask_100_ = refill_ask_250_ = 0.f;
    refill_bid_50_ = refill_bid_100_ = refill_bid_250_ = 0.f;
    mid_hist_time_ms_.clear();
    mid_hist_.clear();
    mid_hist_start_ms_ = 0;
    exec_mid_time_ms_.clear();
    exec_mid_hist_.clear();
    n_mid_moves_30s = n_mid_moves_60s = rv_30s = rv_60s = 0.f;
    obi = 0.f;
    mid_price = 0.f;
    predicted_movement = 0.f;
    alpha = 0.f;
    position_ = {};
    tau = 0.407647f;
    output_ = {};
    {
        std::lock_guard<std::mutex> lock(signal_mu_);
        published_ = {};
    }
    signal_seq_.store(0);

    tick_size = progressiv_->interface_->get_tick_size(instId);
    const auto tf = progressiv_->interface_->get_tick_filter(progressiv_->interface_->trade_symbol());
    trade_tick_size_ = static_cast<float>(tf.tick_size);
    trade_price_decimals_ = tf.decimals;
    load_model_params();
}

void orderbook_script::load_model_params()
{
    t_coef_ = {};
    alpha_coef_ = {};

    const std::string p_text = load_first({
        "param.mod",
        "model/param.mod",
        "models/param.mod",
        "../model/param.mod",
    });
    if (p_text.empty())
    {
        async_log::instance().error("param.mod not found; using built-in horizon/tp_offset/q_ord");
    }
    else
    {
        for_each_kv(p_text, [&](const std::string& key, const std::string& value)
        {
            if (key == "horizon")
                horizon = parse_float(value, horizon);
            else if (key == "tp_offset")
                tp_offset = parse_float(value, tp_offset);
            else if (key == "q_ord")
                q_ord = parse_float(value, q_ord);
            else if (key == "enable_dynamic_risk_management")
                enable_dynamic_risk_management = parse_bool(value, enable_dynamic_risk_management);
        });
        std::ostringstream oss;
        oss << "loaded param.mod horizon=" << horizon
            << " tp_offset=" << tp_offset
            << " q_ord=" << q_ord
            << " enable_dynamic_risk_management="
            << (enable_dynamic_risk_management ? "true" : "false");
        async_log::instance().info(oss.str());
    }

    const std::string t_text = load_first({
        "T_Param",
        "models/T_Param",
        "pytool/models/T_Param",
        "../pytool/models/T_Param",
    });
    if (t_text.empty())
    {
        async_log::instance().error("T_Param not found; T() will return 0");
    }
    else
    {
        for_each_kv(t_text, [&](const std::string& key, const std::string& value)
        {
            const float v = parse_float(value);
            if (key == "intercept") t_coef_.intercept = v;
            else if (key == "obi") t_coef_.obi = v;
            else if (key == "ml_ofi_5s" || key == "ofi_5s") t_coef_.ml_ofi_5s = v;
            else if (key == "ml_ofi_15s" || key == "ofi_15s") t_coef_.ml_ofi_15s = v;
            else if (key == "ml_ofi_30s" || key == "ofi_30s") t_coef_.ml_ofi_30s = v;
            else if (key == "ml_ofi_60s" || key == "ofi_60s") t_coef_.ml_ofi_60s = v;
            else if (key == "tau") tau = v;
        });
        std::ostringstream oss;
        oss << "loaded T_Param intercept=" << t_coef_.intercept
            << " obi=" << t_coef_.obi
            << " ml_ofi_5s=" << t_coef_.ml_ofi_5s
            << " ml_ofi_15s=" << t_coef_.ml_ofi_15s
            << " ml_ofi_30s=" << t_coef_.ml_ofi_30s
            << " ml_ofi_60s=" << t_coef_.ml_ofi_60s
            << " tau=" << tau;
        async_log::instance().info(oss.str());
    }

    const std::string a_text = load_first({
        "alpha_Param",
        "models/alpha_Param",
        "pytool/models/alpha_Param",
        "../pytool/models/alpha_Param",
    });
    if (a_text.empty())
    {
        async_log::instance().error("alpha_Param not found; calculate_alpha() will use defaults");
    }
    else
    {
        for_each_kv(a_text, [&](const std::string& key, const std::string& value)
        {
            if (key == "kind")
            {
                alpha_coef_.is_exp = parse_is_exp(value);
                return;
            }
            const float v = parse_float(value);
            if (key == "intercept") alpha_coef_.intercept = v;
            else if (key == "n_mid_moves_30s") alpha_coef_.n_mid_moves_30s = v;
            else if (key == "n_mid_moves_60s") alpha_coef_.n_mid_moves_60s = v;
            else if (key == "rv_30s") alpha_coef_.rv_30s = v;
            else if (key == "rv_60s") alpha_coef_.rv_60s = v;
            else if (key == "abs_T") alpha_coef_.abs_T = v;
            else if (key == "scale") alpha_coef_.scale = v;
            else if (key == "clip_min") alpha_coef_.clip_min = v;
            else if (key == "clip_max") alpha_coef_.clip_max = v;
        });
        std::ostringstream oss;
        oss << "loaded alpha_Param kind=" << (alpha_coef_.is_exp ? "exp" : "linear")
            << " intercept=" << alpha_coef_.intercept
            << " clip=[" << alpha_coef_.clip_min << "," << alpha_coef_.clip_max << "]";
        async_log::instance().info(oss.str());
    }
}

float orderbook_script::T(const T_parameter& param_) const
{
    return t_coef_.intercept
         + t_coef_.obi * param_.obi
         + t_coef_.ml_ofi_5s * param_.ml_ofi_5s
         + t_coef_.ml_ofi_15s * param_.ml_ofi_15s
         + t_coef_.ml_ofi_30s * param_.ml_ofi_30s
         + t_coef_.ml_ofi_60s * param_.ml_ofi_60s;
}

float orderbook_script::calculate_alpha(const alpha_parameter& param_) const
{
    const float z = alpha_coef_.intercept
                  + alpha_coef_.n_mid_moves_30s * param_.n_mid_moves_30s
                  + alpha_coef_.n_mid_moves_60s * param_.n_mid_moves_60s
                  + alpha_coef_.rv_30s * param_.rv_30s
                  + alpha_coef_.rv_60s * param_.rv_60s
                  + alpha_coef_.abs_T * param_.abs_T;

    float hat = alpha_coef_.is_exp ? std::exp(z) : z;
    if (!(hat > 0.f))
        hat = 0.f;
    hat *= alpha_coef_.scale;
    if (hat < alpha_coef_.clip_min)
        hat = alpha_coef_.clip_min;
    if (hat > alpha_coef_.clip_max)
        hat = alpha_coef_.clip_max;
    return hat;
}

void orderbook_script::update_ml_ofi_windows(long long now_ms)
{
    ml_ofi_250ms = ml_ofi_1s = ml_ofi_2s = 0.f;
    ml_ofi_5s = ml_ofi_15s = ml_ofi_30s = ml_ofi_60s = 0.f;
    if (ml_ofi_start_ms_ == 0)
        return;
    const long long age_start = now_ms - ml_ofi_start_ms_;
    float s250 = 0.f, a250 = 0.f, s1 = 0.f, a1 = 0.f, s2 = 0.f, a2 = 0.f;
    float s5 = 0.f, a5 = 0.f, s15 = 0.f, a15 = 0.f;
    float s30 = 0.f, a30 = 0.f, s60 = 0.f, a60 = 0.f;
    const size_t n = ml_ofi.size();
    for (size_t i = 0; i < n; ++i)
    {
        const long long age = now_ms - ml_ofi_time_ms_[i];
        if (age > kMlOfiMaxMs)
            break;
        const float v = ml_ofi[i];
        const float av = std::fabs(v);
        s60 += v;
        a60 += av;
        if (age <= 30000)
        {
            s30 += v;
            a30 += av;
        }
        if (age <= 15000)
        {
            s15 += v;
            a15 += av;
        }
        if (age <= 5000)
        {
            s5 += v;
            a5 += av;
        }
        if (age <= 2000)
        {
            s2 += v;
            a2 += av;
        }
        if (age <= 1000)
        {
            s1 += v;
            a1 += av;
        }
        if (age <= 250)
        {
            s250 += v;
            a250 += av;
        }
    }
    if (age_start >= 250 && a250 > 1e-12f)
        ml_ofi_250ms = s250 / a250;
    if (age_start >= 1000 && a1 > 1e-12f)
        ml_ofi_1s = s1 / a1;
    if (age_start >= 2000 && a2 > 1e-12f)
        ml_ofi_2s = s2 / a2;
    if (age_start >= 5000 && a5 > 1e-12f)
        ml_ofi_5s = s5 / a5;
    if (age_start >= 15000 && a15 > 1e-12f)
        ml_ofi_15s = s15 / a15;
    if (age_start >= 30000 && a30 > 1e-12f)
        ml_ofi_30s = s30 / a30;
    if (age_start >= 60000 && a60 > 1e-12f)
        ml_ofi_60s = s60 / a60;
}

void orderbook_script::update_barrier_features(long long now_ms)
{
    const float mid = mid_price;
    const float tick = tick_size > 0.f ? tick_size : 0.f;
    const float up = mid * (1.f + kBarrierTheta);
    const float dn = mid * (1.f - kBarrierTheta);
    const BandFeat ask_b = scan_band(asks, true, mid, up, tick);
    const BandFeat bid_b = scan_band(bids, false, mid, dn, tick);

    output_.d_ask_10bp = ask_b.depth;
    output_.d_bid_10bp = bid_b.depth;
    output_.d_imb_10bp = imb_of(bid_b.depth, ask_b.depth);
    output_.cover_ask_10bp = ask_b.cover;
    output_.cover_bid_10bp = bid_b.cover;
    output_.gap_ask = ask_b.gap_ticks;
    output_.gap_bid = bid_b.gap_ticks;
    output_.gap_max_ask = ask_b.gap_max;
    output_.gap_max_bid = bid_b.gap_max;
    output_.gap_imb = imb_of(ask_b.gap_ticks + ask_b.gap_max, bid_b.gap_ticks + bid_b.gap_max);
    output_.ask_slope = ask_b.slope;
    output_.bid_slope = bid_b.slope;
    output_.slope_imb = imb_of(bid_b.slope, ask_b.slope);

    long long dt_ms = 0;
    if (feat_time_ms_ > 0 && now_ms > feat_time_ms_)
        dt_ms = now_ms - feat_time_ms_;
    if (dt_ms > 2000)
        dt_ms = 2000;

    float buy_mo = 0.f;
    float sell_mo = 0.f;
    if (progressiv_ && progressiv_->interface_ && dt_ms > 0)
    {
        const aggressor_flow flow = progressiv_->interface_->get_ws_aggressor_flow(dt_ms);
        buy_mo = flow.buy_qty;
        sell_mo = flow.sell_qty;
    }
    if (progressiv_ && progressiv_->interface_)
        output_.aggressor_imb_5s = progressiv_->interface_->get_ws_aggressor_flow(5000).imbalance;

    const float dt_s = dt_ms > 0 ? static_cast<float>(dt_ms) * 0.001f : 0.f;
    if (dt_s > 0.f && dt_s < 5.f)
    {
        const float decay = std::exp(-kHawkesBeta * dt_s);
        hawkes_buy_mo_ *= decay;
        hawkes_sell_mo_ *= decay;
        hawkes_ask_add_ *= decay;
        hawkes_bid_add_ *= decay;
        hawkes_ask_cancel_ *= decay;
        hawkes_bid_cancel_ *= decay;
    }
    else if (dt_s >= 5.f)
    {
        hawkes_buy_mo_ = hawkes_sell_mo_ = 0.f;
        hawkes_ask_add_ = hawkes_bid_add_ = 0.f;
        hawkes_ask_cancel_ = hawkes_bid_cancel_ = 0.f;
    }

    float ask_add = 0.f, ask_reduce = 0.f, bid_add = 0.f, bid_reduce = 0.f;
    if (has_prev_ && tick > 0.f)
    {
        side_delta(prev_asks, asks, true, mid, up, tick, ask_add, ask_reduce);
        side_delta(prev_bids, bids, false, mid, dn, tick, bid_add, bid_reduce);

        auto in_ask = [&](float px) { return px >= mid && px <= up; };
        auto in_bid = [&](float px) { return px <= mid && px >= dn; };
        for (const auto& lv : prev_asks)
        {
            if (!in_ask(lv.price) || !(lv.quantity > 0.f))
                continue;
            if (qty_at_key(asks, px_key(lv.price, tick), tick) > 0.f)
                continue;
            ClearEvent ev;
            ev.t_ms = now_ms;
            ev.price = lv.price;
            ev.qty_before = lv.quantity;
            ev.is_ask = true;
            clears_.push_back(ev);
        }
        for (const auto& lv : prev_bids)
        {
            if (!in_bid(lv.price) || !(lv.quantity > 0.f))
                continue;
            if (qty_at_key(bids, px_key(lv.price, tick), tick) > 0.f)
                continue;
            ClearEvent ev;
            ev.t_ms = now_ms;
            ev.price = lv.price;
            ev.qty_before = lv.quantity;
            ev.is_ask = false;
            clears_.push_back(ev);
        }
        while (clears_.size() > 512)
            clears_.pop_front();
    }

    const float ask_cancel = std::max(0.f, ask_reduce - buy_mo);
    const float bid_cancel = std::max(0.f, bid_reduce - sell_mo);
    hawkes_buy_mo_ += buy_mo;
    hawkes_sell_mo_ += sell_mo;
    hawkes_ask_add_ += ask_add;
    hawkes_bid_add_ += bid_add;
    hawkes_ask_cancel_ += ask_cancel;
    hawkes_bid_cancel_ += bid_cancel;

    output_.hawkes_buy_mo = hawkes_buy_mo_;
    output_.hawkes_sell_mo = hawkes_sell_mo_;
    output_.hawkes_ask_add = hawkes_ask_add_;
    output_.hawkes_bid_add = hawkes_bid_add_;
    output_.hawkes_ask_cancel = hawkes_ask_cancel_;
    output_.hawkes_bid_cancel = hawkes_bid_cancel_;

    const float ask_net = buy_mo + ask_cancel - ask_add;
    const float bid_net = sell_mo + bid_cancel - bid_add;
    flow_hist_.push_back({now_ms, ask_b.depth, bid_b.depth, ask_net, bid_net});
    while (!flow_hist_.empty() && now_ms - flow_hist_.front().t_ms > kFlowKeepMs)
        flow_hist_.pop_front();

    float sum_ask = 0.f, sum_bid = 0.f;
    float d_ask_1s = ask_b.depth;
    float d_bid_1s = bid_b.depth;
    long long t_old = now_ms;
    for (auto it = flow_hist_.rbegin(); it != flow_hist_.rend(); ++it)
    {
        const long long age = now_ms - it->t_ms;
        if (age > kDepleteMs)
            break;
        sum_ask += it->ask_net;
        sum_bid += it->bid_net;
        d_ask_1s = it->d_ask;
        d_bid_1s = it->d_bid;
        t_old = it->t_ms;
    }
    const float span = std::max(0.05f, static_cast<float>(now_ms - t_old) * 0.001f);
    if (!flow_hist_.empty() && now_ms - flow_hist_.front().t_ms >= kDepleteMs)
    {
        output_.r_ask = sum_ask / span;
        output_.r_bid = sum_bid / span;
    }
    else
    {
        output_.r_ask = 0.f;
        output_.r_bid = 0.f;
    }
    output_.d_ask_chg_1s = ask_b.depth - d_ask_1s;
    output_.d_bid_chg_1s = bid_b.depth - d_bid_1s;

    auto t_clear = [](float depth, float rate) -> float
    {
        if (!(depth > 0.f))
            return 0.f;
        if (rate > 1e-8f)
            return depth / rate;
        return 1.e6f;
    };
    const float h = horizon > 1.f ? horizon : 15.f;
    output_.t_clear_ask = t_clear(ask_b.depth, output_.r_ask);
    output_.t_clear_bid = t_clear(bid_b.depth, output_.r_bid);
    output_.t_clear_ask_h = output_.t_clear_ask / h;
    output_.t_clear_bid_h = output_.t_clear_bid / h;

    for (auto& ev : clears_)
    {
        const long long age = now_ms - ev.t_ms;
        const auto& book = ev.is_ask ? asks : bids;
        const float qn = qty_at_key(book, px_key(ev.price, tick), tick);
        const float ratio = ev.qty_before > 1e-12f ? qn / ev.qty_before : 0.f;
        auto apply = [&](unsigned bit, long long need, float& acc)
        {
            if ((ev.mask & bit) || age < need)
                return;
            ewma_refill(acc, ratio);
            ev.mask |= bit;
        };
        if (ev.is_ask)
        {
            apply(1u, 50, refill_ask_50_);
            apply(2u, 100, refill_ask_100_);
            apply(4u, 250, refill_ask_250_);
        }
        else
        {
            apply(1u, 50, refill_bid_50_);
            apply(2u, 100, refill_bid_100_);
            apply(4u, 250, refill_bid_250_);
        }
    }
    while (!clears_.empty() && (clears_.front().mask & 7u) == 7u)
        clears_.pop_front();
    while (!clears_.empty() && now_ms - clears_.front().t_ms > 2000)
        clears_.pop_front();

    output_.refill_ask_50ms = refill_ask_50_;
    output_.refill_ask_100ms = refill_ask_100_;
    output_.refill_ask_250ms = refill_ask_250_;
    output_.refill_bid_50ms = refill_bid_50_;
    output_.refill_bid_100ms = refill_bid_100_;
    output_.refill_bid_250ms = refill_bid_250_;

    feat_time_ms_ = now_ms;
}

void orderbook_script::update_dir_micro_features(long long now_ms, float ofi_tick, float l1_ofi_tick)
{
    output_.ofi_tick = ofi_tick;
    output_.l1_ofi_tick = l1_ofi_tick;
    output_.ml_ofi_250ms = ml_ofi_250ms;
    output_.ml_ofi_1s = ml_ofi_1s;
    output_.ml_ofi_2s = ml_ofi_2s;

    auto mid_ret = [&](long long window_ms) -> float {
        if (mid_hist_start_ms_ == 0 || now_ms - mid_hist_start_ms_ < window_ms)
            return 0.f;
        if (!(mid_price > 0.f) || mid_hist_.empty())
            return 0.f;
        float then = mid_hist_.back();
        for (size_t i = 0; i < mid_hist_time_ms_.size(); ++i)
        {
            if (now_ms - mid_hist_time_ms_[i] >= window_ms)
            {
                then = mid_hist_[i];
                break;
            }
        }
        if (!(then > 0.f))
            return 0.f;
        return mid_price / then - 1.f;
    };
    output_.ret_100ms = mid_ret(100);
    output_.ret_250ms = mid_ret(250);
    output_.ret_1s = mid_ret(1000);
    output_.ret_5s = mid_ret(5000);

    const float b1 = bids.empty() ? 0.f : bids[0].quantity;
    const float a1 = asks.empty() ? 0.f : asks[0].quantity;
    output_.sig_l1_imb = imb_of(b1, a1);
    float b3 = 0.f, a3 = 0.f;
    for (size_t i = 0; i < 3 && i < bids.size(); ++i)
        b3 += bids[i].quantity;
    for (size_t i = 0; i < 3 && i < asks.size(); ++i)
        a3 += asks[i].quantity;
    output_.sig_l3_imb = imb_of(b3, a3);
    output_.sig_micro_off = 0.f;
    output_.sig_spread_ticks = 0.f;
    if (!bids.empty() && !asks.empty())
    {
        if (b1 + a1 > 1e-12f)
        {
            const float micro = (asks[0].price * b1 + bids[0].price * a1) / (b1 + a1);
            output_.sig_micro_off = micro - mid_price;
        }
        if (tick_size > 0.f)
            output_.sig_spread_ticks = (asks[0].price - bids[0].price) / tick_size;
    }

    output_.aggressor_imb_250ms = 0.f;
    output_.aggressor_imb_1s = 0.f;
    output_.aggressor_net_1s = 0.f;
    output_.last_trade_sign = 0.f;
    output_.last_trade_mid_bps = 0.f;
    output_.last_trade_age_ms = 0.f;
    output_.exec_aggressor_imb_250ms = 0.f;
    output_.exec_aggressor_imb_1s = 0.f;
    output_.basis = 0.f;
    output_.basis_chg_1s = 0.f;

    if (progressiv_ && progressiv_->interface_)
    {
        auto* iface = progressiv_->interface_;
        try
        {
            output_.aggressor_imb_250ms = iface->get_ws_aggressor_flow(250).imbalance;
            const aggressor_flow f1 = iface->get_ws_aggressor_flow(1000);
            output_.aggressor_imb_1s = f1.imbalance;
            output_.aggressor_net_1s = f1.net_qty;
        }
        catch (const std::exception&)
        {
        }
        try
        {
            const agg_trade_info tr = iface->get_ws_last_agg_trade(false);
            if (tr.quantity > 0.f && tr.price > 0.f)
            {
                output_.last_trade_sign = tr.buyer_is_maker ? -1.f : 1.f;
                if (mid_price > 0.f)
                    output_.last_trade_mid_bps = (tr.price - mid_price) / mid_price * 1.e4f;
                const long long tt = tr.trade_time != 0 ? tr.trade_time : tr.message_time;
                if (tt > 0)
                    output_.last_trade_age_ms = static_cast<float>(std::max(0LL, now_ms - tt));
            }
        }
        catch (const std::exception&)
        {
        }

        if (iface->split_trade_market())
        {
            try
            {
                output_.exec_aggressor_imb_250ms = iface->get_ws_aggressor_flow(250, true).imbalance;
                output_.exec_aggressor_imb_1s = iface->get_ws_aggressor_flow(1000, true).imbalance;
            }
            catch (const std::exception&)
            {
            }
            try
            {
                const orderbook_info book = iface->get_ws_trade_orderbook();
                if (!book.bids.empty() && !book.asks.empty() && mid_price > 0.f)
                {
                    const float emid = 0.5f * (book.bids[0].price + book.asks[0].price);
                    if (emid > 0.f)
                        output_.basis = (emid - mid_price) / mid_price;
                    exec_mid_hist_.push_front(emid);
                    exec_mid_time_ms_.push_front(now_ms);
                    while (!exec_mid_time_ms_.empty() && now_ms - exec_mid_time_ms_.back() > 5000)
                    {
                        exec_mid_hist_.pop_back();
                        exec_mid_time_ms_.pop_back();
                    }
                    if (emid > 0.f && !exec_mid_hist_.empty()
                        && now_ms - exec_mid_time_ms_.back() >= 1000)
                    {
                        float then = exec_mid_hist_.back();
                        for (size_t i = 0; i < exec_mid_time_ms_.size(); ++i)
                        {
                            if (now_ms - exec_mid_time_ms_[i] >= 1000)
                            {
                                then = exec_mid_hist_[i];
                                break;
                            }
                        }
                        if (then > 0.f)
                            output_.basis_chg_1s = (emid / then - 1.f) - output_.ret_1s;
                    }
                }
            }
            catch (const std::exception&)
            {
            }
        }
    }

    output_.hawkes_mo_imb = imb_of(hawkes_buy_mo_, hawkes_sell_mo_);
    output_.hawkes_add_imb = imb_of(hawkes_bid_add_, hawkes_ask_add_);
    output_.hawkes_cancel_imb = imb_of(hawkes_ask_cancel_, hawkes_bid_cancel_);
    output_.r_imb = imb_of(output_.r_ask, output_.r_bid);
    output_.d_chg_imb_1s = imb_of(output_.d_bid_chg_1s, output_.d_ask_chg_1s);
    output_.refill_imb_100ms = imb_of(output_.refill_bid_100ms, output_.refill_ask_100ms);
    output_.cover_imb_10bp = imb_of(output_.cover_bid_10bp, output_.cover_ask_10bp);
}

void orderbook_script::publish_signal(uint64_t tick, long long msg_ms, long long txn_ms)
{
    signal_snapshot snap{};
    snap.tick = tick;
    snap.message_time_ms = msg_ms;
    snap.transact_time_ms = txn_ms;
    snap.T = predicted_movement;
    snap.alpha = alpha;
    snap.tau = tau;
    snap.horizon = horizon;
    snap.tp_offset = tp_offset;
    snap.q_ord = q_ord;
    snap.enable_dynamic_risk_management = enable_dynamic_risk_management;
    // 执行热路径不需要整份 features，省略拷贝

    {
        std::lock_guard<std::mutex> lock(signal_mu_);
        snap.seq = signal_seq_.load(std::memory_order_relaxed) + 1;
        published_ = snap;
        signal_seq_.store(snap.seq, std::memory_order_release);
    }
    signal_cv_.notify_one();
}

bool orderbook_script::wait_take_signal(uint64_t& last_seq, signal_snapshot& out, int timeout_ms)
{
    // 短自旋：信号刚 publish 时多数情况无需进内核等待
    for (int i = 0; i < 64; ++i)
    {
        const uint64_t s = signal_seq_.load(std::memory_order_acquire);
        if (s != last_seq)
        {
            std::lock_guard<std::mutex> lock(signal_mu_);
            out = published_;
            last_seq = out.seq;
            return true;
        }
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__GNUC__)
        __builtin_ia32_pause();
#endif
    }

    std::unique_lock<std::mutex> lock(signal_mu_);
    if (!signal_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return signal_seq_.load(std::memory_order_acquire) != last_seq;
        }))
    {
        return false;
    }
    out = published_;
    last_seq = out.seq;
    return true;
}

signal_snapshot orderbook_script::latest_signal() const
{
    std::lock_guard<std::mutex> lock(signal_mu_);
    return published_;
}

void orderbook_script::run()
{
    run_signal();
}

void orderbook_script::run_signal()
{
    last_tick = current_tick;
    current_tick = progressiv_->current_tick;
    if (current_tick <= last_tick)
        return;

    if (asks.empty() || bids.empty())
        return;

    if (bids.size() > depth)
        bids.resize(depth);
    if (asks.size() > depth)
        asks.resize(depth);

    const float ask_px = asks[0].price;
    const float bid_px = bids[0].price;

    float bids_sum = 0.f;
    float asks_sum = 0.f;
    for (const auto& bid : bids)
        bids_sum += bid.quantity;
    for (const auto& ask : asks)
        asks_sum += ask.quantity;

    const float depth_sum = bids_sum + asks_sum;
    if (depth_sum <= 0.f)
        return;

    obi = (bids_sum - asks_sum) / depth_sum;
    mid_price = 0.5f * (ask_px + bid_px);

    float multilevel_ofi = 0.f;
    float l1_ofi = 0.f;
    if (has_prev_ && !prev_bids.empty() && !prev_asks.empty())
    {
        const int levels = static_cast<int>(std::min(
            {static_cast<size_t>(kMlOfiLevels),
             bids.size(), asks.size(),
             prev_bids.size(), prev_asks.size()}));

        for (int i = 0; i < levels; ++i)
        {
            const float b_px = bids[i].price;
            const float b_qty = bids[i].quantity;
            const float prev_b_px = prev_bids[i].price;
            const float prev_b_qty = prev_bids[i].quantity;
            float d_ofi = 0.f;
            if (b_px > prev_b_px)
                d_ofi += b_qty;
            else if (b_px < prev_b_px)
                d_ofi -= prev_b_qty;
            else
                d_ofi += b_qty - prev_b_qty;

            const float a_px = asks[i].price;
            const float a_qty = asks[i].quantity;
            const float prev_a_px = prev_asks[i].price;
            const float prev_a_qty = prev_asks[i].quantity;
            if (a_px < prev_a_px)
                d_ofi -= a_qty;
            else if (a_px > prev_a_px)
                d_ofi += prev_a_qty;
            else
                d_ofi -= (a_qty - prev_a_qty);

            multilevel_ofi += d_ofi;
            if (i == 0)
                l1_ofi = d_ofi;
        }
    }

    const long long now_ms = progressiv_->current_orderbook.message_time != 0
        ? progressiv_->current_orderbook.message_time
        : progressiv_->current_orderbook.transact_time;
    const long long msg_ms = progressiv_->current_orderbook.message_time;
    const long long txn_ms = progressiv_->current_orderbook.transact_time;

    // deque push_front O(1)，避免 vector insert(begin) 整表搬移
    ml_ofi.push_front(multilevel_ofi);
    ml_ofi_time_ms_.push_front(now_ms);
    if (ml_ofi_start_ms_ == 0)
        ml_ofi_start_ms_ = now_ms;
    while (!ml_ofi_time_ms_.empty() && now_ms - ml_ofi_time_ms_.back() > kMlOfiMaxMs)
    {
        ml_ofi.pop_back();
        ml_ofi_time_ms_.pop_back();
    }
    update_ml_ofi_windows(now_ms);

    mid_hist_.push_front(mid_price);
    mid_hist_time_ms_.push_front(now_ms);
    if (mid_hist_start_ms_ == 0)
        mid_hist_start_ms_ = now_ms;
    while (!mid_hist_time_ms_.empty() && now_ms - mid_hist_time_ms_.back() > kVolMaxMs)
    {
        mid_hist_time_ms_.pop_back();
        mid_hist_.pop_back();
    }

    n_mid_moves_30s = n_mid_moves_60s = rv_30s = rv_60s = 0.f;
    if (mid_hist_start_ms_ != 0 && mid_hist_.size() >= 2
        && now_ms - mid_hist_start_ms_ >= 30000)
    {
        double sumsq30 = 0.0, sumsq60 = 0.0;
        int moves30 = 0, moves60 = 0;
        const bool ready60 = (now_ms - mid_hist_start_ms_ >= 60000);
        for (size_t i = 0; i + 1 < mid_hist_.size(); ++i)
        {
            const long long age0 = now_ms - mid_hist_time_ms_[i];
            const long long age1 = now_ms - mid_hist_time_ms_[i + 1];
            if (age0 > 60000)
                break;
            if (age1 > 60000)
                break;
            const float d = mid_hist_[i] - mid_hist_[i + 1];
            const double dd = static_cast<double>(d) * static_cast<double>(d);
            const bool moved = std::fabs(d) > kMidEps;
            if (ready60)
            {
                sumsq60 += dd;
                if (moved)
                    ++moves60;
            }
            if (age0 <= 30000 && age1 <= 30000)
            {
                sumsq30 += dd;
                if (moved)
                    ++moves30;
            }
        }
        rv_30s = static_cast<float>(std::sqrt(sumsq30));
        n_mid_moves_30s = static_cast<float>(moves30);
        if (ready60)
        {
            rv_60s = static_cast<float>(std::sqrt(sumsq60));
            n_mid_moves_60s = static_cast<float>(moves60);
        }
    }

    update_barrier_features(now_ms);
    update_dir_micro_features(now_ms, multilevel_ofi, l1_ofi);

    prev_bids = bids;
    prev_asks = asks;
    has_prev_ = true;

    predicted_movement = T({
        .obi = obi,
        .ml_ofi_5s = ml_ofi_5s,
        .ml_ofi_15s = ml_ofi_15s,
        .ml_ofi_30s = ml_ofi_30s,
        .ml_ofi_60s = ml_ofi_60s,
    });
    alpha = calculate_alpha({
        .n_mid_moves_30s = n_mid_moves_30s,
        .n_mid_moves_60s = n_mid_moves_60s,
        .rv_30s = rv_30s,
        .rv_60s = rv_60s,
        .abs_T = std::fabs(predicted_movement),
    });

    output_.obi = obi;
    output_.mid_price = mid_price;
    output_.ml_ofi_5s = ml_ofi_5s;
    output_.ml_ofi_15s = ml_ofi_15s;
    output_.ml_ofi_30s = ml_ofi_30s;
    output_.ml_ofi_60s = ml_ofi_60s;
    output_.n_mid_moves_30s = n_mid_moves_30s;
    output_.n_mid_moves_60s = n_mid_moves_60s;
    output_.rv_30s = rv_30s;
    output_.rv_60s = rv_60s;
    output_.T = predicted_movement;
    output_.alpha = alpha;

    publish_signal(current_tick, msg_ms, txn_ms);
}

void orderbook_script::run_execution()
{
}

void orderbook_script::run_execution(const signal_snapshot&)
{
}

void orderbook_script::destroy()
{
    script::destroy();
}
