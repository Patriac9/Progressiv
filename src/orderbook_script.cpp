//
// Created by zagym on 09/08/2026.
//
#include "orderbook_script.h"
#include "loader.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string>

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
}

orderbook_script::orderbook_script() = default;

orderbook_script::~orderbook_script() = default;

void orderbook_script::init(std::string instId)
{
    script::init(instId);
    has_prev_ = false;
    model_ready_ = false;
    q_count_ = {};
    r2_count_ = {};
    absorb_dm_sum_ = {};
    row_total_ = {};
    g1_ = {};
    adjustment_ = {};
    ml_ofi.clear();
    ml_ofi_time_ms_.clear();
    ml_ofi_start_ms_ = 0;
    ml_ofi_1s = ml_ofi_5s = ml_ofi_15s = ml_ofi_30s = ml_ofi_60s = 0.f;
    z_ofi_time_ms_.clear();
    z_ofi_start_ms_ = 0;
    z_hist_1s_.clear();
    z_hist_5s_.clear();
    z_hist_15s_.clear();
    z_hist_30s_.clear();
    z_hist_60s_.clear();
    z_ml_ofi_1s = z_ml_ofi_5s = z_ml_ofi_15s = z_ml_ofi_30s = z_ml_ofi_60s = 0.f;
    aggressor_buy_1s = aggressor_sell_1s = aggressor_net_1s = aggressor_imb_1s = aggressor_imb_5s = 0.f;
    spread_ticks = bid_gap_ticks = ask_gap_ticks = gap_imb = 0.f;
    bid_slope = ask_slope = slope_imb = 0.f;
    impact_start_ms_ = 0;
    impact_time_ms_.clear();
    impact_ofi_x_.clear();
    impact_dmid_.clear();
    mid_hist_time_ms_.clear();
    mid_hist_.clear();
    mid_hist_start_ms_ = 0;
    n_mid_moves_30s = n_mid_moves_60s = rv_30s = rv_60s = 0.f;
    impact_ofi_1s = impact_ofi_5s = impact_ofi_15s = impact_ofi_30s = impact_ofi_60s = 0.f;
    impact_trade_1s = impact_trade_5s = impact_trade_15s = impact_trade_30s = impact_trade_60s = 0.f;
    ofi_l1 = 0.f;
    predicted_movement = 0.f;
    alpha = 0.f;
    position_ = {};
    tau = 0.407647f;

    tick_size = progressiv_ -> interface_ ->get_tick_size(instId);
    const auto tf = progressiv_->interface_->get_tick_filter(progressiv_->interface_->trade_symbol());
    trade_tick_size_ = static_cast<float>(tf.tick_size);
    trade_price_decimals_ = tf.decimals;
    load_model_params();
}

int orderbook_script::imb_bin(float imbalance)
{
    const float x = std::clamp(imbalance, 0.f, 1.f - 1e-6f);
    return static_cast<int>(x * kImbBins);
}

void orderbook_script::compute_book_shape()
{
    spread_ticks = bid_gap_ticks = ask_gap_ticks = gap_imb = 0.f;
    bid_slope = ask_slope = slope_imb = 0.f;
    if (bids.empty() || asks.empty() || tick_size <= 0.f)
        return;

    spread_ticks = (asks[0].price - bids[0].price) / tick_size;

    auto gap_ticks = [&](const std::vector<orderbook_level>& levels, bool is_bid) -> float
    {
        float gap = 0.f;
        for (size_t i = 0; i + 1 < levels.size(); ++i)
        {
            const float dp = is_bid
                ? (levels[i].price - levels[i + 1].price)
                : (levels[i + 1].price - levels[i].price);
            const float ticks = dp / tick_size;
            if (ticks > 1.f)
                gap += ticks - 1.f;
        }
        return gap;
    };

    // 累计深度对距 mid 的 tick 距离做 OLS，斜率大 = 该侧更厚（Næs-Skjeltorp 的离散近似）
    auto depth_slope = [&](const std::vector<orderbook_level>& levels, bool is_bid) -> float
    {
        double sx = 0.0;
        double sy = 0.0;
        double sxx = 0.0;
        double sxy = 0.0;
        int n = 0;
        float cum = 0.f;
        for (const auto& lv : levels)
        {
            cum += lv.quantity;
            const float dist = is_bid ? (mid_price - lv.price) : (lv.price - mid_price);
            const float x = dist / tick_size;
            if (x < 0.f)
                continue;
            sx += x;
            sy += static_cast<double>(cum);
            sxx += static_cast<double>(x) * x;
            sxy += static_cast<double>(x) * cum;
            ++n;
        }
        if (n < 2)
            return 0.f;
        const double denom = static_cast<double>(n) * sxx - sx * sx;
        if (std::fabs(denom) < 1e-12)
            return 0.f;
        return static_cast<float>((static_cast<double>(n) * sxy - sx * sy) / denom);
    };

    bid_gap_ticks = gap_ticks(bids, true);
    ask_gap_ticks = gap_ticks(asks, false);
    const float gap_sum = bid_gap_ticks + ask_gap_ticks;
    gap_imb = gap_sum > 1e-12f
        ? (ask_gap_ticks - bid_gap_ticks) / (gap_sum + 1.f)
        : 0.f;

    bid_slope = depth_slope(bids, true);
    ask_slope = depth_slope(asks, false);
    const float slope_abs = std::fabs(bid_slope) + std::fabs(ask_slope);
    slope_imb = slope_abs > 1e-12f
        ? (bid_slope - ask_slope) / (slope_abs + 1e-6f)
        : 0.f;
}

