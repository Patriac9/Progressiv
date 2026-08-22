//
// Created by zagym on 22/08/2026.
//
#include "mft_script.h"
#include "Progressiv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <exception>

mft_script::mft_script() = default;

void mft_script::init(std::string instId)
{
    trade_id = std::move(instId);
    bars_.clear();
    basis_1s_.clear();
    rv5m_hist_.clear();
    last_trade_ms_ = 0;
    last_same_ms_n_ = 0;
    now_ms_ = 0;
    last_basis_sec_ = -1;
    last_rvq_ms_ = 0;
    last_basis_ = 0.f;
    bid_ = ask_ = mid_ = bid_qty_ = ask_qty_ = 0.f;
    mark_ = index_ = oi_ = 0.f;
    funding_ = 0.f;
    liq_long_30s_ = liq_short_30s_ = liq_long_5m_ = liq_short_5m_ = 0.f;
    doi_1m_raw_ = doi_5m_raw_ = 0.f;
    next_funding_time_ = 0;
    out = {};
    calculate_factors();
}

void mft_script::run()
{
    if (!progressiv_ || !progressiv_->interface_)
        return;

    last_tick = current_tick;
    current_tick = progressiv_->current_tick;
    update_book();
    update_funding_oi_liq();
    ingest_new_trades();

    const float px = last_close() > 0.f ? last_close() : mid_;
    if (now_ms_ > 0 && px > 0.f)
        ensure_bar((now_ms_ / 1000) * 1000, px);
    prune_bars();
    calculate_factors();
}

