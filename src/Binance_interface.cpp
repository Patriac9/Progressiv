//
// Created by zagym on 09/08/2026.
//

#include "Binance_interface.h"
#include "net_transport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "loader.h"
#include "ed25519_sign.h"

namespace
{
    int b64_value(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }

    std::vector<unsigned char> base64_decode(const std::string& input)
    {
        std::vector<unsigned char> out;
        out.reserve(input.size() * 3 / 4);
        int val = 0;
        int valb = -8;
        for (unsigned char c : input)
        {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                continue;
            const int d = b64_value(static_cast<char>(c));
            if (d < 0)
                continue;
            val = (val << 6) + d;
            valb += 6;
            if (valb >= 0)
            {
                out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    std::string base64_encode(const unsigned char* data, size_t len)
    {
        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        for (size_t i = 0; i < len; i += 3)
        {
            const unsigned int n = (static_cast<unsigned int>(data[i]) << 16)
                | ((i + 1 < len ? data[i + 1] : 0u) << 8)
                | (i + 2 < len ? data[i + 2] : 0u);
            out.push_back(kTable[(n >> 18) & 63]);
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back(i + 1 < len ? kTable[(n >> 6) & 63] : '=');
            out.push_back(i + 2 < len ? kTable[n & 63] : '=');
        }
        return out;
    }

    std::vector<unsigned char> pem_to_der(const std::string& pem)
    {
        const auto begin = pem.find("-----BEGIN");
        const auto end = pem.find("-----END");
        if (begin == std::string::npos || end == std::string::npos || end <= begin)
            throw std::runtime_error("Invalid Ed25519 PEM private key");

        size_t i = begin;
        while (i < end && pem[i] != '\n' && pem[i] != '\r')
            ++i;
        if (i >= end)
            throw std::runtime_error("Invalid Ed25519 PEM private key body");
        while (i < end && (pem[i] == '\n' || pem[i] == '\r'))
            ++i;
        return base64_decode(pem.substr(i, end - i));
    }

    std::string json_escape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
            }
        }
        return out;
    }

    std::string extract_json_string(const std::string& obj, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return {};

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return {};

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i >= obj.size() || obj[i] != '"')
            return {};

