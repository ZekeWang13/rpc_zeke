#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>

//熔断关闭、开启、开启后5秒半开启
enum CircuitState
{
    CLOSED = 0,
    OPEN,
    HALF_OPEN
};

//
class CircuitBreaker
{
public:
    CircuitBreaker(int failure_threshold = 3, int open_timeout_ms = 5000);

    bool AllowRequest();
    void OnSuccess();
    void OnFailure();
    CircuitState GetState() const;

private:
    void Open();
    void Close();

private:
    mutable std::mutex m_mutex;
    CircuitState m_state;
    int m_consecutive_failures;
    int m_failure_threshold;
    int m_open_timeout_ms;
    int m_half_open_inflight;
    std::chrono::steady_clock::time_point m_open_timestamp;
};

class CircuitBreakerManager
{
public:
    static CircuitBreakerManager& GetInstance();

    std::shared_ptr<CircuitBreaker> GetBreaker(const std::string& service_name,
                                               const std::string& method_name);

private:
    CircuitBreakerManager() = default;
    CircuitBreakerManager(const CircuitBreakerManager&) = delete;
    CircuitBreakerManager(CircuitBreakerManager&&) = delete;

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<CircuitBreaker>> m_breakers;
};