void orderbook_script::observe_transition(int from, int to, float d_mid)
{
    if (from < 0 || from >= kImbBins || to < 0 || to >= kImbBins)
        return;

    row_total_[from] += 1.0;
    if (std::fabs(d_mid) < kMidEps)
    {
        // transient: mid 不变，仅 imbalance 转移
        q_count_[from][to] += 1.0;
    }
    else
    {
        // absorbing: mid 发生变化
        absorb_dm_sum_[from] += static_cast<double>(d_mid);
        r2_count_[from][to] += 1.0;
    }
}

bool orderbook_script::solve_linear(std::array<std::array<double, kImbBins>, kImbBins> a,
                                    std::array<double, kImbBins> b,
                                    std::array<double, kImbBins>& x)
{
    // 高斯消元求解 (I-Q) x = b
    for (int col = 0; col < kImbBins; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row < kImbBins; ++row)
        {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col]))
                pivot = row;
        }
        if (std::fabs(a[pivot][col]) < 1e-12)
            return false;

        std::swap(a[col], a[pivot]);
        std::swap(b[col], b[pivot]);

        const double div = a[col][col];
        for (int j = col; j < kImbBins; ++j)
            a[col][j] /= div;
        b[col] /= div;

        for (int row = 0; row < kImbBins; ++row)
        {
            if (row == col)
                continue;
            const double factor = a[row][col];
            for (int j = col; j < kImbBins; ++j)
                a[row][j] -= factor * a[col][j];
            b[row] -= factor * b[col];
        }
    }
    x = b;
    return true;
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
        std::cerr << "param.mod not found; using built-in horizon/tp_offset/q_ord\n";
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
        std::cout << "loaded param.mod horizon=" << horizon
                  << " tp_offset=" << tp_offset
                  << " q_ord=" << q_ord
                  << " enable_dynamic_risk_management="
                  << (enable_dynamic_risk_management ? "true" : "false") << '\n';
    }

    const std::string t_text = load_first({
        "T_Param",
        "models/T_Param",
        "pytool/models/T_Param",
        "../pytool/models/T_Param",
    });
    if (t_text.empty())
        std::cerr << "T_Param not found; T() will return 0\n";
    else
    {
        for_each_kv(t_text, [&](const std::string& key, const std::string& value)
        {
            const float v = parse_float(value);
            if (key == "intercept") t_coef_.intercept = v;
            else if (key == "obi") t_coef_.obi = v;
            else if (key == "ml_ofi_5s" || key == "ofi_5s") t_coef_.ml_ofi_5s = v;
            else if (key == "ml_ofi_15s") t_coef_.ml_ofi_15s = v;
            else if (key == "tau") tau = v;
        });
        std::cout << "loaded T_Param intercept=" << t_coef_.intercept
                  << " obi=" << t_coef_.obi
                  << " ml_ofi_5s=" << t_coef_.ml_ofi_5s
                  << " ml_ofi_15s=" << t_coef_.ml_ofi_15s
                  << " tau=" << tau << '\n';
    }

    const std::string a_text = load_first({
        "alpha_Param",
        "models/alpha_Param",
        "pytool/models/alpha_Param",
        "../pytool/models/alpha_Param",
    });
    if (a_text.empty())
        std::cerr << "alpha_Param not found; calculate_alpha() will use defaults\n";
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
        std::cout << "loaded alpha_Param kind=" << (alpha_coef_.is_exp ? "exp" : "linear")
                  << " intercept=" << alpha_coef_.intercept
                  << " clip=[" << alpha_coef_.clip_min << "," << alpha_coef_.clip_max << "]\n";
    }
}

float orderbook_script::T(const T_parameter& param_)
{
    return t_coef_.intercept
         + t_coef_.obi * param_.obi
         + t_coef_.ml_ofi_5s * param_.ml_ofi_5s
         + t_coef_.ml_ofi_15s * param_.ml_ofi_15s;
}

float orderbook_script::calculate_alpha(const alpha_parameter& param_)
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

void orderbook_script::recompute_stoikov_model()
{
    // 构造 Q、R2 概率，以及吸收期望回报向量 r
    std::array<std::array<double, kImbBins>, kImbBins> Q{};
    std::array<std::array<double, kImbBins>, kImbBins> R2{};
    std::array<double, kImbBins> r{};

    for (int i = 0; i < kImbBins; ++i)
    {
        if (row_total_[i] < 1.0)
        {
            Q[i][i] = 1.0; // 无样本时自环，避免奇异
            continue;
        }
        for (int j = 0; j < kImbBins; ++j)
        {
            Q[i][j] = q_count_[i][j] / row_total_[i];
            R2[i][j] = r2_count_[i][j] / row_total_[i];
        }
        r[i] = absorb_dm_sum_[i] / row_total_[i];
    }

    // G1 = (I - Q)^{-1} r
    std::array<std::array<double, kImbBins>, kImbBins> I_minus_Q{};
    for (int i = 0; i < kImbBins; ++i)
    {
        for (int j = 0; j < kImbBins; ++j)
            I_minus_Q[i][j] = (i == j ? 1.0 : 0.0) - Q[i][j];
    }

    if (!solve_linear(I_minus_Q, r, g1_))
    {
        model_ready_ = false;
        return;
    }

    // 对称化：g1(i) = -g1(n-1-i) 的平均（Stoikov 原文 symmetrize）
    for (int i = 0; i < kImbBins / 2; ++i)
    {
        const int j = kImbBins - 1 - i;
        const double v = 0.5 * (g1_[i] - g1_[j]);
        g1_[i] = v;
        g1_[j] = -v;
    }
    if (kImbBins % 2 == 1)
        g1_[kImbBins / 2] = 0.0;

    // B = (I - Q)^{-1} R2，逐列求解
    std::array<std::array<double, kImbBins>, kImbBins> B{};
    for (int col = 0; col < kImbBins; ++col)
    {
        std::array<double, kImbBins> rhs{};
        for (int row = 0; row < kImbBins; ++row)
            rhs[row] = R2[row][col];

        // 每次需要新鲜的 (I-Q)
        std::array<std::array<double, kImbBins>, kImbBins> A{};
        for (int i = 0; i < kImbBins; ++i)
            for (int j = 0; j < kImbBins; ++j)
                A[i][j] = (i == j ? 1.0 : 0.0) - Q[i][j];

        std::array<double, kImbBins> col_vec{};
        if (!solve_linear(A, rhs, col_vec))
        {
            model_ready_ = false;
            return;
        }
        for (int row = 0; row < kImbBins; ++row)
            B[row][col] = col_vec[row];
    }

    // 对称化 B（关于 imbalance 翻转）
    std::array<std::array<double, kImbBins>, kImbBins> B_sym{};
    for (int i = 0; i < kImbBins; ++i)
    {
        for (int j = 0; j < kImbBins; ++j)
        {
            const int ii = kImbBins - 1 - i;
            const int jj = kImbBins - 1 - j;
            B_sym[i][j] = 0.5 * (B[i][j] + B[ii][jj]);
        }
    }
    B = B_sym;

    // adjustment = G1 + B G1 + ... + B^{H-1} G1
    adjustment_ = g1_;
    std::array<double, kImbBins> term = g1_;
    for (int h = 1; h < kHorizon; ++h)
    {
        std::array<double, kImbBins> next{};
        for (int i = 0; i < kImbBins; ++i)
        {
            double s = 0.0;
            for (int j = 0; j < kImbBins; ++j)
                s += B[i][j] * term[j];
            next[i] = s;
        }
        term = next;
        for (int i = 0; i < kImbBins; ++i)
            adjustment_[i] += term[i];
    }

    model_ready_ = true;
}

