//
// Created by zagym on 09/08/2026.
//

#include "Progressiv.h"
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include "Binance_interface.h"
#include "async_log.h"
#include "loader.h"

namespace
{
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

    bool parse_bool_value(std::string value)
    {
        trim_inplace(value);
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    void apply_app_cfg_line(std::string key, std::string value,
                            bool& enable_training_capture,
                            bool& enable_live_execution,
                            bool& use_testnet)
    {
        trim_inplace(key);
        trim_inplace(value);
        if (key == "enable_training_capture")
            enable_training_capture = parse_bool_value(value);
        else if (key == "enable_live_execution" || key == "enable_live_action")
            enable_live_execution = parse_bool_value(value);
        else if (key == "use_testnet")
            use_testnet = parse_bool_value(value);
    }

    std::string format_ms_timestamp(long long ms)
    {
        if (ms <= 0)
            return "-";

        using namespace std::chrono;
        const auto tp = system_clock::time_point{milliseconds{ms}};
        const std::time_t sec = system_clock::to_time_t(tp);
        const int millis = static_cast<int>(ms % 1000);

        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &sec);
#else
        localtime_r(&sec, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d%H%M%S") << '.'
            << std::setw(3) << std::setfill('0') << millis;
        return oss.str();
    }

    void append_jsonl(const std::string& symbol, const std::string& day, const std::string& line)
    {
        if (symbol.empty() || day.empty() || day == "-")
            return;
        const std::string dir = "training_data/" + symbol;
        std::filesystem::create_directories(dir);
        async_log::instance().write_file(dir + "/" + day + ".jsonl", line);
    }
}

Progressiv::Progressiv()
{
    interface_ = new binance_interface();
    mft_script_ = new mft_script();
}

void Progressiv::destroy()
{
    running_ = false;
    if (interface_)
        interface_->stop();
    if (data_thread_.joinable())
        data_thread_.join();
    async_log::instance().stop();
}

Progressiv::~Progressiv()
{
    destroy();
    delete mft_script_;
    mft_script_ = nullptr;
    delete interface_;
    interface_ = nullptr;
}

void Progressiv::load_app_cfg(const std::string& content)
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

        apply_app_cfg_line(line.substr(0, eq), line.substr(eq + 1),
                           enable_training_capture, enable_live_execution, use_testnet);
    }
}

void Progressiv::init()
{
    async_log::instance().start();
    load_app_cfg(loader::load_file("app.cfg"));
    if (use_testnet) interface_->init("demo_credential.cfg");
    else interface_->init("credential.cfg");
    interface_->start(20);
    if (mft_script_)
    {
        mft_script_->set_controller(this);
        mft_script_->init(interface_->signal_symbol());
    }
    async_log::instance().info("Progressiv init OK");
}

