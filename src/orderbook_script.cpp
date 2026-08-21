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
    prev_bid_quantity = 0.f;
    prev_ask_quantity = 0.f;
    prev_asks.clear();
    prev_bids.clear();
    ml_ofi.clear();
    ml_ofi_time_ms_.clear();
    ml_ofi_start_ms_ = 0;
    ml_ofi_5s = ml_ofi_15s = 0.f;
    mid_hist_time_ms_.clear();
    mid_hist_.clear();
    mid_hist_start_ms_ = 0;
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
            else if (key == "ml_ofi_15s") t_coef_.ml_ofi_15s = v;
            else if (key == "tau") tau = v;
        });
        std::ostringstream oss;
        oss << "loaded T_Param intercept=" << t_coef_.intercept
            << " obi=" << t_coef_.obi
            << " ml_ofi_5s=" << t_coef_.ml_ofi_5s
            << " ml_ofi_15s=" << t_coef_.ml_ofi_15s
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
         + t_coef_.ml_ofi_15s * param_.ml_ofi_15s;
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
    snap.features = output_;

    std::lock_guard<std::mutex> lock(signal_mu_);
    snap.seq = signal_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    published_ = snap;
    signal_cv_.notify_all();
}

bool orderbook_script::wait_for_signal(uint64_t& last_seq, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(signal_mu_);
    const bool ok = signal_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return signal_seq_.load(std::memory_order_relaxed) != last_seq;
    });
    if (!ok && signal_seq_.load(std::memory_order_relaxed) == last_seq)
        return false;
    last_seq = signal_seq_.load(std::memory_order_relaxed);
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

    obi = (bids_sum - asks_sum) / depth_sum;
    mid_price = 0.5f * (ask_px + bid_px);

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
    const long long msg_ms = progressiv_->current_orderbook.message_time;
    const long long txn_ms = progressiv_->current_orderbook.transact_time;

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
    // 冷启动：从首次采样起未满窗口时长则输出 0
    auto normalize_ofi_window = [&](long long window_ms) -> float
    {
        if (ml_ofi_start_ms_ == 0 || now_ms - ml_ofi_start_ms_ < window_ms)
            return 0.f;

        const auto [sum, abs_sum] = sum_ofi_window(window_ms);
        return abs_sum > 1e-12f ? sum / abs_sum : 0.f;
    };

    ml_ofi_5s = normalize_ofi_window(5000);
    ml_ofi_15s = normalize_ofi_window(15000);

    mid_hist_time_ms_.insert(mid_hist_time_ms_.begin(), now_ms);
    mid_hist_.insert(mid_hist_.begin(), mid_price);
    if (mid_hist_start_ms_ == 0)
        mid_hist_start_ms_ = now_ms;
    while (!mid_hist_time_ms_.empty() && now_ms - mid_hist_time_ms_.back() > kVolMaxMs)
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

    prev_bid_quantity = quantity_bid;
    prev_ask_quantity = quantity_ask;
    has_prev_ = true;
    prev_asks = asks;
    prev_bids = bids;

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

    output_.obi = obi;
    output_.mid_price = mid_price;
    output_.ml_ofi_5s = ml_ofi_5s;
    output_.ml_ofi_15s = ml_ofi_15s;
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
    if (!progressiv_ || !progressiv_->interface_)
        return;

    const signal_snapshot snap = latest_signal();
    if (snap.seq == 0)
        return;
    const float predicted_movement = snap.T;
    const float alpha = snap.alpha;
    const float tau = snap.tau;
    const float horizon = snap.horizon;
    const float q_ord = snap.q_ord;
    const float tp_offset = snap.tp_offset;
    const bool enable_dynamic_risk_management = snap.enable_dynamic_risk_management;

    const auto& trade = progressiv_->interface_->trade_symbol();

    if (progressiv_->get_enable_live_action())
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
            async_log::instance().error(std::string("live query rejected: ") + ex.what());
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

        // 仓位刚平：需连续多 tick 确认，避免持仓查询短暂空窗清空状态（会重置 horizon/flip）
        if (position_.time > 0.f && !has_open_pos())
        {
            ++position_.empty_pos_streak;
            if (position_.empty_pos_streak >= 5)
            {
                try
                {
                    if (!orders.empty())
                        progressiv_->interface_->ws_cancel_all_open_orders(trade);
                }
                catch (const std::exception& ex)
                {
                    async_log::instance().error(
                        std::string("cancel-all after flatten rejected: ") + ex.what());
                }
                position_ = {};
            }
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
                        std::ostringstream oss;
                        oss << "post-only " << side << " " << req.price
                            << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                            << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                            << " rejected: " << ex.what();
                        async_log::instance().error(oss.str());
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
            // 仍在确认是否已平仓时，不要把持仓状态清掉、也不要把 TP 当开仓单追价
            if (position_.time > 0.f)
                ;
            else
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
                        async_log::instance().error(
                            std::string("cancel BUY rejected: ") + ex.what());
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
                            req.quantity = o.orig_qty.empty()
                                ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3)
                                : o.orig_qty;
                            progressiv_->interface_->ws_modify_order(o.order_id, req);
                        }
                        catch (const std::exception& ex)
                        {
                            if (binance_interface::is_missing_order_error(ex.what()))
                            {
                                progressiv_->interface_->forget_open_order(o.order_id);
                                order_request req;
                                req.symbol = trade;
                                req.side = "BUY";
                                req.type = "LIMIT";
                                req.time_in_force = "GTX";
                                req.price = px_str;
                                req.quantity = o.orig_qty.empty()
                                    ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3)
                                    : o.orig_qty;
                                try { progressiv_->interface_->ws_place_order(req); }
                                catch (const std::exception& ex2)
                                {
                                    async_log::instance().error(
                                        std::string("re-place BUY after missing modify rejected: ")
                                        + ex2.what());
                                }
                            }
                            else
                            {
                                std::ostringstream oss;
                                oss << "reprice BUY " << px_str
                                    << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                    << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                    << " rejected: " << ex.what();
                                async_log::instance().error(oss.str());
                            }
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
                        async_log::instance().error(
                            std::string("cancel SELL rejected: ") + ex.what());
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
                            req.quantity = o.orig_qty.empty()
                                ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3)
                                : o.orig_qty;
                            progressiv_->interface_->ws_modify_order(o.order_id, req);
                        }
                        catch (const std::exception& ex)
                        {
                            if (binance_interface::is_missing_order_error(ex.what()))
                            {
                                progressiv_->interface_->forget_open_order(o.order_id);
                                order_request req;
                                req.symbol = trade;
                                req.side = "SELL";
                                req.type = "LIMIT";
                                req.time_in_force = "GTX";
                                req.price = px_str;
                                req.quantity = o.orig_qty.empty()
                                    ? format_decimal(std::floor((q_ord / px) * 1000.0 + 1e-6) / 1000.0, 3)
                                    : o.orig_qty;
                                try { progressiv_->interface_->ws_place_order(req); }
                                catch (const std::exception& ex2)
                                {
                                    async_log::instance().error(
                                        std::string("re-place SELL after missing modify rejected: ")
                                        + ex2.what());
                                }
                            }
                            else
                            {
                                std::ostringstream oss;
                                oss << "reprice SELL " << px_str
                                    << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                                    << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                                    << " rejected: " << ex.what();
                                async_log::instance().error(oss.str());
                            }
                        }
                    }
                }
            }
            } // else: 无持仓状态，管理开仓挂单
        }
        else if (!positions.empty())
        {
            position_.empty_pos_streak = 0;
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
                    async_log::instance().error(
                        std::string("cancel-all on zero position rejected: ") + ex.what());
                }
                position_ = {};
            }
            else
            {
                const float dir = amt > 0.f ? 1.f : -1.f;
                const char* close_side = dir > 0.f ? "SELL" : "BUY";
                // horizon 用本地墙钟，避免盘口 message_time 停滞导致计时失效
                const float now_sec = static_cast<float>(binance_interface::now_ms()) / 1000.f;
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
                        async_log::instance().error(
                            std::string("cancel reduce-only rejected: ") + ex.what());
                    }
                };

                auto cancel_all = [&]()
                {
                    for (const auto& o : orders)
                        cancel_order(o.order_id);
                    orders.clear();
                };

                auto place_reduce_gtx = [&](const std::string& px_str) -> bool
                {
                    if (!book_ok || qty < 0.001f || px_str.empty() || !(bid < ask))
                        return false;
                    order_request req;
                    req.symbol = trade;
                    req.side = close_side;
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
                        std::ostringstream oss;
                        oss << "reduce-only GTX " << close_side << " " << req.price
                            << " bid=" << format_on_tick(bid, px_tick, px_decimals, false)
                            << " ask=" << format_on_tick(ask, px_tick, px_decimals, true)
                            << " rejected: " << ex.what();
                        async_log::instance().error(oss.str());
                        return false;
                    }
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
                    return place_reduce_gtx(format_tick_n(n, px_tick, px_decimals));
                };

                auto order_px = [](const order_info& o) -> float
                {
                    try { return std::stof(o.price); }
                    catch (...) { return 0.f; }
                };

                // flip/horizon：必须 GTX。改价用撤+重挂，避免 order.modify 丢 GTX/reduceOnly
                auto chase_gtx_bbo = [&]()
                {
                    std::string chase_str;
                    double chase_px = 0.0;
                    if (!same_side_bbo(dir < 0.f, bid, ask, px_tick, px_decimals, chase_str, chase_px))
                        return;

                    const order_info* working = nullptr;
                    for (const auto& o : orders)
                    {
                        if (o.side == close_side
                            && std::fabs(order_px(o) - chase_px) <= px_tick * 0.5)
                        {
                            working = &o;
                            continue;
                        }
                        cancel_order(o.order_id);
                    }
                    if (working != nullptr)
                        return;
                    place_reduce_gtx(chase_str);
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
                    // 撤掉旧单再挂 GTX，保证 timeInForce/reduceOnly 不被 modify 丢掉
                    cancel_order(working->order_id);
                    place_reduce_gtx(price_str);
                };

                auto snap_exit = [&](double raw, std::string& str, double& px)
                {
                    const bool is_buy = close_side[0] == 'B';
                    const long long n = tick_index(raw, px_tick, !is_buy);
                    px = static_cast<double>(n) * px_tick;
                    str = format_tick_n(n, px_tick, px_decimals);
                };

                auto ensure_tp = [&](double tp_snap, const std::string& tp_str)
                {
                    const order_info* tp_ord = nullptr;
                    for (const auto& o : orders)
                    {
                        if (o.side != close_side)
                        {
                            cancel_order(o.order_id);
                            continue;
                        }
                        // 暂不挂交易所 SL；清掉遗留 STOP_* 单
                        const bool is_stop = (o.type == "STOP_MARKET" || o.type == "STOP"
                            || o.type == "TAKE_PROFIT_MARKET" || o.type == "TAKE_PROFIT");
                        if (is_stop)
                        {
                            cancel_order(o.order_id);
                            continue;
                        }
                        if (o.type == "LIMIT" || o.type.empty())
                        {
                            if (tp_ord == nullptr)
                                tp_ord = &o;
                            else
                                cancel_order(o.order_id);
                        }
                        else
                        {
                            cancel_order(o.order_id);
                        }
                    }

                    const bool tp_maker = (close_side[0] == 'B') ? (tp_snap < ask) : (tp_snap > bid);
                    if (!tp_maker)
                    {
                        if (tp_ord)
                            cancel_order(tp_ord->order_id);
                    }
                    else if (tp_ord == nullptr)
                    {
                        place_reduce_post_only(close_side, static_cast<float>(tp_snap));
                    }
                    else if (std::fabs(order_px(*tp_ord) - tp_snap) > px_tick * 0.5)
                    {
                        cancel_order(tp_ord->order_id);
                        place_reduce_gtx(tp_str);
                    }
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
                        {
                            position_.close_flag = true;
                            std::ostringstream oss;
                            oss << "exit trigger: horizon age=" << (now_sec - position_.time)
                                << "s dir=" << dir;
                            async_log::instance().info(oss.str());
                        }
                        else if (predicted_movement * position_.direction < 0.f)
                        {
                            position_.close_flag = true;
                            std::ostringstream oss;
                            oss << "exit trigger: flip T=" << predicted_movement
                                << " dir=" << dir;
                            async_log::instance().info(oss.str());
                        }
                    }
                    if (position_.close_flag)
                        cancel_all();
                }

                if (!book_ok || qty < 0.001f)
                    ;
                else if (position_.close_flag)
                {
                    // flip/horizon：GTX 贴同向一档（平多卖一 / 平空买一）
                    chase_gtx_bbo();
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
                        chase_gtx_bbo();
                }
                else
                {
                    std::string tp_str;
                    double tp_snap = 0.0;
                    snap_exit(tp_px, tp_str, tp_snap);
                    ensure_tp(tp_snap, tp_str);
                }
            }
        }
    }
    else
    {
        std::vector<order_info> orders = progressiv_->interface_->ws_open_orders(trade);
        std::vector<position_info> positions = progressiv_->interface_->ws_get_positions(trade);
        if (!orders.empty())
        {
            try
            {
                progressiv_->interface_->ws_cancel_all_open_orders(trade);
            }
            catch (const std::exception& ex)
            {
                async_log::instance().error(
                    std::string("cancel-all on disable rejected: ") + ex.what());
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
                    async_log::instance().error(
                        std::string("market flatten rejected: ") + ex.what());
                }
            }
            try
            {
                progressiv_->interface_->ws_cancel_all_open_orders(trade);
            }
            catch (const std::exception& ex)
            {
                async_log::instance().error(
                    std::string("cancel-all after flatten rejected: ") + ex.what());
            }
            position_ = {};
        }
    }
}

void orderbook_script::destroy()
{
    script::destroy();
}