        std::string value;
        for (size_t j = i + 1; j < obj.size(); ++j)
        {
            if (obj[j] == '\\' && j + 1 < obj.size())
            {
                value.push_back(obj[j + 1]);
                ++j;
                continue;
            }
            if (obj[j] == '"')
                break;
            value.push_back(obj[j]);
        }
        return value;
    }

    // 字符串或裸数字/布尔，统一成 string（合约字段常为 DECIMAL 字符串，偶发裸数字）
    std::string extract_json_scalar(const std::string& obj, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return {};

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return {};

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i >= obj.size())
            return {};

        if (obj[i] == '"')
        {
            std::string value;
            for (size_t j = i + 1; j < obj.size(); ++j)
            {
                if (obj[j] == '\\' && j + 1 < obj.size())
                {
                    value.push_back(obj[j + 1]);
                    ++j;
                    continue;
                }
                if (obj[j] == '"')
                    break;
                value.push_back(obj[j]);
            }
            return value;
        }

        size_t end = i;
        while (end < obj.size() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']'
               && obj[end] != ' ' && obj[end] != '\n' && obj[end] != '\r' && obj[end] != '\t')
            ++end;
        return obj.substr(i, end - i);
    }

    float extract_json_float(const std::string& obj, const std::string& key)
    {
        const std::string s = extract_json_scalar(obj, key);
        if (s.empty() || s == "null")
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    }

    std::vector<std::string> extract_json_object_array(const std::string& json, const std::string& array_key)
    {
        std::vector<std::string> objs;
        const std::string pattern = "\"" + array_key + "\"";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return objs;

        const auto arr_begin = json.find('[', key_pos + pattern.size());
        if (arr_begin == std::string::npos)
            return objs;

        size_t i = arr_begin + 1;
        while (i < json.size())
        {
            while (i < json.size()
                   && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t' || json[i] == ','))
                ++i;
            if (i >= json.size() || json[i] == ']')
                break;
            if (json[i] != '{')
                break;

            int depth = 0;
            size_t j = i;
            for (; j < json.size(); ++j)
            {
                if (json[j] == '{')
                    ++depth;
                else if (json[j] == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        ++j;
                        break;
                    }
                }
            }
            objs.push_back(json.substr(i, j - i));
            i = j;
        }
        return objs;
    }

    bool extract_json_bool(const std::string& obj, const std::string& key, bool default_value = false)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return default_value;

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return default_value;

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i + 4 <= obj.size() && obj.compare(i, 4, "true") == 0)
            return true;
        if (i + 5 <= obj.size() && obj.compare(i, 5, "false") == 0)
            return false;
        return default_value;
    }

    bool is_zero_amount(const std::string& value)
    {
        if (value.empty())
            return true;
        for (char c : value)
        {
            if (c != '0' && c != '.' && c != '-')
                return false;
        }
        return true;
    }

    void trim_inplace(std::string& s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
            ++start;
        if (start > 0)
            s.erase(0, start);
    }

    long long extract_json_int(const std::string& json, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return 0;

        const auto colon = json.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return 0;

        size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
            ++i;

        const auto end = json.find_first_not_of("0123456789-", i);
        if (i >= json.size() || end == i)
            return 0;
        return std::stoll(json.substr(i, end == std::string::npos ? std::string::npos : end - i));
    }

    bool is_json_number_param(const std::string& key)
    {
        return key == "timestamp"
            || key == "orderId"
            || key == "cancelOrderId"
            || key == "limit"
            || key == "recvWindow"
            || key == "strategyId"
            || key == "strategyType"
            || key == "trailingDelta"
            || key == "pegOffsetValue";
    }

    bool is_json_bool_param(const std::string& key)
    {
        return key == "omitZeroBalances"
            || key == "reduceOnly"
            || key == "closePosition";
    }

    void fill_trade_params(std::map<std::string, std::string>& params, const order_request& req)
    {
        params["side"] = req.side;
        params["type"] = req.type;
        if (!req.time_in_force.empty())
            params["timeInForce"] = req.time_in_force;
        else if (req.type == "LIMIT" || req.type == "STOP_LOSS_LIMIT" || req.type == "TAKE_PROFIT_LIMIT")
            params["timeInForce"] = "GTC";
        if (!req.price.empty())
            params["price"] = req.price;
        if (!req.stop_price.empty())
            params["stopPrice"] = req.stop_price;
        if (!req.quantity.empty())
            params["quantity"] = req.quantity;
        if (!req.quote_order_qty.empty())
            params["quoteOrderQty"] = req.quote_order_qty;
        if (!req.client_order_id.empty())
            params["newClientOrderId"] = req.client_order_id;
        if (req.reduce_only)
            params["reduceOnly"] = "true";
        if (req.close_position)
            params["closePosition"] = "true";
    }

    std::vector<orderbook_level> parse_price_levels(const std::string& json, const std::string& key)
    {
        std::vector<orderbook_level> levels;
        // 用 "key": 精确匹配，避免单字母 key（如 b/a）误命中
        const std::string pattern = "\"" + key + "\":";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return levels;

        const auto arr_begin = json.find('[', key_pos + pattern.size());
        if (arr_begin == std::string::npos)
            return levels;

        size_t i = arr_begin + 1;
        int depth = 1;
        while (i < json.size() && depth > 0)
        {
            if (json[i] == '[')
            {
                if (depth == 1)
                {
                    const auto level_end = json.find(']', i + 1);
                    if (level_end == std::string::npos)
                        break;

                    const std::string entry = json.substr(i, level_end - i + 1);
                    orderbook_level level;
                    const auto p1 = entry.find('"');
                    const auto p2 = entry.find('"', p1 + 1);
                    const auto p3 = entry.find('"', p2 + 1);
                    const auto p4 = entry.find('"', p3 + 1);
                    if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos && p4 != std::string::npos)
                    {
                        level.price = std::stof(entry.substr(p1 + 1, p2 - p1 - 1));
                        level.quantity = std::stof(entry.substr(p3 + 1, p4 - p3 - 1));
                        levels.push_back(std::move(level));
                    }
                    i = level_end + 1;
                    continue;
                }
                ++depth;
            }
            else if (json[i] == ']')
            {
                --depth;
            }
            ++i;
        }
        return levels;
    }

    order_info parse_order_fields(const std::string& obj)
    {
        order_info order;
        order.symbol = extract_json_string(obj, "symbol");
        order.order_id = extract_json_int(obj, "orderId");
        order.order_list_id = extract_json_int(obj, "orderListId");
        if (order.order_list_id == 0 && obj.find("\"orderListId\"") == std::string::npos)
            order.order_list_id = -1;
        order.client_order_id = extract_json_string(obj, "clientOrderId");
        if (order.client_order_id.empty())
            order.client_order_id = extract_json_string(obj, "origClientOrderId");
        order.status = extract_json_string(obj, "status");
        order.side = extract_json_string(obj, "side");
        order.type = extract_json_string(obj, "type");
        order.time_in_force = extract_json_string(obj, "timeInForce");
        order.price = extract_json_string(obj, "price");
        order.stop_price = extract_json_string(obj, "stopPrice");
        if (order.stop_price.empty())
            order.stop_price = extract_json_string(obj, "sp");
        order.orig_qty = extract_json_string(obj, "origQty");
        order.executed_qty = extract_json_string(obj, "executedQty");
        order.cummulative_quote_qty = extract_json_string(obj, "cummulativeQuoteQty");
        if (order.cummulative_quote_qty.empty())
            order.cummulative_quote_qty = extract_json_scalar(obj, "cumQuote");
        order.position_side = extract_json_string(obj, "positionSide");
        order.transact_time = extract_json_int(obj, "transactTime");
        if (order.transact_time == 0)
            order.transact_time = extract_json_int(obj, "updateTime");
        order.raw_json = obj;
        return order;
    }

    std::string url_encode(const std::string& s)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                out.push_back(static_cast<char>(c));
            else
            {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 15]);
            }
        }
        return out;
    }

    std::string extract_json_object(const std::string& json, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\":";
        size_t pos = 0;
        while ((pos = json.find(pattern, pos)) != std::string::npos)
        {
            size_t i = pos + pattern.size();
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
                ++i;
            if (i < json.size() && json[i] == '{')
            {
                int depth = 0;
                const size_t begin = i;
                for (; i < json.size(); ++i)
                {
                    if (json[i] == '{')
                        ++depth;
                    else if (json[i] == '}')
                    {
                        --depth;
                        if (depth == 0)
                            return json.substr(begin, i - begin + 1);
                    }
                }
                return {};
            }
            pos += pattern.size();
        }
        return {};
    }

    int tick_string_decimals(const std::string& s)
    {
        const auto dot = s.find('.');
        if (dot == std::string::npos)
            return 0;
        size_t end = s.size();
        while (end > dot + 1 && s[end - 1] == '0')
            --end;
        return static_cast<int>(end - dot - 1);
    }

    binance_interface::tick_filter parse_tick_filter(const std::string& json)
    {
        const auto filter_pos = json.find("\"PRICE_FILTER\"");
        if (filter_pos == std::string::npos)
            throw std::runtime_error("PRICE_FILTER not found in exchangeInfo");

        size_t obj_begin = filter_pos;
        while (obj_begin > 0 && json[obj_begin] != '{')
            --obj_begin;

        const auto obj_end = json.find('}', filter_pos);
        if (obj_end == std::string::npos || obj_begin >= obj_end)
            throw std::runtime_error("invalid PRICE_FILTER object in exchangeInfo");

        const std::string filter_obj = json.substr(obj_begin, obj_end - obj_begin + 1);
        const std::string tick_size = extract_json_string(filter_obj, "tickSize");
        if (tick_size.empty())
            throw std::runtime_error("tickSize not found in PRICE_FILTER");
        try
        {
            binance_interface::tick_filter f;
            f.tick_size = std::stod(tick_size);
            f.decimals = tick_string_decimals(tick_size);
            return f;
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("invalid tickSize value: " + tick_size);
        }
    }

}

struct binance_interface::RpcChannel
{
    std::string host;
    std::string path = "/ws-api/v3";
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;

    struct Job
    {
        std::string id;
        std::string request;
        std::promise<std::string> promise;
    };
    std::queue<Job> jobs;

    void start(std::string h, std::string p = "/ws-api/v3")
    {
        host = std::move(h);
        path = std::move(p);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        cv.notify_all();
        if (worker.joinable())
            worker.join();

        std::lock_guard lock(mutex);
        while (!jobs.empty())
        {
            jobs.front().promise.set_exception(
                std::make_exception_ptr(std::runtime_error("RpcChannel stopped")));
            jobs.pop();
        }
    }

