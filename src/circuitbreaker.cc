#include "circuitbreaker.h"
#include "logger.h"

CircuitBreaker::CircuitBreaker(int failure_threshold, int open_timeout_ms)
    : m_state(CLOSED),
      m_consecutive_failures(0),
      m_failure_threshold(failure_threshold),
      m_open_timeout_ms(open_timeout_ms),
      m_half_open_inflight(0)
{
}

bool CircuitBreaker::AllowRequest()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == CLOSED)
    {
        return true;
    }

    if (m_state == OPEN)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_open_timestamp).count();

        if (elapsed >= m_open_timeout_ms)
        {
            m_state = HALF_OPEN;
            m_half_open_inflight = 1;
            LOG_INFO("circuit state change: OPEN -> HALF_OPEN");
            return true;
        }

        return false;
    }

    // HALF_OPEN: 只允许一个探测请求
    if (m_state == HALF_OPEN)
    {
        if (m_half_open_inflight > 0)
        {
            return false;
        }

        m_half_open_inflight = 1;
        return true;
    }

    return true;
}

void CircuitBreaker::OnSuccess()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consecutive_failures = 0;
    m_half_open_inflight = 0;

    if (m_state != CLOSED)
    {
        LOG_INFO("circuit state change: %d -> CLOSED", m_state);
    }

    m_state = CLOSED;
}

void CircuitBreaker::OnFailure()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == HALF_OPEN)
    {
        Open();
        return;
    }

    if (m_state == CLOSED)
    {
        ++m_consecutive_failures;
        if (m_consecutive_failures >= m_failure_threshold)
        {
            Open();
        }
    }
}

CircuitState CircuitBreaker::GetState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void CircuitBreaker::Open()
{
    m_state = OPEN;
    m_open_timestamp = std::chrono::steady_clock::now();
    m_half_open_inflight = 0;
    LOG_ERR("circuit state change: -> OPEN");
}

void CircuitBreaker::Close()
{
    m_state = CLOSED;
    m_consecutive_failures = 0;
    m_half_open_inflight = 0;
}

CircuitBreakerManager& CircuitBreakerManager::GetInstance()
{
    static CircuitBreakerManager instance;
    return instance;
}

std::shared_ptr<CircuitBreaker> CircuitBreakerManager::GetBreaker(
    const std::string& service_name,
    const std::string& method_name)
{
    std::string key = service_name + "." + method_name;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_breakers.find(key);
    if (it != m_breakers.end())
    {
        return it->second;
    }

    auto breaker = std::make_shared<CircuitBreaker>();
    m_breakers.insert({key, breaker});
    return breaker;
}