void orderbook_script::run()
{
    script::run();
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
    const float quantity_ask = asks[0].quantity;
    const float quantity_bid = bids[0].quantity;


    float bids_sum = 0.f;
    float asks_sum = 0.f;
    for (const auto& bid : bids)
        bids_sum += bid.quantity;
    for (const auto& ask : asks)
        asks_sum += ask.quantity;

    const float depth_sum = bids_sum + asks_sum;
    if (depth_sum <= 0.f)
        return;

    // 多档定义（与 depth_sum 一致）:
    // I = ΣQb / (ΣQb + ΣQa) ∈ [0,1)
    // weighted mid = (Pa·ΣQb + Pb·ΣQa) / (ΣQb + ΣQa)
    obi = (bids_sum - asks_sum) / depth_sum;
    weighed_obi = obi;
    mid_price = 0.5f * (ask_px + bid_px);
    weighed_mid_price = (ask_px * bids_sum + bid_px * asks_sum) / depth_sum;
    compute_book_shape();

    const int bin = imb_bin(obi);

    if (has_prev_)
    {
        observe_transition(prev_imb_bin_, bin, mid_price - prev_mid_);
        double samples = 0.0;
        for (double v : row_total_)
            samples += v;
        if (samples >= static_cast<double>(kImbBins * 20))
            recompute_stoikov_model();
    }

    // microprice 随多档 I 变化：
    // - 模型就绪: M + adjustment[bin(I_multi)]
    // - 冷启动: 多档加权中间价
    if (model_ready_)
        microprice = mid_price + static_cast<float>(adjustment_[bin]);
    else
        microprice = weighed_mid_price;

    float buyer_contribution = 0.f;
    float seller_contribution = 0.f;
    if (has_prev_ && !prev_bids.empty() && !prev_asks.empty())
    {
        const float prev_bid_px = prev_bids[0].price;
        const float prev_ask_px = prev_asks[0].price;

        // Cont L1 OFI: ΔVb - ΔVa
        if (bid_px > prev_bid_px)
            buyer_contribution = quantity_bid;
        else if (bid_px < prev_bid_px)
            buyer_contribution = -prev_bid_quantity;
        else
            buyer_contribution = quantity_bid - prev_bid_quantity;

        if (ask_px < prev_ask_px)
            seller_contribution = -quantity_ask;
        else if (ask_px > prev_ask_px)
            seller_contribution = prev_ask_quantity;
        else
            seller_contribution = -(quantity_ask - prev_ask_quantity);
    }

    ofi_l1 = buyer_contribution + seller_contribution;

    // Multilevel Cont OFI（仅前 10 档），插入 ml_ofi 最前端，并聚合时间窗
    float multilevel_ofi = 0.f;
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
            if (b_px > prev_b_px)
                multilevel_ofi += b_qty;
            else if (b_px < prev_b_px)
                multilevel_ofi -= prev_b_qty;
            else
                multilevel_ofi += b_qty - prev_b_qty;

            const float a_px = asks[i].price;
            const float a_qty = asks[i].quantity;
            const float prev_a_px = prev_asks[i].price;
            const float prev_a_qty = prev_asks[i].quantity;
            if (a_px < prev_a_px)
                multilevel_ofi -= a_qty;
            else if (a_px > prev_a_px)
                multilevel_ofi += prev_a_qty;
            else
                multilevel_ofi -= (a_qty - prev_a_qty);
        }
    }

    const long long now_ms = progressiv_->current_orderbook.message_time != 0
        ? progressiv_->current_orderbook.message_time
        : progressiv_->current_orderbook.transact_time;

    ml_ofi.insert(ml_ofi.begin(), multilevel_ofi);
    ml_ofi_time_ms_.insert(ml_ofi_time_ms_.begin(), now_ms);
    if (ml_ofi_start_ms_ == 0)
        ml_ofi_start_ms_ = now_ms;

    while (!ml_ofi_time_ms_.empty() && now_ms - ml_ofi_time_ms_.back() > kMlOfiMaxMs)
    {
        ml_ofi.pop_back();
        ml_ofi_time_ms_.pop_back();
    }

    auto sum_ofi_window = [&](long long window_ms) -> std::pair<float, float>
    {
        float sum = 0.f;
        float abs_sum = 0.f;
        for (size_t i = 0; i < ml_ofi.size(); ++i)
        {
            if (now_ms - ml_ofi_time_ms_[i] > window_ms)
                break;
            sum += ml_ofi[i];
            abs_sum += std::fabs(ml_ofi[i]);
        }
        return {sum, abs_sum};
    };

    // 归一化: Σ ofi / Σ |ofi| ∈ [-1, 1]
    // 冷启动：从首次采样起未满窗口时长则输出 0（不依赖裁剪后的 buffer span）
    auto normalize_ofi_window = [&](long long window_ms) -> float
    {
        if (ml_ofi_start_ms_ == 0 || now_ms - ml_ofi_start_ms_ < window_ms)
            return 0.f;

        const auto [sum, abs_sum] = sum_ofi_window(window_ms);
        return abs_sum > 1e-12f ? sum / abs_sum : 0.f;
    };

    ml_ofi_1s = normalize_ofi_window(1000);
    ml_ofi_5s = normalize_ofi_window(5000);
    ml_ofi_15s = normalize_ofi_window(15000);
    ml_ofi_30s = normalize_ofi_window(30000);
    ml_ofi_60s = normalize_ofi_window(60000);

    // 滚动 1min 历史后计算各窗口因子的 z-score；未满 1min 为 0
    z_ofi_time_ms_.insert(z_ofi_time_ms_.begin(), now_ms);
    z_hist_1s_.insert(z_hist_1s_.begin(), ml_ofi_1s);
    z_hist_5s_.insert(z_hist_5s_.begin(), ml_ofi_5s);
    z_hist_15s_.insert(z_hist_15s_.begin(), ml_ofi_15s);
    z_hist_30s_.insert(z_hist_30s_.begin(), ml_ofi_30s);
    z_hist_60s_.insert(z_hist_60s_.begin(), ml_ofi_60s);
    if (z_ofi_start_ms_ == 0)
        z_ofi_start_ms_ = now_ms;

    while (!z_ofi_time_ms_.empty() && now_ms - z_ofi_time_ms_.back() > kZOfiWindowMs)
    {
        z_ofi_time_ms_.pop_back();
        z_hist_1s_.pop_back();
        z_hist_5s_.pop_back();
        z_hist_15s_.pop_back();
        z_hist_30s_.pop_back();
        z_hist_60s_.pop_back();
    }

    auto zscore_1min = [&](const std::vector<float>& hist, float x) -> float
    {
        if (z_ofi_start_ms_ == 0 || now_ms - z_ofi_start_ms_ < kZOfiWindowMs)
            return 0.f;
        if (hist.size() < 2)
            return 0.f;

        double mean = 0.0;
        for (float v : hist)
            mean += static_cast<double>(v);
        mean /= static_cast<double>(hist.size());

        double var = 0.0;
        for (float v : hist)
        {
            const double d = static_cast<double>(v) - mean;
            var += d * d;
        }
        var /= static_cast<double>(hist.size());
        const double stddev = std::sqrt(var);
        if (stddev < 1e-12)
            return 0.f;
        return static_cast<float>((static_cast<double>(x) - mean) / stddev);
    };

    z_ml_ofi_1s = zscore_1min(z_hist_1s_, ml_ofi_1s);
    z_ml_ofi_5s = zscore_1min(z_hist_5s_, ml_ofi_5s);
    z_ml_ofi_15s = zscore_1min(z_hist_15s_, ml_ofi_15s);
    z_ml_ofi_30s = zscore_1min(z_hist_30s_, ml_ofi_30s);
    z_ml_ofi_60s = zscore_1min(z_hist_60s_, ml_ofi_60s);
    aggressor_buy_1s = aggressor_sell_1s = aggressor_net_1s = aggressor_imb_1s = aggressor_imb_5s = 0.f;
    if (progressiv_ && progressiv_->interface_)
    {
        try
        {
            const auto flow_1s = progressiv_->interface_->get_ws_aggressor_flow(1000);
            aggressor_buy_1s = flow_1s.buy_qty;
            aggressor_sell_1s = flow_1s.sell_qty;
            aggressor_net_1s = flow_1s.net_qty;
            aggressor_imb_1s = flow_1s.imbalance;
            aggressor_imb_5s = progressiv_->interface_->get_ws_aggressor_flow(5000).imbalance;
        }
        catch (const std::exception&)
        {
        }
    }

    // Cont 冲击系数: Δmid = λ · ofi_l1 + ε，无截距 OLS
    // Kyle 冲击系数: Δmid_W / aggressor_net_W
    impact_ofi_1s = impact_ofi_5s = impact_ofi_15s = impact_ofi_30s = impact_ofi_60s = 0.f;
    impact_trade_1s = impact_trade_5s = impact_trade_15s = impact_trade_30s = impact_trade_60s = 0.f;

    mid_hist_time_ms_.insert(mid_hist_time_ms_.begin(), now_ms);
    mid_hist_.insert(mid_hist_.begin(), mid_price);
    if (mid_hist_start_ms_ == 0)
        mid_hist_start_ms_ = now_ms;
    while (!mid_hist_time_ms_.empty() && now_ms - mid_hist_time_ms_.back() > kImpactMaxMs)
    {
        mid_hist_time_ms_.pop_back();
        mid_hist_.pop_back();
    }

    n_mid_moves_30s = n_mid_moves_60s = rv_30s = rv_60s = 0.f;
    auto mid_rv_moves = [&](long long window_ms, float& rv, float& n_moves)
    {
        rv = 0.f;
        n_moves = 0.f;
        if (mid_hist_start_ms_ == 0 || now_ms - mid_hist_start_ms_ < window_ms)
            return;
        if (mid_hist_.size() < 2)
            return;
        double sumsq = 0.0;
        int moves = 0;
        for (size_t i = 0; i + 1 < mid_hist_.size(); ++i)
        {
            if (now_ms - mid_hist_time_ms_[i] > window_ms)
                break;
            if (now_ms - mid_hist_time_ms_[i + 1] > window_ms)
                break;
            const float d = mid_hist_[i] - mid_hist_[i + 1];
            sumsq += static_cast<double>(d) * static_cast<double>(d);
            if (std::fabs(d) > kMidEps)
                ++moves;
        }
        rv = static_cast<float>(std::sqrt(sumsq));
        n_moves = static_cast<float>(moves);
    };
    mid_rv_moves(30000, rv_30s, n_mid_moves_30s);
    mid_rv_moves(60000, rv_60s, n_mid_moves_60s);

    if (has_prev_)
    {
        const float d_mid = mid_price - prev_mid_;
        impact_time_ms_.insert(impact_time_ms_.begin(), now_ms);
        impact_ofi_x_.insert(impact_ofi_x_.begin(), ofi_l1);
        impact_dmid_.insert(impact_dmid_.begin(), d_mid);
        if (impact_start_ms_ == 0)
            impact_start_ms_ = now_ms;

        while (!impact_time_ms_.empty() && now_ms - impact_time_ms_.back() > kImpactMaxMs)
        {
            impact_time_ms_.pop_back();
            impact_ofi_x_.pop_back();
            impact_dmid_.pop_back();
        }

        auto ols_lambda = [&](long long window_ms) -> float
        {
            if (impact_start_ms_ == 0 || now_ms - impact_start_ms_ < window_ms)
                return 0.f;

            double sxx = 0.0;
            double sxy = 0.0;
            for (size_t i = 0; i < impact_time_ms_.size(); ++i)
            {
                if (now_ms - impact_time_ms_[i] > window_ms)
                    break;
                const double x = static_cast<double>(impact_ofi_x_[i]);
                const double y = static_cast<double>(impact_dmid_[i]);
                sxx += x * x;
                sxy += x * y;
            }
            if (sxx < 1e-18)
                return 0.f;
            return static_cast<float>(sxy / sxx);
        };

        impact_ofi_1s = ols_lambda(1000);
        impact_ofi_5s = ols_lambda(5000);
        impact_ofi_15s = ols_lambda(15000);
        impact_ofi_30s = ols_lambda(30000);
        impact_ofi_60s = ols_lambda(60000);
    }

    auto mid_delta = [&](long long window_ms) -> float
    {
        if (mid_hist_.size() < 2)
            return 0.f;

        float mid_old = mid_hist_.back();
        bool found = false;
        for (size_t i = 0; i < mid_hist_time_ms_.size(); ++i)
        {
            if (now_ms - mid_hist_time_ms_[i] >= window_ms)
            {
                mid_old = mid_hist_[i];
                found = true;
                break;
            }
        }
        if (!found)
            return 0.f;
        return mid_price - mid_old;
    };

    auto kyle_lambda = [&](long long window_ms) -> float
    {
        if (!progressiv_ || !progressiv_->interface_)
            return 0.f;
        const float dmid = mid_delta(window_ms);
        if (dmid == 0.f && mid_hist_.size() < 2)
            return 0.f;
        try
        {
            const auto flow = progressiv_->interface_->get_ws_aggressor_flow(window_ms);
            if (std::fabs(flow.net_qty) < 1e-12f)
                return 0.f;
            return dmid / flow.net_qty;
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    };

    impact_trade_1s = kyle_lambda(1000);
    impact_trade_5s = kyle_lambda(5000);
    impact_trade_15s = kyle_lambda(15000);
    impact_trade_30s = kyle_lambda(30000);
    impact_trade_60s = kyle_lambda(60000);

    micro_price_offset = (microprice - mid_price) / tick_size;

    prev_bid_quantity = quantity_bid;
    prev_ask_quantity = quantity_ask;
    prev_mid_ = mid_price;
    prev_imb_bin_ = bin;
    has_prev_ = true;
    prev_asks = asks;
    prev_bids = bids;
    //------------------------------------------------------------------------------------------------------------------

    predicted_movement = T({
        .obi = obi,
        .ml_ofi_5s = ml_ofi_5s,
        .ml_ofi_15s = ml_ofi_15s,
    });
    alpha = calculate_alpha({
        .n_mid_moves_30s = n_mid_moves_30s,
        .n_mid_moves_60s = n_mid_moves_60s,
        .rv_30s = rv_30s,
        .rv_60s = rv_60s,
        .abs_T = std::fabs(predicted_movement),
    });

    const auto& signal = progressiv_->interface_->signal_symbol();
    const auto& trade  = progressiv_->interface_->trade_symbol();

    if (progressiv_ -> get_enable_live_action())
    {
        std::vector<order_info> orders;
        std::vector<position_info> positions;
        try
        {
            orders = progressiv_->interface_->ws_open_orders(trade);
            positions = progressiv_->interface_->ws_get_positions(trade);
        }
        catch (const std::exception& ex)
        {
            // 例如 -2015 IP/权限：打日志并跳过本 tick，避免整个进程退出
            std::cerr << "live query rejected: " << ex.what() << '\n';
            return;
        }

        auto has_open_pos = [&]() -> bool
        {
            for (const auto& p : positions)
            {
                if (std::fabs(p.position_amt) > 1e-12f)
                    return true;
            }
            return false;
        };

        // 仓位刚平时：撤掉剩余 TP/SL/追价单，本 tick 不再开新仓
        if (position_.time > 0.f && !has_open_pos())
        {
            try
            {
                if (!orders.empty())
                    progressiv_->interface_->ws_cancel_all_open_orders(trade);
            }
            catch (const std::exception& ex)
            {
                std::cerr << "cancel-all after flatten rejected: " << ex.what() << '\n';
            }
            position_ = {};
        }
        else if (orders.empty() && positions.empty())
        {
            position_ = {};
            const auto trade_book = progressiv_->interface_->get_ws_trade_orderbook();
            if (trade_book.bids.empty() || trade_book.asks.empty())
                ;
            else
            {
                const float bid = trade_book.bids.front().price;
                const float ask = trade_book.asks.front().price;
                const double px_tick = trade_tick_size_ > 0.f ? trade_tick_size_ : tick_size;
                const int px_decimals = trade_price_decimals_;

                auto place_post_only = [&](const char* side)
                {
                    const bool is_buy = side[0] == 'B';
                    std::string px_str;
                    double px = 0.0;
                    if (!same_side_bbo(is_buy, bid, ask, px_tick, px_decimals, px_str, px))
                        return;

                    float qty = static_cast<float>(q_ord / px);
                    qty = std::floor(qty * 1000.f + 1e-6f) / 1000.f;
                    if (qty < 0.001f)
                        return;

                    order_request req;
                    req.symbol = trade;
                    req.side = side;
                    req.type = "LIMIT";
                    req.time_in_force = "GTX";
                    req.price = px_str;
                    req.quantity = format_decimal(qty, 3);
                    try
                    {
                        progressiv_->interface_->ws_place_order(req);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "post-only " << side << " " << req.price
                                  << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                  << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                  << " rejected: " << ex.what() << '\n';
                    }
                };

                if (std::abs(predicted_movement) >= tau && predicted_movement > 0) // long: 买挂买一
                    place_post_only("BUY");
                else if (std::abs(predicted_movement) >= tau && predicted_movement < 0) // short: 卖挂卖一
                    place_post_only("SELL");
            }
        }
        else if (positions.empty() && !orders.empty())
        {
            position_ = {};
            const auto& o = orders[0];
            // 未成交挂单用 side（BUY/SELL）；单向持仓下 positionSide 常为 BOTH
            const bool is_buy = (o.side == "BUY" || o.position_side == "LONG");
            const bool is_sell = (o.side == "SELL" || o.position_side == "SHORT");

            const double px_tick = trade_tick_size_ > 0.f ? trade_tick_size_ : tick_size;
            const int px_decimals = trade_price_decimals_;

            auto trade_book = progressiv_->interface_->get_ws_trade_orderbook();
            if (trade_book.bids.empty() || trade_book.asks.empty())
                ;
            else if (is_buy)
            {
                if (std::abs(predicted_movement) < tau)
                {
                    try
                    {
                        progressiv_->interface_->ws_cancel_order(o.order_id, trade);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "cancel BUY rejected: " << ex.what() << '\n';
                    }
                }
                else if (std::abs(predicted_movement) >= tau)
                {
                    const float bid = trade_book.bids.front().price;
                    const float ask = trade_book.asks.front().price;
                    float order_px = 0.f;
                    try { order_px = std::stof(o.price); } catch (...) {}
                    double px = 0.0;
                    std::string px_str;
                    if (same_side_bbo(true, bid, ask, px_tick, px_decimals, px_str, px)
                        && std::fabs(px - order_px) > px_tick * 0.5)
                    {
                        try
                        {
                            order_request req;
                            req.symbol = trade;
                            req.side = "BUY";
                            req.price = px_str;
                            req.quantity = o.orig_qty.empty() ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3) : o.orig_qty;
                            progressiv_->interface_->ws_modify_order(o.order_id, req);
                        }
                        catch (const std::exception& ex)
                        {
                            std::cerr << "reprice BUY " << px_str
                                      << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                      << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                      << " rejected: " << ex.what() << '\n';
                        }
                    }
                }
            }
            else if (is_sell)
            {
                if (std::abs(predicted_movement) < tau)
                {
                    try
                    {
                        progressiv_->interface_->ws_cancel_order(o.order_id, trade);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "cancel SELL rejected: " << ex.what() << '\n';
                    }
                }
                else if (std::abs(predicted_movement) >= tau)
                {
                    const float bid = trade_book.bids.front().price;
                    const float ask = trade_book.asks.front().price;
                    float order_px = 0.f;
                    try { order_px = std::stof(o.price); } catch (...) {}
                    double px = 0.0;
                    std::string px_str;
                    if (same_side_bbo(false, bid, ask, px_tick, px_decimals, px_str, px)
                        && std::fabs(px - order_px) > px_tick * 0.5)
                    {
                        try
                        {
                            order_request req;
                            req.symbol = trade;
                            req.side = "SELL";
                            req.price = px_str;
                            req.quantity = o.orig_qty.empty() ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3) : o.orig_qty;
                            progressiv_->interface_->ws_modify_order(o.order_id, req);
                        }
                        catch (const std::exception& ex)
                        {
                            std::cerr << "reprice SELL " << px_str
                                      << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                      << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                      << " rejected: " << ex.what() << '\n';
                        }
                    }
                }
            }
        }
        else if (!positions.empty())
        {
            const position_info& pos = positions.front();
            const float amt = pos.position_amt;
            if (std::fabs(amt) < 1e-12f)
            {
                try
                {
                    if (!orders.empty())
                        progressiv_->interface_->ws_cancel_all_open_orders(trade);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "cancel-all on zero position rejected: " << ex.what() << '\n';
                }
                position_ = {};
            }
            else
            {
                const float dir = amt > 0.f ? 1.f : -1.f;
                const char* close_side = dir > 0.f ? "SELL" : "BUY";
                const float now_sec = static_cast<float>(now_ms) / 1000.f;
                const double px_tick = trade_tick_size_ > 0.f ? trade_tick_size_ : tick_size;
                const int px_decimals = trade_price_decimals_;
                float qty = std::fabs(amt);
                qty = std::floor(qty * 1000.f + 1e-6f) / 1000.f;

                auto trade_book = progressiv_->interface_->get_ws_trade_orderbook();
                const bool book_ok = !trade_book.bids.empty() && !trade_book.asks.empty();
                const float bid = book_ok ? trade_book.bids.front().price : 0.f;
                const float ask = book_ok ? trade_book.asks.front().price : 0.f;

                auto cancel_order = [&](long long order_id)
                {
                    try
                    {
                        progressiv_->interface_->ws_cancel_order(order_id, trade);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "cancel reduce-only rejected: " << ex.what() << '\n';
                    }
                };

                auto cancel_all = [&]()
                {
                    for (const auto& o : orders)
                        cancel_order(o.order_id);
                    orders.clear();
                };

                auto place_reduce_post_only = [&](const char* side, float raw_px) -> bool
                {
                    if (!book_ok || qty < 0.001f || raw_px <= 0.f || !(bid < ask))
                        return false;
                    const bool is_buy = side[0] == 'B';
                    const long long n = tick_index(raw_px, px_tick, !is_buy);
                    const double px = static_cast<double>(n) * px_tick;
                    if (n <= 0)
                        return false;
                    if (is_buy && !(px < ask))
                        return false;
                    if (!is_buy && !(px > bid))
                        return false;
                    const std::string px_str = format_tick_n(n, px_tick, px_decimals);

                    order_request req;
                    req.symbol = trade;
                    req.side = side;
                    req.type = "LIMIT";
                    req.time_in_force = "GTX";
                    req.reduce_only = true;
                    req.price = px_str;
                    req.quantity = format_decimal(qty, 3);
                    try
                    {
                        progressiv_->interface_->ws_place_order(req);
                        return true;
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "reduce-only " << side << " " << req.price
                                  << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                  << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                  << " rejected: " << ex.what() << '\n';
                        return false;
                    }
                };

                auto order_px = [](const order_info& o) -> float
                {
                    try { return std::stof(o.price); }
                    catch (...) { return 0.f; }
                };

                auto keep_or_reprice = [&](const std::string& price_str, double target_px)
                {
                    const order_info* working = nullptr;
                    for (const auto& o : orders)
                    {
                        if (o.side == close_side)
                        {
                            working = &o;
                            break;
                        }
                    }
                    for (const auto& o : orders)
                    {
                        if (working && o.order_id == working->order_id)
                            continue;
                        cancel_order(o.order_id);
                    }
                    if (working == nullptr)
                    {
                        place_reduce_post_only(close_side, static_cast<float>(target_px));
                        return;
                    }
                    if (std::fabs(order_px(*working) - target_px) <= px_tick * 0.5)
                        return;
                    try
                    {
                        order_request req;
                        req.symbol = trade;
                        req.side = close_side;
                        req.price = price_str;
                        req.quantity = format_decimal(qty, 3);
                        progressiv_->interface_->ws_modify_order(working->order_id, req);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "reprice reduce-only rejected: " << ex.what() << '\n';
                    }
                };

                auto snap_exit = [&](double raw, std::string& str, double& px)
                {
                    const bool is_buy = close_side[0] == 'B';
                    const long long n = tick_index(raw, px_tick, !is_buy);
                    px = static_cast<double>(n) * px_tick;
                    str = format_tick_n(n, px_tick, px_decimals);
                };

                auto sync_reduce = [&](const order_info* o, double target_px, const std::string& price_str)
                {
                    if (o == nullptr)
                    {
                        place_reduce_post_only(close_side, static_cast<float>(target_px));
                        return;
                    }
                    if (std::fabs(order_px(*o) - target_px) <= px_tick * 0.5)
                        return;
                    try
                    {
                        order_request req;
                        req.symbol = trade;
                        req.side = close_side;
                        req.price = price_str;
                        req.quantity = format_decimal(qty, 3);
                        progressiv_->interface_->ws_modify_order(o->order_id, req);
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "reprice reduce-only rejected: " << ex.what() << '\n';
                    }
                };

                auto ensure_tp_sl = [&](double tp_snap, const std::string& tp_str,
                                        double sl_snap, const std::string& sl_str)
                {
                    const order_info* tp_ord = nullptr;
                    const order_info* sl_ord = nullptr;
                    for (const auto& o : orders)
                    {
                        if (o.side != close_side)
                        {
                            cancel_order(o.order_id);
                            continue;
                        }
                        const float p = order_px(o);
                        const double d_tp = std::fabs(p - tp_snap);
                        const double d_sl = std::fabs(p - sl_snap);
                        if (d_tp <= px_tick * 0.5)
                        {
                            if (tp_ord == nullptr)
                                tp_ord = &o;
                            else
                                cancel_order(o.order_id);
                        }
                        else if (d_sl <= px_tick * 0.5)
                        {
                            if (sl_ord == nullptr)
                                sl_ord = &o;
                            else
                                cancel_order(o.order_id);
                        }
                        else if (tp_ord == nullptr && d_tp <= d_sl)
                            tp_ord = &o;
                        else if (sl_ord == nullptr)
                            sl_ord = &o;
                        else
                            cancel_order(o.order_id);
                    }
                    sync_reduce(tp_ord, tp_snap, tp_str);
                    sync_reduce(sl_ord, sl_snap, sl_str);
                };

                const bool new_fill = (position_.time <= 0.f || position_.direction != dir);
                if (new_fill)
                {
                    position_.time = now_sec;
                    position_.direction = dir;
                    position_.close_flag = false;
                    position_.sl_hit = false;
                    position_.tp_offset = enable_dynamic_risk_management ? alpha : tp_offset;
                    cancel_all();
                }

                const float tp_px = pos.entry_price + dir * position_.tp_offset;
                const float sl_px = pos.entry_price - dir * position_.tp_offset;

                if (!position_.close_flag && !position_.sl_hit)
                {
                    // 与 trainer 一致：TP > SL > horizon > flip
                    if (book_ok)
                    {
                        const bool hit_tp = dir > 0.f ? (bid >= tp_px) : (ask <= tp_px);
                        const bool hit_sl = dir > 0.f ? (ask <= sl_px) : (bid >= sl_px);
                        if (!hit_tp && hit_sl)
                            position_.sl_hit = true;
                    }
                    if (!position_.sl_hit)
                    {
                        if (now_sec - position_.time >= horizon)
                            position_.close_flag = true;
                        if (predicted_movement * position_.direction < 0.f)
                            position_.close_flag = true;
                    }
                    if (position_.close_flag)
                        cancel_all();
                }

                if (!book_ok || qty < 0.001f)
                    ;
                else if (position_.close_flag)
                {
                    // 平多挂卖一、平空挂买一；价格变了立刻改到新的同向一档
                    std::string chase_str;
                    double chase_px = 0.0;
                    if (same_side_bbo(dir < 0.f, bid, ask, px_tick, px_decimals, chase_str, chase_px))
                        keep_or_reprice(chase_str, chase_px);
                }
                else if (position_.sl_hit)
                {
                    // 止损价若无法 GTX 挂上，贴同向一档退出；能挂则只留止损单
                    std::string sl_str;
                    double sl_snap = 0.0;
                    snap_exit(sl_px, sl_str, sl_snap);
                    const bool sl_maker = (close_side[0] == 'B') ? (sl_snap < ask) : (sl_snap > bid);
                    if (sl_maker)
                        keep_or_reprice(sl_str, sl_snap);
                    else
                    {
                        std::string chase_str;
                        double chase_px = 0.0;
                        if (same_side_bbo(dir < 0.f, bid, ask, px_tick, px_decimals, chase_str, chase_px))
                            keep_or_reprice(chase_str, chase_px);
                    }
                }
                else
                {
                    std::string tp_str, sl_str;
                    double tp_snap = 0.0, sl_snap = 0.0;
                    snap_exit(tp_px, tp_str, tp_snap);
                    snap_exit(sl_px, sl_str, sl_snap);
                    ensure_tp_sl(tp_snap, tp_str, sl_snap, sl_str);
                }
            }
        }

    }

    //todo: else :close all orders and positions
    else
    {
        std::vector<order_info> orders = progressiv_->interface_->ws_open_orders(trade);
        std::vector<position_info> positions = progressiv_ -> interface_ -> ws_get_positions(trade);
        if (!orders.empty())
        {
            try
            {
                progressiv_->interface_->ws_cancel_all_open_orders(trade);
            }
            catch (const std::exception& ex)
            {
                std::cerr << "cancel-all on disable rejected: " << ex.what() << '\n';
            }
        }
        if (!positions.empty())
        {
            for (const auto& pos : positions)
            {
                const float amt = pos.position_amt;
                if (std::fabs(amt) < 1e-12f)
                    continue;

                float qty = std::fabs(amt);
                qty = std::floor(qty * 1000.f + 1e-6f) / 1000.f;
                if (qty < 0.001f)
                    continue;

                order_request req;
                req.symbol = trade;
                req.side = amt > 0.f ? "SELL" : "BUY";
                req.type = "MARKET";
                req.reduce_only = true;
                req.quantity = format_decimal(qty, 3);
                try
                {
                    progressiv_->interface_->ws_place_order(req);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "market flatten rejected: " << ex.what() << '\n';
                }
            }
            try
            {
                progressiv_->interface_->ws_cancel_all_open_orders(trade);
            }
            catch (const std::exception& ex)
            {
                std::cerr << "cancel-all after flatten rejected: " << ex.what() << '\n';
            }
            position_ = {};
        }
    }


    //------------------------------------------------------------------------------------------------------------------


    output_.obi = obi;
    output_.mid_price = mid_price;
    output_.ml_ofi_1s = ml_ofi_1s;
    output_.ml_ofi_5s = ml_ofi_5s;
    output_.ml_ofi_15s = ml_ofi_15s;
    output_.ml_ofi_30s = ml_ofi_30s;
    output_.ml_ofi_60s = ml_ofi_60s;
    output_.aggressor_imb_1s = aggressor_imb_1s;
    output_.aggressor_imb_5s = aggressor_imb_5s;
    output_.spread_ticks = spread_ticks;
    output_.bid_gap_ticks = bid_gap_ticks;
    output_.ask_gap_ticks = ask_gap_ticks;
    output_.gap_imb = gap_imb;
    output_.bid_slope = bid_slope;
    output_.ask_slope = ask_slope;
    output_.slope_imb = slope_imb;
    output_.impact_ofi_1s = impact_ofi_1s;
    output_.impact_ofi_5s = impact_ofi_5s;
    output_.impact_ofi_15s = impact_ofi_15s;
    output_.impact_ofi_30s = impact_ofi_30s;
    output_.impact_ofi_60s = impact_ofi_60s;
    output_.impact_trade_1s = impact_trade_1s;
    output_.impact_trade_5s = impact_trade_5s;
    output_.impact_trade_15s = impact_trade_15s;
    output_.impact_trade_30s = impact_trade_30s;
    output_.impact_trade_60s = impact_trade_60s;
    output_.n_mid_moves_30s = n_mid_moves_30s;
    output_.n_mid_moves_60s = n_mid_moves_60s;
    output_.rv_30s = rv_30s;
    output_.rv_60s = rv_60s;
}

void orderbook_script::destroy()
{
    script::destroy();
}