void mft_script::update_book()
{
    const orderbook_info& book = progressiv_->current_orderbook;
    asks = book.asks;
    bids = book.bids;
    now_ms_ = book.message_time > 0 ? book.message_time : book.transact_time;
    if (now_ms_ <= 0)
    {
        using namespace std::chrono;
        now_ms_ = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    if (!bids.empty() && !asks.empty())
    {
        bid_ = bids[0].price;
        ask_ = asks[0].price;
        bid_qty_ = bids[0].quantity;
        ask_qty_ = asks[0].quantity;
        if (bid_ > 0.f && ask_ > 0.f)
            mid_ = 0.5f * (bid_ + ask_);
    }
}

void mft_script::update_funding_oi_liq()
{
    auto* iface = progressiv_->interface_;
    try
    {
        const funding_info fr = iface->get_ws_funding_rate();
        mark_ = fr.mark_price;
        index_ = fr.index_price;
        funding_ = fr.funding_rate;
        next_funding_time_ = fr.next_funding_time;
    }
    catch (const std::exception&) {}

    try
    {
        oi_ = iface->get_ws_open_interest().open_interest;
        doi_1m_raw_ = iface->get_ws_open_interest_change(60000);
        doi_5m_raw_ = iface->get_ws_open_interest_change(300000);
    }
    catch (const std::exception&) {}

    try
    {
        const liquidation_flow f30 = iface->get_ws_liquidation_flow(30000);
        const liquidation_flow f5m = iface->get_ws_liquidation_flow(300000);
        liq_long_30s_ = f30.long_qty;
        liq_short_30s_ = f30.short_qty;
        liq_long_5m_ = f5m.long_qty;
        liq_short_5m_ = f5m.short_qty;
    }
    catch (const std::exception&) {}
}

void mft_script::ingest_new_trades()
{
    auto* iface = progressiv_->interface_;
    long long window = 1800000;
    if (last_trade_ms_ > 0)
    {
        window = now_ms_ - last_trade_ms_ + 1500;
        if (window < 2500)
            window = 2500;
        if (window > 60000)
            window = 60000;
    }

    std::vector<agg_trade_info> trades;
    try
    {
        trades = iface->get_ws_agg_trades(window, false);
    }
    catch (const std::exception&)
    {
        return;
    }

    const long long cutoff = last_trade_ms_;
    const int skip_same = last_same_ms_n_;
    int seen_at_cutoff = 0;
    for (const auto& tr : trades)
    {
        const long long t = tr.trade_time > 0 ? tr.trade_time : tr.message_time;
        if (t <= 0)
            continue;
        if (cutoff > 0)
        {
            if (t < cutoff)
                continue;
            if (t == cutoff)
            {
                ++seen_at_cutoff;
                if (seen_at_cutoff <= skip_same)
                    continue;
            }
        }
        add_trade(tr);
    }
}

void mft_script::add_trade(const agg_trade_info& tr)
{
    if (!(tr.price > 0.f) || !(tr.quantity > 0.f))
        return;
    const long long t = tr.trade_time > 0 ? tr.trade_time : tr.message_time;
    if (t <= 0)
        return;

    if (t < last_trade_ms_)
        return;
    if (t == last_trade_ms_)
        ++last_same_ms_n_;
    else
    {
        last_trade_ms_ = t;
        last_same_ms_n_ = 1;
    }

    ensure_bar((t / 1000) * 1000, tr.price);
    if (bars_.empty())
        return;

    SecBar& b = bars_.back();
    if (tr.price > b.h)
        b.h = tr.price;
    if (tr.price < b.l)
        b.l = tr.price;
    b.c = tr.price;
    b.v += tr.quantity;
    if (tr.buyer_is_maker)
        b.vn += tr.quantity;
    else
        b.vp += tr.quantity;
    ++b.n;
    const float prev_cvd = bars_.size() >= 2 ? bars_[bars_.size() - 2].cvd : 0.f;
    b.cvd = prev_cvd + (b.vp - b.vn);
}

void mft_script::ensure_bar(long long bucket_ms, float px)
{
    if (bucket_ms <= 0 || !(px > 0.f))
        return;
    if (bars_.empty())
    {
        bars_.push_back({bucket_ms, px, px, px, px, 0.f, 0.f, 0.f, 0.f, 0});
        return;
    }
    while (bars_.back().t_ms + 1000 < bucket_ms)
    {
        const SecBar& p = bars_.back();
        SecBar g{};
        g.t_ms = p.t_ms + 1000;
        g.o = g.h = g.l = g.c = p.c;
        g.cvd = p.cvd;
        bars_.push_back(g);
    }
    if (bars_.back().t_ms < bucket_ms)
    {
        const SecBar& p = bars_.back();
        SecBar b{};
        b.t_ms = bucket_ms;
        b.o = b.h = b.l = b.c = px;
        b.cvd = p.cvd;
        bars_.push_back(b);
    }
}

void mft_script::prune_bars()
{
    if (bars_.size() <= static_cast<size_t>(kMaxBars))
        return;
    const size_t drop = bars_.size() - static_cast<size_t>(kMaxBars);
    bars_.erase(bars_.begin(), bars_.begin() + static_cast<std::ptrdiff_t>(drop));
}

void mft_script::push_basis_sample(float b)
{
    const long long sec = now_ms_ / 1000;
    if (sec == last_basis_sec_)
        return;
    last_basis_sec_ = sec;
    basis_1s_.push_back(b);
    while (basis_1s_.size() > static_cast<size_t>(kMaxBars))
        basis_1s_.pop_front();
}

void mft_script::push_rv5m_sample(float rv)
{
    if (now_ms_ - last_rvq_ms_ < 10000)
        return;
    last_rvq_ms_ = now_ms_;
    rv5m_hist_.push_back(rv);
    while (rv5m_hist_.size() > 180)
        rv5m_hist_.pop_front();
}

bool mft_script::ready(int sec) const
{
    return bars_.size() >= static_cast<size_t>(std::max(sec, 0)) + 1u;
}

size_t mft_script::win0(int sec) const
{
    const size_t n = bars_.size();
    const size_t w = static_cast<size_t>(std::max(sec, 0));
    if (w >= n)
        return 0;
    return n - w;
}

float mft_script::last_close() const
{
    if (bars_.empty())
        return mid_;
    return bars_.back().c > 0.f ? bars_.back().c : mid_;
}

float mft_script::sgn(float x)
{
    if (x > 0.f) return 1.f;
    if (x < 0.f) return -1.f;
    return 0.f;
}

float mft_script::rv_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double s = 0.0;
    for (size_t i = i0; i < n; ++i)
    {
        const float p0 = bars_[i - 1].c;
        const float p1 = bars_[i].c;
        if (p0 > 0.f && p1 > 0.f)
        {
            const double r = std::log(static_cast<double>(p1) / static_cast<double>(p0));
            s += r * r;
        }
    }
    return static_cast<float>(s);
}

