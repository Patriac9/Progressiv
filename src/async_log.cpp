#include "async_log.h"

#include <chrono>
#include <fstream>
#include <iostream>

#include "Binance_interface.h"

async_log& async_log::instance()
{
    static async_log g;
    return g;
}

async_log::~async_log()
{
    stop();
}

void async_log::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    worker_ = std::thread([this] { worker_loop(); });
}

void async_log::stop()
{
    if (!running_.exchange(false))
        return;
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void async_log::enqueue(item it)
{
    {
        std::lock_guard lock(mu_);
        // 防爆：丢最旧的非 error
        constexpr size_t kMax = 4096;
        while (q_.size() >= kMax)
        {
            if (q_.front().k == kind::error)
                break;
            q_.pop();
        }
        q_.push(std::move(it));
    }
    cv_.notify_one();
}

void async_log::info(std::string msg) { enqueue({kind::info, std::move(msg), {}}); }
void async_log::warn(std::string msg) { enqueue({kind::warn, std::move(msg), {}}); }
void async_log::error(std::string msg) { enqueue({kind::error, std::move(msg), {}}); }

void async_log::write_file(std::string path, std::string line)
{
    enqueue({kind::file, std::move(line), std::move(path)});
}

void async_log::set_loop_metrics(long long exch_lag_ms, double wait_ms, double compute_ms)
{
    std::lock_guard lock(metrics_mu_);
    exch_lag_ms_ = exch_lag_ms;
    wait_ms_ = wait_ms;
    compute_ms_ = compute_ms;
    ++metrics_seq_;
}

void async_log::worker_loop()
{
    while (true)
    {
        item it;
        bool has = false;
        {
            std::unique_lock lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !running_ || !q_.empty();
            });
            if (!q_.empty())
            {
                it = std::move(q_.front());
                q_.pop();
                has = true;
            }
            else if (!running_)
                break;
        }

        if (has)
        {
            if (it.k == kind::file)
            {
                if (!it.path.empty())
                {
                    std::ofstream out(it.path, std::ios::app);
                    if (out)
                        out << it.text;
                }
            }
            else if (it.k == kind::error)
                std::cerr << it.text;
            else
                std::cout << it.text;
            if (!it.text.empty() && it.text.back() != '\n')
            {
                if (it.k == kind::error)
                    std::cerr << '\n';
                else if (it.k != kind::file)
                    std::cout << '\n';
            }
        }

        // 指标节流打印
        long long lag = -1;
        double wait_ms = 0.0;
        double compute_ms = 0.0;
        long long seq = 0;
        {
            std::lock_guard lock(metrics_mu_);
            lag = exch_lag_ms_;
            wait_ms = wait_ms_;
            compute_ms = compute_ms_;
            seq = metrics_seq_;
        }
        const long long now = binance_interface::now_ms();
        if (seq != last_metrics_print_seq_
            && now - last_metrics_print_ms_ >= metrics_interval_ms_)
        {
            last_metrics_print_seq_ = seq;
            last_metrics_print_ms_ = now;
            std::cout << "exch_lag_ms=" << lag
                      << " wait_ms=" << wait_ms
                      << " compute_ms=" << compute_ms << '\n';
        }
    }

    // drain
    std::lock_guard lock(mu_);
    while (!q_.empty())
    {
        item it = std::move(q_.front());
        q_.pop();
        if (it.k == kind::file && !it.path.empty())
        {
            std::ofstream out(it.path, std::ios::app);
            if (out)
                out << it.text;
        }
        else if (it.k == kind::error)
            std::cerr << it.text << (it.text.empty() || it.text.back() == '\n' ? "" : "\n");
        else if (it.k != kind::file)
            std::cout << it.text << (it.text.empty() || it.text.back() == '\n' ? "" : "\n");
    }
}
