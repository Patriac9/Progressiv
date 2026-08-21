#ifndef PROGRESSIV_ASYNC_LOG_H
#define PROGRESSIV_ASYNC_LOG_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// 异步日志：热路径只入队，单独线程写 stderr/stdout/文件
class async_log
{
public:
    static async_log& instance();

    void start();
    void stop();

    void info(std::string msg);
    void warn(std::string msg);
    void error(std::string msg);
    void write_file(std::string path, std::string line); // append 一行

    // 指标：热路径更新，IO 线程按 interval 打印
    // wait_ms = 等下一帧盘口；compute_ms = 真正算信号
    void set_loop_metrics(long long exch_lag_ms, double wait_ms, double compute_ms);
    void set_metrics_interval_ms(long long ms) { metrics_interval_ms_ = ms; }

private:
    async_log() = default;
    ~async_log();

    enum class kind { info, warn, error, file };

    struct item
    {
        kind k = kind::info;
        std::string text;
        std::string path; // file only
    };

    void worker_loop();
    void enqueue(item it);

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<item> q_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::mutex metrics_mu_;
    long long exch_lag_ms_ = -1;
    double wait_ms_ = 0.0;
    double compute_ms_ = 0.0;
    long long metrics_seq_ = 0;
    long long last_metrics_print_seq_ = 0;
    long long metrics_interval_ms_ = 1000;
    long long last_metrics_print_ms_ = 0;
};

#endif