void mft_script::rs_w(int sec, float& plus, float& minus) const
{
    plus = minus = 0.f;
    if (!ready(sec))
        return;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double rp = 0.0, rm = 0.0;
    for (size_t i = i0; i < n; ++i)
    {
        const float p0 = bars_[i - 1].c;
        const float p1 = bars_[i].c;
        if (!(p0 > 0.f) || !(p1 > 0.f))
            continue;
        const double r = std::log(static_cast<double>(p1) / static_cast<double>(p0));
        const double rr = r * r;
        if (r >= 0.0)
            rp += rr;
        else
            rm += rr;
    }
    plus = static_cast<float>(rp);
    minus = static_cast<float>(rm);
}

float mft_script::hl_ln(int sec) const
{
    if (bars_.size() < 2)
        return 0.f;
    const size_t i0 = win0(sec);
    float hi = bars_[i0].h;
    float lo = bars_[i0].l;
    for (size_t i = i0 + 1; i < bars_.size(); ++i)
    {
        hi = std::max(hi, bars_[i].h);
        lo = std::min(lo, bars_[i].l);
    }
    if (!(hi > 0.f) || !(lo > 0.f))
        return 0.f;
    return static_cast<float>(std::log(static_cast<double>(hi) / static_cast<double>(lo)));
}

float mft_script::ret_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const float p0 = bars_[bars_.size() - 1 - static_cast<size_t>(sec)].c;
    const float p1 = last_close();
    if (!(p0 > 0.f) || !(p1 > 0.f))
        return 0.f;
    return static_cast<float>(std::log(static_cast<double>(p1) / static_cast<double>(p0)));
}

float mft_script::trend_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    const double t0 = static_cast<double>(bars_[i0].t_ms) / 1000.0;
    for (size_t i = i0; i < n; ++i)
    {
        if (!(bars_[i].c > 0.f))
            continue;
        const double x = static_cast<double>(bars_[i].t_ms) / 1000.0 - t0;
        const double y = std::log(static_cast<double>(bars_[i].c));
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++k;
    }
    if (k < 3)
        return 0.f;
    const double den = static_cast<double>(k) * sxx - sx * sx;
    if (std::abs(den) < 1e-18)
        return 0.f;
    const float beta = static_cast<float>((static_cast<double>(k) * sxy - sx * sy) / den);
    return beta / (std::sqrt(rv_w(sec)) + kEps);
}

float mft_script::er_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    const float p0 = bars_[i0].c;
    const float p1 = last_close();
    if (!(p0 > 0.f) || !(p1 > 0.f))
        return 0.f;
    const double num = std::abs(std::log(static_cast<double>(p1) / static_cast<double>(p0)));
    double den = 0.0;
    for (size_t i = i0 + 1; i < n; ++i)
    {
        const float a = bars_[i - 1].c;
        const float b = bars_[i].c;
        if (a > 0.f && b > 0.f)
            den += std::abs(std::log(static_cast<double>(b) / static_cast<double>(a)));
    }
    return static_cast<float>(num / (den + static_cast<double>(kEps)));
}

void mft_script::pos_w(int sec, float& pos, float& dhi_bps, float& dlo_bps) const
{
    pos = dhi_bps = dlo_bps = 0.f;
    if (bars_.empty())
        return;
    const size_t i0 = win0(sec);
    float hi = bars_[i0].h;
    float lo = bars_[i0].l;
    for (size_t i = i0 + 1; i < bars_.size(); ++i)
    {
        hi = std::max(hi, bars_[i].h);
        lo = std::min(lo, bars_[i].l);
    }
    const float c = last_close();
    pos = (c - lo) / (hi - lo + kEps);
    if (c > 0.f && hi > 0.f)
        dhi_bps = 1.0e4f * static_cast<float>(std::log(static_cast<double>(c) / static_cast<double>(hi)));
    if (c > 0.f && lo > 0.f)
        dlo_bps = 1.0e4f * static_cast<float>(std::log(static_cast<double>(c) / static_cast<double>(lo)));
}