    std::string call(std::string id, std::string request)
    {
        if (!running)
            throw std::runtime_error("RpcChannel not started");

        std::promise<std::string> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lock(mutex);
            jobs.push(Job{std::move(id), std::move(request), std::move(promise)});
        }
        cv.notify_one();

        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            throw std::runtime_error("WebSocket RPC timeout");
        return future.get();
    }

    void run()
    {
        while (running)
        {
            try
            {
                WsConnection ws = ws_connect(host, path);
                while (running)
                {
                    Job job;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait_for(lock, std::chrono::seconds(1), [this] {
                            return !running || !jobs.empty();
                        });
                        if (!running)
                            break;
                        if (jobs.empty())
                            continue;
                        job = std::move(jobs.front());
                        jobs.pop();
                    }

                    try
                    {
                        ws_send(ws, job.request);
                        std::string response;
                        for (;;)
                        {
                            response = ws_recv(ws);
                            if (response.empty())
                                throw std::runtime_error("Empty RPC response");
                            const bool has_id = response.find("\"id\"") != std::string::npos;
                            const bool id_match = response.find(job.id) != std::string::npos;
                            const bool id_null = response.find("\"id\":null") != std::string::npos;
                            if (!has_id || id_match || id_null)
                                break;
                        }
                        job.promise.set_value(std::move(response));
                    }
                    catch (...)
                    {
                        try
                        {
                            job.promise.set_exception(std::current_exception());
                        }
                        catch (...)
                        {
                        }
                        throw; // 触发外层重连
                    }
                }
                ws.close();
            }
            catch (...)
            {
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
};

struct binance_interface::MarketChannel
{
    std::string host;
    std::string symbol;
    uint32_t levels = 20;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    orderbook_info latest;
    bool has_data = false;
    uint64_t generation_ = 0;
    uint64_t last_consumed_gen_ = 0;

    void start(std::string h, std::string sym, uint32_t depth_levels)
    {
        host = std::move(h);
        symbol = std::move(sym);
        levels = (depth_levels <= 5) ? 5 : (depth_levels <= 10) ? 10 : 20;
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    orderbook_info get_latest(std::chrono::milliseconds wait = std::chrono::seconds(30))
    {
        std::unique_lock lock(mutex);
        const bool ok = cv.wait_for(lock, wait, [this] {
            return !running || (has_data && generation_ > last_consumed_gen_);
        });
        if (!running)
            throw std::runtime_error("MarketChannel stopped");
        if (!ok)
            throw std::runtime_error("MarketChannel: wait orderbook timeout");
        if (!has_data)
            throw std::runtime_error("MarketChannel stopped without data");
        last_consumed_gen_ = generation_;
        return latest;
    }

    orderbook_info get_cached()
    {
        std::lock_guard lock(mutex);
        if (!has_data)
            throw std::runtime_error("MarketChannel: no orderbook yet");
        return latest;
    }

    void publish(orderbook_info book)
    {
        book.message_time = binance_interface::now_ms();
        if (book.transact_time == 0)
            book.transact_time = book.message_time;
        {
            std::lock_guard lock(mutex);
            latest = std::move(book);
            has_data = true;
            ++generation_;
        }
        cv.notify_all();
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string path = "/ws/" + stream_symbol + "@depth" + std::to_string(levels) + "@0ms";

        while (running)
        {
            try
            {
                WsConnection ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws);
                    if (msg.empty())
                        break;

                    publish(binance_interface::parse_orderbook(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "MarketChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "MarketChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

struct binance_interface::UserDataChannel
{
    binance_interface* owner = nullptr;
    std::string stream_host;
    std::string rest_host;
    std::string api_key;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex key_mutex;
    std::string listen_key;

    void start(binance_interface* o, std::string stream_h, std::string rest_h, std::string key)
    {
        owner = o;
        stream_host = std::move(stream_h);
        rest_host = std::move(rest_h);
        api_key = std::move(key);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
        close_listen_key();
    }

    std::string create_listen_key()
    {
        const auto result = https_api_key(rest_host, "POST", "/fapi/v1/listenKey", api_key);
        if (result.status != 200)
            throw std::runtime_error("listenKey create failed: " + result.body);
        const std::string key = extract_json_string(result.body, "listenKey");
        if (key.empty())
            throw std::runtime_error("listenKey missing in response: " + result.body);
        return key;
    }

    void keepalive_listen_key(const std::string& key)
    {
        const auto result = https_api_key(rest_host, "PUT", "/fapi/v1/listenKey", api_key);
        if (result.status != 200)
            throw std::runtime_error("listenKey keepalive failed: " + result.body);
        (void)key;
    }

    void close_listen_key()
    {
        try
        {
            https_api_key(rest_host, "DELETE", "/fapi/v1/listenKey", api_key);
        }
        catch (...)
        {
        }
        std::lock_guard lock(key_mutex);
        listen_key.clear();
    }

    void handle_message(const std::string& msg)
    {
        if (!owner)
            return;
        const std::string event = extract_json_string(msg, "e");
        if (event == "ORDER_TRADE_UPDATE")
        {
            const std::string order_obj = extract_json_object(msg, "o");
            if (order_obj.empty())
                return;
            owner->upsert_open_order_cache(binance_interface::parse_user_stream_order(order_obj));
            owner->invalidate_positions_cache();
        }
        else if (event == "ACCOUNT_UPDATE")
        {
            owner->apply_account_update(msg);
        }
        else if (event == "listenKeyExpired")
        {
            std::cerr << "UserDataChannel listenKeyExpired; will recreate\n";
            std::lock_guard lock(key_mutex);
            listen_key.clear();
        }
    }

    void run_session(const std::string& path, const std::string& key, long long& last_keepalive_ms)
    {
        WsConnection ws = ws_connect(stream_host, path);
        last_keepalive_ms = binance_interface::now_ms();
        std::cerr << "UserDataChannel connected: " << path << '\n';

        while (running)
        {
            const long long now = binance_interface::now_ms();
            if (now - last_keepalive_ms >= 30 * 60 * 1000)
            {
                keepalive_listen_key(key);
                last_keepalive_ms = now;
            }

            {
                std::lock_guard lock(key_mutex);
                if (listen_key.empty())
                    break;
            }

            bool timed_out = false;
            const std::string msg = ws_recv(ws, &timed_out);
            if (timed_out)
                continue;
            if (msg.empty())
                break;
            handle_message(msg);
        }
        ws.close();
    }

    void run()
    {
        long long last_keepalive_ms = 0;
        while (running)
        {
            try
            {
                std::string key;
                {
                    std::lock_guard lock(key_mutex);
                    if (listen_key.empty())
                        listen_key = create_listen_key();
                    key = listen_key;
                }

                // 当前环境 WinHTTP 对 /private/ws 升级常失败；优先用稳定的 /ws/{listenKey}
                try
                {
                    run_session("/ws/" + key, key, last_keepalive_ms);
                }
                catch (const std::exception& e_legacy)
                {
                    std::cerr << "UserDataChannel legacy /ws failed: " << e_legacy.what()
                              << "; trying /private/ws\n";
                    const std::string private_path =
                        "/private/ws?listenKey=" + key
                        + "&events=ORDER_TRADE_UPDATE,ACCOUNT_UPDATE,listenKeyExpired";
                    run_session(private_path, key, last_keepalive_ms);
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "UserDataChannel error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            catch (...)
            {
                std::cerr << "UserDataChannel unknown error\n";
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }
};

struct binance_interface::FundingChannel
{
    std::string host;
    std::string rest_host;
    std::string symbol;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    funding_info latest;
    bool has_data = false;

    void start(std::string h, std::string rest_h, std::string sym)
    {
        host = std::move(h);
        rest_host = std::move(rest_h);
        symbol = std::move(sym);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
    }

    funding_info get_latest()
    {
        std::lock_guard lock(mutex);
        if (!has_data)
            throw std::runtime_error("FundingChannel: no funding data yet");
        return latest;
    }

    void publish(funding_info info)
    {
        info.message_time = binance_interface::now_ms();
        {
            std::lock_guard lock(mutex);
            latest = std::move(info);
            has_data = true;
        }
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // markPrice 属于 /market 路由；含资金费率字段 r
        const std::string path = "/market/ws/" + stream_symbol + "@markPrice@1s";

        try
        {
            const std::string rest_path = "/fapi/v1/premiumIndex?symbol=" + symbol;
            const std::string json = https_get(rest_host, rest_path);
            publish(binance_interface::parse_funding(json, symbol));
        }
        catch (const std::exception& e)
        {
            std::cerr << "FundingChannel REST premiumIndex bootstrap failed: " << e.what() << std::endl;
        }

        while (running)
        {
            try
            {
                WsConnection ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws);
                    if (msg.empty())
                        break;
                    publish(binance_interface::parse_funding(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "FundingChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "FundingChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

struct binance_interface::AggTradeChannel
{
    static constexpr long long kMaxKeepMs = 60000;

    std::string host;
    std::string symbol;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::deque<agg_trade_info> trades;

    void start(std::string h, std::string sym)
    {
        host = std::move(h);
        symbol = std::move(sym);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
    }

    aggressor_flow get_flow(long long window_ms)
    {
        const long long now = binance_interface::now_ms();
        std::lock_guard lock(mutex);
        while (!trades.empty())
        {
            const long long t = trades.front().trade_time != 0
                ? trades.front().trade_time
                : trades.front().message_time;
            if (now - t <= kMaxKeepMs)
                break;
            trades.pop_front();
        }

        aggressor_flow flow;
        for (auto it = trades.rbegin(); it != trades.rend(); ++it)
        {
            const long long t = it->trade_time != 0 ? it->trade_time : it->message_time;
            if (now - t > window_ms)
                break;
            if (it->buyer_is_maker)
                flow.sell_qty += it->quantity; // 买方是 maker → 主动卖
            else
                flow.buy_qty += it->quantity;  // 买方是 taker → 主动买
        }
        flow.net_qty = flow.buy_qty - flow.sell_qty;
        const float sum = flow.buy_qty + flow.sell_qty;
        flow.imbalance = sum > 1e-12f ? flow.net_qty / sum : 0.f;
        return flow;
    }

    agg_trade_info peek_last()
    {
        std::lock_guard lock(mutex);
        if (trades.empty())
            return {};
        return trades.back();
    }

    void push(agg_trade_info trade)
    {
        trade.message_time = binance_interface::now_ms();
        if (trade.trade_time == 0)
            trade.trade_time = trade.message_time;
        const long long now = trade.message_time;
        std::lock_guard lock(mutex);
        trades.push_back(std::move(trade));
        while (!trades.empty())
        {
            const long long t = trades.front().trade_time != 0
                ? trades.front().trade_time
                : trades.front().message_time;
            if (now - t <= kMaxKeepMs)
                break;
            trades.pop_front();
        }
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // aggTrade 属于 /market 路由
        const std::string path = "/market/ws/" + stream_symbol + "@aggTrade";

        while (running)
        {
            try
            {
                WsConnection ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws);
                    if (msg.empty())
                        break;
                    push(binance_interface::parse_agg_trade(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "AggTradeChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "AggTradeChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

binance_interface::binance_interface() = default;

binance_interface::~binance_interface()
{
    stop();
}

void binance_interface::init(std::string credential_path)
{
    std::string content = loader::load_file(std::move(credential_path));
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();

    const auto first_end = content.find('\n');
    std::string api = (first_end == std::string::npos)
        ? content
        : content.substr(0, first_end);
    if (!api.empty() && api.back() == '\r')
        api.pop_back();

    std::string remainder = (first_end == std::string::npos)
        ? std::string{}
        : content.substr(first_end + 1);

    std::string testnet_line;
    const auto last_end = remainder.rfind('\n');
    if (last_end == std::string::npos)
    {
        testnet_line = remainder;
    }
    else
    {
        testnet_line = remainder.substr(last_end + 1);
    }
    if (!testnet_line.empty() && testnet_line.back() == '\r')
        testnet_line.pop_back();

    const bool last_is_flag = (testnet_line == "0" || testnet_line == "1"
        || testnet_line == "true" || testnet_line == "TRUE"
        || testnet_line == "false" || testnet_line == "FALSE");
    if (last_is_flag)
    {
        if (last_end == std::string::npos)
            remainder.clear();
        else
        {
            remainder.resize(last_end);
            if (!remainder.empty() && remainder.back() == '\r')
                remainder.pop_back();
        }
    }

    use_testnet_ = (testnet_line == "1" || testnet_line == "true" || testnet_line == "TRUE");
    // 账户/下单仍走现货 WS API；行情与合约账户查询走 USD-M
    ws_api_host_ = use_testnet_ ? "ws-api.testnet.binance.vision" : "ws-api.binance.com";
    ws_stream_host_ = use_testnet_ ? "stream.binancefuture.com" : "fstream.binance.com";
    ws_fapi_host_ = use_testnet_ ? "testnet.binancefuture.com" : "ws-fapi.binance.com";

    set_credentials(api, remainder);
}

void binance_interface::set_credentials(const std::string& api_key, const std::string& ed25519_private_key_pem)
{
    api_key_ = api_key;
    private_key_pem_ = ed25519_private_key_pem;
}

void binance_interface::start(uint32_t depth_levels)
{
    if (started_)
        return;

    const auto symbols = parse_inst_id(loader::load_file("instId.cfg"));
    signal_symbol_ = symbols.first;
    trade_symbol_ = symbols.second;
    split_trade_market_ = (signal_symbol_ != trade_symbol_);

    balance_channel_ = std::make_unique<RpcChannel>();
    order_channel_ = std::make_unique<RpcChannel>();
    futures_channel_ = std::make_unique<RpcChannel>();
    market_channel_ = std::make_unique<MarketChannel>();
    funding_channel_ = std::make_unique<FundingChannel>();
    agg_trade_channel_ = std::make_unique<AggTradeChannel>();

    balance_channel_->start(ws_api_host_);
    order_channel_->start(ws_api_host_);
    futures_channel_->start(ws_fapi_host_, "/ws-fapi/v1");
    const std::string rest_host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    market_channel_->start(ws_stream_host_, signal_symbol_, depth_levels);
    funding_channel_->start(ws_stream_host_, rest_host, signal_symbol_);
    agg_trade_channel_->start(ws_stream_host_, signal_symbol_);

    if (split_trade_market_)
    {
        trade_market_channel_ = std::make_unique<MarketChannel>();
        trade_agg_trade_channel_ = std::make_unique<AggTradeChannel>();
        trade_market_channel_->start(ws_stream_host_, trade_symbol_, depth_levels);
        trade_agg_trade_channel_->start(ws_stream_host_, trade_symbol_);
    }

    user_data_channel_ = std::make_unique<UserDataChannel>();
    user_data_channel_->start(this, ws_stream_host_, rest_host, api_key_);
    try
    {
        refresh_open_order_cache_rest(trade_symbol_);
        std::cerr << "openOrders bootstrap OK, cached "
                  << snapshot_open_order_cache(trade_symbol_).size() << " orders\n";
    }
    catch (const std::exception& ex)
    {
        std::cerr << "openOrders bootstrap skipped: " << ex.what() << '\n';
    }

    started_ = true;
}

void binance_interface::stop()
{
    if (!started_)
        return;
    started_ = false;

    if (market_channel_)
        market_channel_->stop();
    if (trade_market_channel_)
        trade_market_channel_->stop();
    if (funding_channel_)
        funding_channel_->stop();
    if (agg_trade_channel_)
        agg_trade_channel_->stop();
    if (trade_agg_trade_channel_)
        trade_agg_trade_channel_->stop();
    if (balance_channel_)
        balance_channel_->stop();
    if (order_channel_)
        order_channel_->stop();
    if (futures_channel_)
        futures_channel_->stop();
    if (user_data_channel_)
        user_data_channel_->stop();

    market_channel_.reset();
    trade_market_channel_.reset();
    funding_channel_.reset();
    agg_trade_channel_.reset();
    trade_agg_trade_channel_.reset();
    balance_channel_.reset();
    order_channel_.reset();
    futures_channel_.reset();
    user_data_channel_.reset();
}

std::pair<std::string, std::string> binance_interface::parse_inst_id(const std::string& content)
{
    std::vector<std::string> symbols;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
        trim_inplace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        std::transform(line.begin(), line.end(), line.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        symbols.push_back(std::move(line));
        if (symbols.size() == 2)
            break;
    }
    if (symbols.empty())
        throw std::runtime_error("instId.cfg has no symbol");
    if (symbols.size() == 1)
        return {symbols[0], symbols[0]};
    return {symbols[0], symbols[1]};
}

std::string binance_interface::resolve_symbol(std::string symbol) const
{
    if (symbol.empty())
    {
        if (!trade_symbol_.empty())
            symbol = trade_symbol_;
        else
            symbol = parse_inst_id(loader::load_file("instId.cfg")).second;
    }
    trim_inplace(symbol);
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (symbol.empty())
        throw std::runtime_error("symbol is empty");
    return symbol;
}

void binance_interface::ensure_ws_ok(const std::string& json, const char* what)
{
    const auto status = extract_json_int(json, "status");
    if (status != 0 && status != 200)
        throw std::runtime_error(std::string(what) + " failed: " + json);
}

std::string binance_interface::extract_result_object(const std::string& json)
{
    const auto result_pos = json.find("\"result\"");
    if (result_pos == std::string::npos)
        return json;
    const auto brace = json.find('{', result_pos);
    if (brace == std::string::npos)
        return json;
    return json.substr(brace);
}

orderbook_info binance_interface::get_ws_orderbook(uint32_t /*data_amount*/)
{
    if (!market_channel_)
        throw std::runtime_error("market channel not started; call start() first");
    return market_channel_->get_latest();
}

orderbook_info binance_interface::get_ws_trade_orderbook()
{
    if (trade_market_channel_)
        return trade_market_channel_->get_cached();
    if (!market_channel_)
        throw std::runtime_error("market channel not started; call start() first");
    return market_channel_->get_cached();
}

agg_trade_info binance_interface::get_ws_last_agg_trade(bool trade_market)
{
    AggTradeChannel* channel = (trade_market && trade_agg_trade_channel_)
        ? trade_agg_trade_channel_.get()
        : agg_trade_channel_.get();
    if (!channel)
        throw std::runtime_error("aggTrade channel not started; call start() first");
    return channel->peek_last();
}

funding_info binance_interface::get_ws_funding_rate()
{
    if (!funding_channel_)
        throw std::runtime_error("funding channel not started; call start() first");
    return funding_channel_->get_latest();
}

aggressor_flow binance_interface::get_ws_aggressor_flow(long long window_ms, bool trade_market)
{
    AggTradeChannel* channel = (trade_market && trade_agg_trade_channel_)
        ? trade_agg_trade_channel_.get()
        : agg_trade_channel_.get();
    if (!channel)
        throw std::runtime_error("aggTrade channel not started; call start() first");
    return channel->get_flow(window_ms);
}

std::vector<asset_balance> binance_interface::get_ws_balance(bool omit_zero_balances)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!balance_channel_)
        throw std::runtime_error("balance channel not started; call start() first");

    param_map params;
    if (omit_zero_balances)
        params["omitZeroBalances"] = "true";

    const std::string response = signed_ws_call(*balance_channel_, "account.status", std::move(params));
    ensure_ws_ok(response, "account.status");
    return parse_balances(response, omit_zero_balances);
}

binance_interface::tick_filter binance_interface::get_tick_filter(std::string symbol)
{
    symbol = resolve_symbol(std::move(symbol));
    const std::string host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    const std::string path = "/fapi/v1/exchangeInfo?symbol=" + symbol;
    return parse_tick_filter(https_get(host, path));
}

float binance_interface::get_tick_size(std::string symbol)
{
    return static_cast<float>(get_tick_filter(std::move(symbol)).tick_size);
}

order_info binance_interface::ws_place_order(const order_request& req)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (req.side.empty() || req.type.empty())
        throw std::runtime_error("order side/type are required");

    param_map params;
    params["symbol"] = resolve_symbol(req.symbol);
    fill_trade_params(params, req);

    const std::string response = signed_ws_call(*futures_channel_, "order.place", std::move(params));
    ensure_ws_ok(response, "order.place");
    order_info order = parse_order(response);
    upsert_open_order_cache(order);
    return order;
}

order_info binance_interface::ws_modify_order(long long order_id, const order_request& req)
{
    param_map ids;
    ids["orderId"] = std::to_string(order_id);
    return modify_order_ws(std::move(ids), req);
}

order_info binance_interface::ws_modify_order(const std::string& client_order_id, const order_request& req)
{
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");
    param_map ids;
    ids["origClientOrderId"] = client_order_id;
    return modify_order_ws(std::move(ids), req);
}

order_info binance_interface::modify_order_ws(param_map cancel_id_params, order_request req)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (req.side.empty())
        throw std::runtime_error("order side is required");

    long long order_id = 0;
    if (const auto it = cancel_id_params.find("orderId"); it != cancel_id_params.end())
    {
        try { order_id = std::stoll(it->second); } catch (...) {}
    }

    param_map params = std::move(cancel_id_params);
    params["symbol"] = resolve_symbol(std::move(req.symbol));
    params["side"] = req.side;
    if (!req.quantity.empty())
        params["quantity"] = req.quantity;
    if (!req.price.empty())
        params["price"] = req.price;

    try
    {
        const std::string response = signed_ws_call(*futures_channel_, "order.modify", std::move(params));
        ensure_ws_ok(response, "order.modify");
        order_info order = parse_order(response);
        upsert_open_order_cache(order);
        return order;
    }
    catch (const std::exception& ex)
    {
        if (is_missing_order_error(ex.what()))
        {
            if (order_id != 0)
                erase_open_order_cache(order_id);
        }
        throw;
    }
}

order_info binance_interface::ws_get_order(long long order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["orderId"] = std::to_string(order_id);

    const std::string response = signed_ws_call(*futures_channel_, "order.status", std::move(params));
    ensure_ws_ok(response, "order.status");
    return parse_order(response);
}

order_info binance_interface::ws_get_order(const std::string& client_order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["origClientOrderId"] = client_order_id;

    const std::string response = signed_ws_call(*futures_channel_, "order.status", std::move(params));
    ensure_ws_ok(response, "order.status");
    return parse_order(response);
}

std::vector<order_info> binance_interface::ws_open_orders(std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");

    symbol = symbol.empty() ? std::string{} : resolve_symbol(std::move(symbol));
    // 热路径只读 User Data Stream / 本地缓存，不再 REST 轮询
    return snapshot_open_order_cache(symbol);
}

std::vector<position_info> binance_interface::ws_get_positions(std::string symbol, bool only_open)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    symbol = symbol.empty() ? std::string{} : resolve_symbol(std::move(symbol));
    {
        std::lock_guard lock(positions_mutex_);
        if (positions_loaded_
            && !positions_dirty_
            && positions_cache_symbol_ == symbol
            && positions_cache_only_open_ == only_open
            && positions_cache_ms_ > 0
            && now_ms() - positions_cache_ms_ < kPositionsRefreshMs)
        {
            return positions_cache_;
        }
    }

    param_map params;
    if (!symbol.empty())
        params["symbol"] = symbol;

    const std::string response = signed_ws_call(*futures_channel_, "account.position", std::move(params));
    ensure_ws_ok(response, "account.position");
    auto positions = parse_positions(response, only_open);
    {
        std::lock_guard lock(positions_mutex_);
        positions_cache_ = positions;
        positions_cache_symbol_ = symbol;
        positions_cache_only_open_ = only_open;
        positions_cache_ms_ = now_ms();
        positions_loaded_ = true;
        positions_dirty_ = false;
    }
    return positions;
}

std::vector<position_info> binance_interface::ws_peek_positions(std::string symbol, bool only_open)
{
    symbol = symbol.empty() ? std::string{} : resolve_symbol(std::move(symbol));
    {
        std::lock_guard lock(positions_mutex_);
        if (positions_loaded_
            && !positions_dirty_
            && positions_cache_symbol_ == symbol
            && positions_cache_only_open_ == only_open)
        {
            return positions_cache_;
        }
        // 脏且缓存里已有仓：先返回旧仓（禁止误判空仓再开同向单）；数量稍后刷新
        if (positions_loaded_
            && positions_dirty_
            && positions_cache_symbol_ == symbol
            && positions_cache_only_open_ == only_open)
        {
            for (const auto& p : positions_cache_)
            {
                if (std::fabs(p.position_amt) > 1e-12f)
                    return positions_cache_;
            }
        }
    }
    // 首次无缓存，或脏且缓存显示空仓：必须同步拉，避免成交后仍以为空仓而连开
    return ws_get_positions(std::move(symbol), only_open);
}

void binance_interface::refresh_positions_if_dirty(std::string symbol, bool only_open)
{
    bool dirty = false;
    {
        std::lock_guard lock(positions_mutex_);
        dirty = positions_dirty_ || !positions_loaded_;
    }
    if (!dirty)
        return;
    try
    {
        ws_get_positions(std::move(symbol), only_open);
    }
    catch (...)
    {
        // 刷新失败保留旧缓存，下一 tick 再试
    }
}

void binance_interface::invalidate_positions_cache()
{
    std::lock_guard lock(positions_mutex_);
    positions_dirty_ = true;
    positions_cache_ms_ = 0;
}

void binance_interface::apply_account_update(const std::string& msg)
{
    const std::string account = extract_json_object(msg, "a");
    if (account.empty())
    {
        invalidate_positions_cache();
        return;
    }

    const auto pos_objs = extract_json_object_array(account, "P");
    if (pos_objs.empty())
    {
        // 无持仓字段时仍标脏，下一 tick 拉 account.position
        invalidate_positions_cache();
        return;
    }

    std::lock_guard lock(positions_mutex_);
    if (!positions_loaded_)
    {
        positions_dirty_ = true;
        positions_cache_ms_ = 0;
        return;
    }

    for (const auto& obj : pos_objs)
    {
        const std::string sym = extract_json_string(obj, "s");
        if (sym.empty())
            continue;
        if (!positions_cache_symbol_.empty() && sym != positions_cache_symbol_)
            continue;

        const float pa = extract_json_float(obj, "pa");
        const float ep = extract_json_float(obj, "ep");
        const float up = extract_json_float(obj, "up");
        const std::string ps = extract_json_string(obj, "ps");

        const auto it = std::find_if(
            positions_cache_.begin(),
            positions_cache_.end(),
            [&](const position_info& p) { return p.symbol == sym; });

        if (std::fabs(pa) < 1e-12f)
        {
            if (it != positions_cache_.end())
                positions_cache_.erase(it);
            continue;
        }

        if (it != positions_cache_.end())
        {
            it->position_amt = pa;
            if (ep != 0.f)
                it->entry_price = ep;
            it->unrealized_profit = up;
            if (!ps.empty())
                it->position_side = ps;
        }
        else if (!positions_cache_only_open_ || std::fabs(pa) > 1e-12f)
        {
            position_info pos;
            pos.symbol = sym;
            pos.position_amt = pa;
            pos.entry_price = ep;
            pos.unrealized_profit = up;
            pos.position_side = ps.empty() ? "BOTH" : ps;
            positions_cache_.push_back(std::move(pos));
        }
    }

    positions_dirty_ = false;
    positions_cache_ms_ = now_ms();
}

order_info binance_interface::ws_cancel_order(long long order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["orderId"] = std::to_string(order_id);

    try
    {
        const std::string response = signed_ws_call(*futures_channel_, "order.cancel", std::move(params));
        ensure_ws_ok(response, "order.cancel");
        order_info order = parse_order(response);
        erase_open_order_cache(order.order_id != 0 ? order.order_id : order_id);
        return order;
    }
    catch (const std::exception& ex)
    {
        // 已成交/已撤：清缓存，视为成功，避免策略反复对僵尸 id 撤单
        if (is_missing_order_error(ex.what()))
        {
            erase_open_order_cache(order_id);
            order_info gone;
            gone.order_id = order_id;
            gone.status = "CANCELED";
            return gone;
        }
        throw;
    }
}

order_info binance_interface::ws_cancel_order(const std::string& client_order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["origClientOrderId"] = client_order_id;

    try
    {
        const std::string response = signed_ws_call(*futures_channel_, "order.cancel", std::move(params));
        ensure_ws_ok(response, "order.cancel");
        order_info order = parse_order(response);
        erase_open_order_cache(order.order_id);
        return order;
    }
    catch (const std::exception& ex)
    {
        if (is_missing_order_error(ex.what()))
        {
            order_info gone;
            gone.client_order_id = client_order_id;
            gone.status = "CANCELED";
            return gone;
        }
        throw;
    }
}

std::vector<order_info> binance_interface::ws_cancel_all_open_orders(std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");

    const std::string sym = resolve_symbol(std::move(symbol));
    param_map params;
    params["symbol"] = sym;

    const std::string response = signed_fapi_rest("DELETE", "/fapi/v1/allOpenOrders", std::move(params));
    std::vector<order_info> cancelled;
    if (!response.empty() && response.front() == '[')
        cancelled = parse_open_orders(response);

    {
        std::lock_guard lock(open_orders_mutex_);
        if (sym.empty())
            open_orders_cache_.clear();
        else
        {
            open_orders_cache_.erase(
                std::remove_if(open_orders_cache_.begin(), open_orders_cache_.end(),
                               [&](const order_info& o) { return o.symbol == sym; }),
                open_orders_cache_.end());
        }
        open_orders_cache_ms_ = now_ms();
    }
    return cancelled;
}

bool binance_interface::is_terminal_order_status(const std::string& status)
{
    return status == "FILLED"
        || status == "CANCELED"
        || status == "CANCELLED"
        || status == "EXPIRED"
        || status == "REJECTED";
}

bool binance_interface::is_missing_order_error(const std::string& message)
{
    return message.find("\"code\":-2013") != std::string::npos
        || message.find("\"code\":-2011") != std::string::npos
        || message.find("Order does not exist") != std::string::npos
        || message.find("Unknown order sent") != std::string::npos;
}

bool binance_interface::is_reduce_only_rejected(const std::string& message)
{
    return message.find("\"code\":-2022") != std::string::npos
        || message.find("ReduceOnly Order is rejected") != std::string::npos;
}

void binance_interface::forget_open_order(long long order_id)
{
    erase_open_order_cache(order_id);
}

void binance_interface::replace_open_order_cache(std::vector<order_info> orders)
{
    std::lock_guard lock(open_orders_mutex_);
    open_orders_cache_ = std::move(orders);
    open_orders_cache_ms_ = now_ms();
}

void binance_interface::upsert_open_order_cache(const order_info& order)
{
    if (order.order_id == 0)
        return;
    std::lock_guard lock(open_orders_mutex_);
    if (is_terminal_order_status(order.status))
    {
        open_orders_cache_.erase(
            std::remove_if(open_orders_cache_.begin(), open_orders_cache_.end(),
                           [&](const order_info& o) { return o.order_id == order.order_id; }),
            open_orders_cache_.end());
        return;
    }
    for (auto& o : open_orders_cache_)
    {
        if (o.order_id == order.order_id)
        {
            o = order;
            return;
        }
    }
    open_orders_cache_.push_back(order);
}

void binance_interface::erase_open_order_cache(long long order_id)
{
    if (order_id == 0)
        return;
    std::lock_guard lock(open_orders_mutex_);
    open_orders_cache_.erase(
        std::remove_if(open_orders_cache_.begin(), open_orders_cache_.end(),
                       [&](const order_info& o) { return o.order_id == order_id; }),
        open_orders_cache_.end());
}

std::vector<order_info> binance_interface::snapshot_open_order_cache(const std::string& symbol) const
{
    std::lock_guard lock(open_orders_mutex_);
    if (symbol.empty())
        return open_orders_cache_;
    std::vector<order_info> out;
    out.reserve(open_orders_cache_.size());
    for (const auto& o : open_orders_cache_)
    {
        if (o.symbol == symbol)
            out.push_back(o);
    }
    return out;
}

void binance_interface::refresh_open_order_cache_rest(const std::string& symbol)
{
    param_map params;
    if (!symbol.empty())
        params["symbol"] = symbol;
    const std::string response = signed_fapi_rest("GET", "/fapi/v1/openOrders", std::move(params));
    replace_open_order_cache(parse_open_orders(response));
}

orderbook_info binance_interface::parse_orderbook(const std::string& json, const std::string& symbol)
{
    if (json.find("\"status\"") != std::string::npos)
        ensure_ws_ok(json, "depth");

    std::string payload = json;
    if (json.find("\"result\"") != std::string::npos)
        payload = extract_result_object(json);

    // 现货 Partial Depth: bids/asks + lastUpdateId
    // 合约 Depth: b/a + u（最终更新 ID）
    const bool spot_keys = payload.find("\"bids\":") != std::string::npos
        && payload.find("\"asks\":") != std::string::npos;
    const bool futures_keys = payload.find("\"b\":") != std::string::npos
        && payload.find("\"a\":") != std::string::npos;
    if (!spot_keys && !futures_keys)
        throw std::runtime_error("orderbook fields not found in response: " + json);

    orderbook_info book;
    book.symbol = symbol;
    book.last_update_id = extract_json_int(payload, "lastUpdateId");
    if (book.last_update_id == 0)
        book.last_update_id = extract_json_int(payload, "u");
    book.first_update_id = extract_json_int(payload, "U");
    book.prev_update_id = extract_json_int(payload, "pu");
    book.transact_time = extract_json_int(payload, "T");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "transactTime");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "E");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "eventTime");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(json, "T");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(json, "E");

    if (spot_keys)
    {
        book.bids = parse_price_levels(payload, "bids");
        book.asks = parse_price_levels(payload, "asks");
    }
    else
    {
        book.bids = parse_price_levels(payload, "b");
        book.asks = parse_price_levels(payload, "a");
    }
    return book;
}

funding_info binance_interface::parse_funding(const std::string& json, const std::string& symbol)
{
    auto parse_float_field = [](const std::string& payload, const std::string& key) -> float
    {
        const std::string s = extract_json_string(payload, key);
        if (s.empty())
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    };

    funding_info info;
    info.symbol = symbol;
    // WS markPrice: r/p/i/T/E；REST premiumIndex: lastFundingRate/markPrice/indexPrice/nextFundingTime
    info.funding_rate = parse_float_field(json, "r");
    if (info.funding_rate == 0.f)
        info.funding_rate = parse_float_field(json, "lastFundingRate");
    info.mark_price = parse_float_field(json, "p");
    if (info.mark_price == 0.f)
        info.mark_price = parse_float_field(json, "markPrice");
    info.index_price = parse_float_field(json, "i");
    if (info.index_price == 0.f)
        info.index_price = parse_float_field(json, "indexPrice");
    info.next_funding_time = extract_json_int(json, "T");
    if (info.next_funding_time == 0)
        info.next_funding_time = extract_json_int(json, "nextFundingTime");
    info.event_time = extract_json_int(json, "E");
    if (info.event_time == 0)
        info.event_time = extract_json_int(json, "time");

    const std::string sym = extract_json_string(json, "s");
    if (sym.empty())
    {
        const std::string sym2 = extract_json_string(json, "symbol");
        if (!sym2.empty())
            info.symbol = sym2;
    }
    else
    {
        info.symbol = sym;
    }
    return info;
}

agg_trade_info binance_interface::parse_agg_trade(const std::string& json, const std::string& symbol)
{
    auto parse_float_field = [](const std::string& payload, const std::string& key) -> float
    {
        const std::string s = extract_json_string(payload, key);
        if (s.empty())
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    };

    agg_trade_info trade;
    trade.symbol = symbol;
    const std::string sym = extract_json_string(json, "s");
    if (!sym.empty())
        trade.symbol = sym;
    trade.price = parse_float_field(json, "p");
    trade.quantity = parse_float_field(json, "q");
    trade.buyer_is_maker = extract_json_bool(json, "m", false);
    trade.trade_time = extract_json_int(json, "T");
    if (trade.trade_time == 0)
        trade.trade_time = extract_json_int(json, "E");
    return trade;
}

order_info binance_interface::parse_order(const std::string& json)
{
    return parse_order_fields(extract_result_object(json));
}

order_info binance_interface::parse_user_stream_order(const std::string& order_obj)
{
    order_info order;
    order.symbol = extract_json_string(order_obj, "s");
    order.order_id = extract_json_int(order_obj, "i");
    order.client_order_id = extract_json_string(order_obj, "c");
    order.status = extract_json_string(order_obj, "X");
    order.side = extract_json_string(order_obj, "S");
    order.type = extract_json_string(order_obj, "o");
    order.time_in_force = extract_json_string(order_obj, "f");
    order.price = extract_json_string(order_obj, "p");
    order.stop_price = extract_json_string(order_obj, "sp");
    order.orig_qty = extract_json_string(order_obj, "q");
    order.executed_qty = extract_json_string(order_obj, "z");
    order.cummulative_quote_qty = extract_json_string(order_obj, "Z");
    order.position_side = extract_json_string(order_obj, "ps");
    order.transact_time = extract_json_int(order_obj, "T");
    if (order.transact_time == 0)
        order.transact_time = extract_json_int(order_obj, "E");
    order.raw_json = order_obj;
    return order;
}

std::vector<order_info> binance_interface::parse_open_orders(const std::string& json)
{
    std::vector<order_info> orders;
    std::string payload = json;
    if (!payload.empty() && payload.front() == '[')
        payload = "{\"result\":" + payload + "}";
    auto objs = extract_json_object_array(payload, "result");
    if (objs.empty())
    {
        // 个别环境下 result 可能是单对象
        const auto result_pos = json.find("\"result\"");
        if (result_pos != std::string::npos)
        {
            const auto brace = json.find('{', result_pos);
            const auto bracket = json.find('[', result_pos);
            if (brace != std::string::npos && (bracket == std::string::npos || brace < bracket))
                objs.push_back(extract_result_object(json));
        }
    }
    orders.reserve(objs.size());
    for (const auto& obj : objs)
        orders.push_back(parse_order_fields(obj));
    return orders;
}

std::vector<position_info> binance_interface::parse_positions(const std::string& json, bool only_open)
{
    std::vector<position_info> positions;
    const auto objs = extract_json_object_array(json, "result");
    positions.reserve(objs.size());
    for (const auto& obj : objs)
    {
        position_info pos;
        pos.symbol = extract_json_string(obj, "symbol");
        pos.position_amt = extract_json_float(obj, "positionAmt");
        pos.entry_price = extract_json_float(obj, "entryPrice");
        pos.mark_price = extract_json_float(obj, "markPrice");
        pos.unrealized_profit = extract_json_float(obj, "unRealizedProfit");
        if (pos.unrealized_profit == 0.f)
            pos.unrealized_profit = extract_json_float(obj, "unrealizedProfit");
        pos.liquidation_price = extract_json_float(obj, "liquidationPrice");
        pos.leverage = extract_json_float(obj, "leverage");
        pos.notional = extract_json_float(obj, "notional");
        if (pos.notional == 0.f)
            pos.notional = extract_json_float(obj, "notionalValue");
        pos.isolated_margin = extract_json_float(obj, "isolatedMargin");
        pos.margin_type = extract_json_string(obj, "marginType");
        pos.position_side = extract_json_string(obj, "positionSide");
        pos.update_time = extract_json_int(obj, "updateTime");
        pos.raw_json = obj;

        if (only_open && std::fabs(pos.position_amt) < 1e-12f)
            continue;
        if (pos.symbol.empty())
            continue;
        positions.push_back(std::move(pos));
    }
    return positions;
}

std::string binance_interface::signed_ws_call(RpcChannel& channel, const std::string& method, param_map params) const
{
    params["apiKey"] = api_key_;
    params["timestamp"] = std::to_string(now_ms());

    std::string payload;
    for (const auto& [key, value] : params)
    {
        if (!payload.empty())
            payload += '&';
        payload += key;
        payload += '=';
        payload += value;
    }
    params["signature"] = ed25519_sign_base64(payload);

    std::string req_id = std::to_string(now_ms()) + "-";
    req_id.reserve(req_id.size() + method.size());
    for (char c : method)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        req_id.push_back((std::isalnum(uc) || c == '-' || c == '_') ? c : '-');
    }
    if (req_id.size() > 36)
        req_id.resize(36);
    std::ostringstream req;
    req << "{\"id\":\"" << json_escape(req_id) << "\",\"method\":\"" << json_escape(method) << "\",\"params\":{";
    bool first = true;
    for (const auto& [key, value] : params)
    {
        if (!first)
            req << ',';
        first = false;
        req << '"' << json_escape(key) << "\":";
        if (is_json_number_param(key) || is_json_bool_param(key))
            req << value;
        else
            req << '"' << json_escape(value) << '"';
    }
    req << "}}";

    return channel.call(req_id, req.str());
}

std::string binance_interface::signed_fapi_rest(const std::string& http_method, const std::string& path, param_map params) const
{
    params["timestamp"] = std::to_string(now_ms());

    std::string payload;
    for (const auto& [key, value] : params)
    {
        if (!payload.empty())
            payload += '&';
        payload += key;
        payload += '=';
        payload += value;
    }
    const std::string signature = ed25519_sign_base64(payload);
    const std::string host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    const std::string full_path = path + "?" + payload + "&signature=" + url_encode(signature);
    const std::string headers = "X-MBX-APIKEY: " + api_key_ + "\r\n";

    const auto result = https_call(host, http_method, full_path, headers);

    if (result.status != 200)
        throw std::runtime_error(std::string("fapi REST ") + result.body);
    if (!result.body.empty() && result.body.front() == '{')
    {
        const auto code = extract_json_int(result.body, "code");
        if (code < 0)
            throw std::runtime_error("fapi REST error: " + result.body);
    }
    return result.body;
}

long long binance_interface::now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string binance_interface::ed25519_sign_base64(const std::string& message) const
{
    const auto der = pem_to_der(private_key_pem_);
    const auto seed = ed25519_seed_from_pkcs8_der(der);
    return ed25519_sign_b64(seed, message);
}

std::vector<asset_balance> binance_interface::parse_balances(const std::string& json, bool omit_zero)
{
    std::vector<asset_balance> result;
    const auto balances_key = json.find("\"balances\"");
    if (balances_key == std::string::npos)
    {
        if (json.find("\"code\"") != std::string::npos || json.find("\"msg\"") != std::string::npos)
            throw std::runtime_error("Binance API error: " + json);
        throw std::runtime_error("balances field not found in response");
    }

    const auto arr_begin = json.find('[', balances_key);
    if (arr_begin == std::string::npos)
        throw std::runtime_error("balances array not found");

    size_t i = arr_begin + 1;
    while (i < json.size())
    {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t' || json[i] == ','))
            ++i;
        if (i >= json.size() || json[i] == ']')
            break;
        if (json[i] != '{')
            break;

        const auto obj_end = json.find('}', i);
        if (obj_end == std::string::npos)
            break;

        const std::string obj = json.substr(i, obj_end - i + 1);
        asset_balance bal;
        bal.asset = extract_json_string(obj, "asset");
        bal.free = extract_json_string(obj, "free");
        bal.locked = extract_json_string(obj, "locked");

        if (!bal.asset.empty())
        {
            if (!omit_zero || !is_zero_amount(bal.free) || !is_zero_amount(bal.locked))
                result.push_back(std::move(bal));
        }

        i = obj_end + 1;
    }

    return result;
}