void Progressiv::data_loop()
{
    while (running_)
    {
        try
        {
            const auto wait_t0 = std::chrono::steady_clock::now();
            current_orderbook = interface_->get_ws_orderbook();
            const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_t0).count();
            if (!running_)
                break;

            const auto compute_t0 = std::chrono::steady_clock::now();
            current_tick = current_orderbook.last_update_id;
            if (mft_script_)
                mft_script_->run();
            if (enable_training_capture)
            {
                transaction_time = format_ms_timestamp(current_orderbook.transact_time);
                message_time = format_ms_timestamp(current_orderbook.message_time);
                const std::string day = (message_time.size() >= 8) ? message_time.substr(0, 8) : std::string{};
                write_factors(day, message_time);
            }
            const auto compute_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - compute_t0).count();

            const long long exch_t = current_orderbook.transact_time;
            const long long recv_t = current_orderbook.message_time;
            const long long exch_lag_ms = (exch_t > 0 && recv_t > 0) ? (recv_t - exch_t) : -1;
            async_log::instance().set_loop_metrics(
                exch_lag_ms,
                static_cast<double>(wait_us) / 1000.0,
                static_cast<double>(compute_us) / 1000.0);
        }
        catch (const std::exception& e)
        {
            if (!running_)
                break;
            async_log::instance().error(std::string("data loop: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void Progressiv::run()
{
    running_ = true;
    async_log::instance().info("entering data thread");
    data_thread_ = std::thread([this] { data_loop(); });
    if (data_thread_.joinable())
        data_thread_.join();
    running_ = false;
    async_log::instance().stop();
}

void Progressiv::write_factors(const std::string& day, const std::string& signal_timestamp)
{
    if (!mft_script_ || !interface_ || day.empty() || day == "-")
        return;

    const data_stream& o = mft_script_->factors();
    if (!(o.mid > 0.f) || o.t_unix_ms <= 0)
        return;

    const long long sec = o.t_unix_ms / 1000;
    if (sec == last_factor_write_sec_)
        return;
    last_factor_write_sec_ = sec;

    std::ostringstream line;
    line << std::setprecision(9);
    auto f = [&](const char* k, float v) { line << ",\"" << k << "\":" << v; };
    auto n = [&](const char* k, long long v) { line << ",\"" << k << "\":" << v; };

    line << "{\"timestamp\":\"" << signal_timestamp << '"'
         << ",\"transaction_time\":\"" << transaction_time << '"';
    n("t_unix_ms", o.t_unix_ms);
    n("tick", static_cast<long long>(o.tick));
    f("bid", o.bid);
    f("ask", o.ask);
    f("mid", o.mid);
    f("bid_qty", o.bid_qty);
    f("ask_qty", o.ask_qty);
    f("mark", o.mark);
    f("index", o.index);
    f("oi", o.oi);
    f("rv_15s", o.rv_15s); f("rv_30s", o.rv_30s); f("rv_1m", o.rv_1m); f("rv_2m", o.rv_2m);
    f("rv_5m", o.rv_5m); f("rv_15m", o.rv_15m); f("rv_30m", o.rv_30m);
    f("sigma_15s", o.sigma_15s); f("sigma_30s", o.sigma_30s); f("sigma_1m", o.sigma_1m);
    f("sigma_2m", o.sigma_2m); f("sigma_5m", o.sigma_5m); f("sigma_15m", o.sigma_15m);
    f("sigma_30m", o.sigma_30m);
    f("z_tp_60", o.z_tp_60); f("z_sl_60", o.z_sl_60);
    f("z_tp_180", o.z_tp_180); f("z_sl_180", o.z_sl_180);
    f("z_tp_300", o.z_tp_300); f("z_sl_300", o.z_sl_300);
    f("rs_plus_1m", o.rs_plus_1m); f("rs_minus_1m", o.rs_minus_1m); f("rs_imb_1m", o.rs_imb_1m);
    f("rs_plus_5m", o.rs_plus_5m); f("rs_minus_5m", o.rs_minus_5m); f("rs_imb_5m", o.rs_imb_5m);
    f("hl_1m", o.hl_1m); f("hl_5m", o.hl_5m); f("hl_15m", o.hl_15m);
    f("vvol_15m", o.vvol_15m); f("vvol_30m", o.vvol_30m);
    f("vol_q", o.vol_q); f("drv", o.drv);
    f("r_5s", o.r_5s); f("r_15s", o.r_15s); f("r_30s", o.r_30s); f("r_1m", o.r_1m);
    f("r_2m", o.r_2m); f("r_5m", o.r_5m); f("r_10m", o.r_10m); f("r_30m", o.r_30m);
    f("t_30s", o.t_30s); f("t_1m", o.t_1m); f("t_5m", o.t_5m); f("t_15m", o.t_15m);
    f("dt_1m_5m", o.dt_1m_5m); f("r_acc_30s", o.r_acc_30s);
    f("er_1m", o.er_1m); f("er_5m", o.er_5m); f("er_15m", o.er_15m);
    f("pos_hl_5m", o.pos_hl_5m); f("pos_hl_15m", o.pos_hl_15m); f("pos_hl_30m", o.pos_hl_30m);
    f("dist_hi_5m_bps", o.dist_hi_5m_bps); f("dist_lo_5m_bps", o.dist_lo_5m_bps);
    f("dist_hi_15m_bps", o.dist_hi_15m_bps); f("dist_lo_15m_bps", o.dist_lo_15m_bps);
    f("dist_hi_30m_bps", o.dist_hi_30m_bps); f("dist_lo_30m_bps", o.dist_lo_30m_bps);
    f("d_vwap_1m", o.d_vwap_1m); f("d_vwap_5m", o.d_vwap_5m); f("d_vwap_30m", o.d_vwap_30m);
    f("clv_1m", o.clv_1m); f("clv_5m", o.clv_5m);
    f("wick_up_1m", o.wick_up_1m); f("wick_dn_1m", o.wick_dn_1m);
    f("wick_up_5m", o.wick_up_5m); f("wick_dn_5m", o.wick_dn_5m);
    f("tfi_5s", o.tfi_5s); f("tfi_15s", o.tfi_15s); f("tfi_30s", o.tfi_30s);
    f("tfi_1m", o.tfi_1m); f("tfi_2m", o.tfi_2m); f("tfi_5m", o.tfi_5m);
    n("run_len", o.run_len);
    f("beta_cvd_1m", o.beta_cvd_1m); f("beta_cvd_5m", o.beta_cvd_5m);
    f("t_cvd_1m", o.t_cvd_1m); f("t_cvd_5m", o.t_cvd_5m);
    f("div_1m", o.div_1m); f("div_5m", o.div_5m); f("corr_px_cvd_5m", o.corr_px_cvd_5m);
    f("vol_30s", o.vol_30s); f("vol_1m", o.vol_1m); f("vol_5m", o.vol_5m);
    n("n_30s", o.n_30s); n("n_1m", o.n_1m); n("n_5m", o.n_5m);
    f("nps_30s", o.nps_30s); f("nps_1m", o.nps_1m); f("nps_5m", o.nps_5m);
    f("vol_acc", o.vol_acc);
    f("spread_frac", o.spread_frac);
    f("amihud_1m", o.amihud_1m); f("amihud_5m", o.amihud_5m);
    f("lambda_1m", o.lambda_1m); f("lambda_5m", o.lambda_5m);
    f("basis", o.basis); f("basis_z", o.basis_z); f("d_basis", o.d_basis);
    f("funding", o.funding); f("t_to_fund_s", o.t_to_fund_s);
    n("utc_minute", o.utc_minute); n("utc_hour", o.utc_hour);
    f("is_m5", o.is_m5); f("is_m15", o.is_m15);
    f("doi_1m", o.doi_1m); f("doi_5m", o.doi_5m); f("px_oi_1m", o.px_oi_1m);
    f("liq_imb_30s", o.liq_imb_30s); f("liq_imb_5m", o.liq_imb_5m); f("liq_acc", o.liq_acc);
    line << "}\n";

    append_jsonl(interface_->signal_symbol(), day, line.str());
}