float mft_script::vwap_d(int sec) const
{
    if (bars_.empty())
        return 0.f;
    const size_t i0 = win0(sec);
    double pv = 0.0, vv = 0.0;
    for (size_t i = i0; i < bars_.size(); ++i)
    {
        pv += static_cast<double>(bars_[i].c) * static_cast<double>(bars_[i].v);
        vv += static_cast<double>(bars_[i].v);
    }
    if (vv <= 0.0)
        return 0.f;
    const float vwap = static_cast<float>(pv / vv);
    const float c = last_close();
    if (!(c > 0.f) || !(vwap > 0.f))
        return 0.f;
    const int rv_sec = std::max(sec, 1);
    const float sig = std::sqrt(rv_w(std::min(rv_sec, static_cast<int>(bars_.size()) - 1))) + kEps;
    return static_cast<float>(std::log(static_cast<double>(c) / static_cast<double>(vwap))) / sig;
}

void mft_script::shape_w(int sec, float& clv, float& wu, float& wd) const
{
    clv = wu = wd = 0.f;
    if (bars_.empty())
        return;
    const size_t i0 = win0(sec);
    const float o = bars_[i0].o > 0.f ? bars_[i0].o : bars_[i0].c;
    const float c = last_close();
    float hi = bars_[i0].h;
    float lo = bars_[i0].l;
    for (size_t i = i0 + 1; i < bars_.size(); ++i)
    {
        hi = std::max(hi, bars_[i].h);
        lo = std::min(lo, bars_[i].l);
    }
    const float rng = hi - lo + kEps;
    clv = (c - lo) / rng - 0.5f;
    wu = (hi - std::max(o, c)) / rng;
    wd = (std::min(o, c) - lo) / rng;
}

float mft_script::tfi_w(int sec) const
{
    if (bars_.empty())
        return 0.f;
    const size_t i0 = win0(sec);
    float vp = 0.f, vn = 0.f;
    for (size_t i = i0; i < bars_.size(); ++i)
    {
        vp += bars_[i].vp;
        vn += bars_[i].vn;
    }
    return (vp - vn) / (vp + vn + kEps);
}

int mft_script::tfi_run_len() const
{
    if (bars_.empty())
        return 0;
    const float last = bars_.back().vp - bars_.back().vn;
    const float s = sgn(last);
    if (s == 0.f)
        return 0;
    int n = 0;
    for (int i = static_cast<int>(bars_.size()) - 1; i >= 0; --i)
    {
        const float d = bars_[static_cast<size_t>(i)].vp - bars_[static_cast<size_t>(i)].vn;
        if (sgn(d) != s)
            break;
        ++n;
    }
    return n;
}

void mft_script::cvd_w(int sec, float& beta, float& T) const
{
    beta = T = 0.f;
    if (!ready(sec))
        return;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    const double t0 = static_cast<double>(bars_[i0].t_ms) / 1000.0;
    for (size_t i = i0; i < n; ++i)
    {
        const double x = static_cast<double>(bars_[i].t_ms) / 1000.0 - t0;
        const double y = static_cast<double>(bars_[i].cvd);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++k;
    }
    if (k < 3)
        return;
    const double den = static_cast<double>(k) * sxx - sx * sx;
    if (std::abs(den) < 1e-18)
        return;
    beta = static_cast<float>((static_cast<double>(k) * sxy - sx * sy) / den);
    T = beta / (std::sqrt(rv_w(sec)) + kEps);
}

float mft_script::corr_px_cvd(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int k = 0;
    for (size_t i = i0; i < n; ++i)
    {
        const float p0 = bars_[i - 1].c;
        const float p1 = bars_[i].c;
        if (!(p0 > 0.f) || !(p1 > 0.f))
            continue;
        const double dx = std::log(static_cast<double>(p1) / static_cast<double>(p0));
        const double dy = static_cast<double>(bars_[i].cvd - bars_[i - 1].cvd);
        sx += dx;
        sy += dy;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
        ++k;
    }
    if (k < 10)
        return 0.f;
    const double num = static_cast<double>(k) * sxy - sx * sy;
    const double den = std::sqrt(std::max(0.0, static_cast<double>(k) * sxx - sx * sx))
                     * std::sqrt(std::max(0.0, static_cast<double>(k) * syy - sy * sy));
    if (den < 1e-18)
        return 0.f;
    return static_cast<float>(num / den);
}

void mft_script::act_w(int sec, float& vol, int& n, float& nps) const
{
    vol = 0.f;
    n = 0;
    nps = 0.f;
    if (bars_.empty() || sec <= 0)
        return;
    const size_t i0 = win0(sec);
    for (size_t i = i0; i < bars_.size(); ++i)
    {
        vol += bars_[i].v;
        n += bars_[i].n;
    }
    nps = static_cast<float>(n) / static_cast<float>(sec);
}

float mft_script::amihud_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double s = 0.0;
    int k = 0;
    for (size_t i = i0; i < n; ++i)
    {
        const float p0 = bars_[i - 1].c;
        const float p1 = bars_[i].c;
        const float notional = bars_[i].c * bars_[i].v;
        if (!(p0 > 0.f) || !(p1 > 0.f))
            continue;
        const double r = std::abs(std::log(static_cast<double>(p1) / static_cast<double>(p0)));
        s += r / (static_cast<double>(notional) + static_cast<double>(kEps));
        ++k;
    }
    if (k <= 0)
        return 0.f;
    return static_cast<float>(s / static_cast<double>(k));
}

float mft_script::lambda_w(int sec) const
{
    if (!ready(sec))
        return 0.f;
    const size_t n = bars_.size();
    const size_t i0 = n - static_cast<size_t>(sec);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    for (size_t i = i0; i < n; ++i)
    {
        const float p0 = bars_[i - 1].c;
        const float p1 = bars_[i].c;
        if (!(p0 > 0.f) || !(p1 > 0.f))
            continue;
        const double x = static_cast<double>(bars_[i].vp - bars_[i].vn);
        const double y = std::log(static_cast<double>(p1) / static_cast<double>(p0));
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++k;
    }
    if (k < 3)
        return 0.f;
    const double den = static_cast<double>(k) * sxx - sx * sx;
    if (std::abs(den) < 1e-18)
        return 0.f;
    return static_cast<float>((static_cast<double>(k) * sxy - sx * sy) / den);
}

float mft_script::vvol_w(int minutes) const
{
    if (minutes < 2 || !ready(minutes * 60))
        return 0.f;
    std::vector<float> xs;
    xs.reserve(static_cast<size_t>(minutes));
    for (int k = 0; k < minutes; ++k)
    {
        const int end_ago = k * 60;
        const int n = static_cast<int>(bars_.size());
        if (n - end_ago < 61)
            break;
        const size_t i1 = static_cast<size_t>(n - end_ago);
        const size_t i0 = i1 - 60;
        double s = 0.0;
        for (size_t i = i0; i < i1; ++i)
        {
            const float p0 = bars_[i - 1].c;
            const float p1 = bars_[i].c;
            if (p0 > 0.f && p1 > 0.f)
            {
                const double r = std::log(static_cast<double>(p1) / static_cast<double>(p0));
                s += r * r;
            }
        }
        xs.push_back(static_cast<float>(s));
    }
    if (xs.size() < 2)
        return 0.f;
    double mean = 0.0;
    for (float x : xs)
        mean += x;
    mean /= static_cast<double>(xs.size());
    double var = 0.0;
    for (float x : xs)
    {
        const double d = static_cast<double>(x) - mean;
        var += d * d;
    }
    var /= static_cast<double>(xs.size() - 1);
    return static_cast<float>(std::sqrt(var));
}

void mft_script::calculate_factors()
{
    auto sigma_of = [](float rv) { return std::sqrt(std::max(rv, 0.f)); };

    out.t_unix_ms = now_ms_;
    out.tick = current_tick;
    out.bid = bid_;
    out.ask = ask_;
    out.mid = mid_;
    out.bid_qty = bid_qty_;
    out.ask_qty = ask_qty_;
    out.mark = mark_;
    out.index = index_;
    out.oi = oi_;
    out.funding = funding_;

    out.rv_15s = rv_w(15);   out.sigma_15s = sigma_of(out.rv_15s);
    out.rv_30s = rv_w(30);   out.sigma_30s = sigma_of(out.rv_30s);
    out.rv_1m  = rv_w(60);   out.sigma_1m  = sigma_of(out.rv_1m);
    out.rv_2m  = rv_w(120);  out.sigma_2m  = sigma_of(out.rv_2m);
    out.rv_5m  = rv_w(300);  out.sigma_5m  = sigma_of(out.rv_5m);
    out.rv_15m = rv_w(900);  out.sigma_15m = sigma_of(out.rv_15m);
    out.rv_30m = rv_w(1800); out.sigma_30m = sigma_of(out.rv_30m);

    out.z_tp_60  = kG / (out.sigma_1m + kEps);
    out.z_sl_60  = kL / (out.sigma_1m + kEps);
    out.z_tp_180 = kG / (sigma_of(rv_w(180)) + kEps);
    out.z_sl_180 = kL / (sigma_of(rv_w(180)) + kEps);
    out.z_tp_300 = kG / (out.sigma_5m + kEps);
    out.z_sl_300 = kL / (out.sigma_5m + kEps);

    rs_w(60, out.rs_plus_1m, out.rs_minus_1m);
    out.rs_imb_1m = (out.rs_plus_1m - out.rs_minus_1m) / (out.rs_plus_1m + out.rs_minus_1m + kEps);
    rs_w(300, out.rs_plus_5m, out.rs_minus_5m);
    out.rs_imb_5m = (out.rs_plus_5m - out.rs_minus_5m) / (out.rs_plus_5m + out.rs_minus_5m + kEps);

    out.hl_1m = hl_ln(60);
    out.hl_5m = hl_ln(300);
    out.hl_15m = hl_ln(900);

    out.vvol_15m = vvol_w(15);
    out.vvol_30m = vvol_w(30);

    push_rv5m_sample(out.rv_5m);
    out.vol_q = 0.f;
    if (!rv5m_hist_.empty())
    {
        int below = 0;
        for (float x : rv5m_hist_)
        {
            if (x <= out.rv_5m)
                ++below;
        }
        out.vol_q = static_cast<float>(below) / static_cast<float>(rv5m_hist_.size());
    }

    out.drv = out.rv_1m / (out.rv_5m + kEps) - 1.f;

    out.r_5s = ret_w(5);
    out.r_15s = ret_w(15);
    out.r_30s = ret_w(30);
    out.r_1m = ret_w(60);
    out.r_2m = ret_w(120);
    out.r_5m = ret_w(300);
    out.r_10m = ret_w(600);
    out.r_30m = ret_w(1800);

    out.t_30s = trend_w(30);
    out.t_1m = trend_w(60);
    out.t_5m = trend_w(300);
    out.t_15m = trend_w(900);
    out.dt_1m_5m = out.t_1m - out.t_5m;

    out.r_acc_30s = 0.f;
    if (ready(60))
    {
        const size_t n = bars_.size();
        const float p_lag0 = bars_[n - 1 - 60].c;
        const float p_lag1 = bars_[n - 1 - 30].c;
        float r_lag = 0.f;
        if (p_lag0 > 0.f && p_lag1 > 0.f)
            r_lag = static_cast<float>(std::log(static_cast<double>(p_lag1) / static_cast<double>(p_lag0)));
        out.r_acc_30s = out.r_30s - r_lag;
    }

    out.er_1m = er_w(60);
    out.er_5m = er_w(300);
    out.er_15m = er_w(900);

    pos_w(300, out.pos_hl_5m, out.dist_hi_5m_bps, out.dist_lo_5m_bps);
    pos_w(900, out.pos_hl_15m, out.dist_hi_15m_bps, out.dist_lo_15m_bps);
    pos_w(1800, out.pos_hl_30m, out.dist_hi_30m_bps, out.dist_lo_30m_bps);

    out.d_vwap_1m = vwap_d(60);
    out.d_vwap_5m = vwap_d(300);
    out.d_vwap_30m = vwap_d(1800);

    shape_w(60, out.clv_1m, out.wick_up_1m, out.wick_dn_1m);
    shape_w(300, out.clv_5m, out.wick_up_5m, out.wick_dn_5m);

    out.tfi_5s = tfi_w(5);
    out.tfi_15s = tfi_w(15);
    out.tfi_30s = tfi_w(30);
    out.tfi_1m = tfi_w(60);
    out.tfi_2m = tfi_w(120);
    out.tfi_5m = tfi_w(300);
    out.run_len = tfi_run_len();

    cvd_w(60, out.beta_cvd_1m, out.t_cvd_1m);
    cvd_w(300, out.beta_cvd_5m, out.t_cvd_5m);
    out.div_1m = sgn(out.t_1m) - sgn(out.beta_cvd_1m);
    out.div_5m = sgn(out.t_5m) - sgn(out.beta_cvd_5m);
    out.corr_px_cvd_5m = corr_px_cvd(300);

    act_w(30, out.vol_30s, out.n_30s, out.nps_30s);
    act_w(60, out.vol_1m, out.n_1m, out.nps_1m);
    act_w(300, out.vol_5m, out.n_5m, out.nps_5m);
    out.vol_acc = out.vol_30s / (out.vol_5m / 10.f + kEps);

    out.spread_frac = 0.f;
    if (mid_ > 0.f && ask_ >= bid_ && bid_ > 0.f)
        out.spread_frac = (ask_ - bid_) / mid_;
    out.amihud_1m = amihud_w(60);
    out.amihud_5m = amihud_w(300);
    out.lambda_1m = lambda_w(60);
    out.lambda_5m = lambda_w(300);

    out.basis = 0.f;
    if (index_ > 0.f)
        out.basis = (mark_ - index_) / index_;
    out.d_basis = out.basis - last_basis_;
    last_basis_ = out.basis;
    push_basis_sample(out.basis);
    out.basis_z = 0.f;
    if (basis_1s_.size() >= 30)
    {
        double mean = 0.0;
        for (float x : basis_1s_)
            mean += x;
        mean /= static_cast<double>(basis_1s_.size());
        double var = 0.0;
        for (float x : basis_1s_)
        {
            const double d = static_cast<double>(x) - mean;
            var += d * d;
        }
        var /= static_cast<double>(basis_1s_.size());
        out.basis_z = static_cast<float>((static_cast<double>(out.basis) - mean) / (std::sqrt(var) + static_cast<double>(kEps)));
    }
    out.t_to_fund_s = 0.f;
    if (next_funding_time_ > 0 && now_ms_ > 0)
        out.t_to_fund_s = static_cast<float>(next_funding_time_ - now_ms_) / 1000.f;

    out.utc_minute = 0;
    out.utc_hour = 0;
    out.is_m5 = out.is_m15 = 0.f;
    if (now_ms_ > 0)
    {
        const std::time_t sec = static_cast<std::time_t>(now_ms_ / 1000);
        std::tm tm{};
#ifdef _MSC_VER
        gmtime_s(&tm, &sec);
#else
        gmtime_r(&sec, &tm);
#endif
        out.utc_minute = tm.tm_min;
        out.utc_hour = tm.tm_hour;
        out.is_m5 = (tm.tm_min % 5 == 0) ? 1.f : 0.f;
        out.is_m15 = (tm.tm_min % 15 == 0) ? 1.f : 0.f;
    }

    out.doi_1m = doi_1m_raw_;
    out.doi_5m = doi_5m_raw_;
    out.px_oi_1m = sgn(out.r_1m) * sgn(out.doi_1m);
    out.liq_imb_30s = (liq_long_30s_ - liq_short_30s_) / (liq_long_30s_ + liq_short_30s_ + kEps);
    out.liq_imb_5m = (liq_long_5m_ - liq_short_5m_) / (liq_long_5m_ + liq_short_5m_ + kEps);
    out.liq_acc = (liq_long_30s_ + liq_short_30s_) / (liq_long_5m_ + liq_short_5m_ + kEps);
}
