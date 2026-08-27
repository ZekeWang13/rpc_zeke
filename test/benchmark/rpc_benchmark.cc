#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include "checkOrder.pb.h"
#include "mprpcapplication.h"
#include "servicediscovery.h"

namespace {
struct Options {
    int threads = 1;
    int warmup_seconds = 3;
    int duration_seconds = 10;
    std::string config_file;
};

bool ParsePositive(const char* value, int& target) {
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || parsed <= 0 || parsed > 3600) return false;
    target = static_cast<int>(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-i" && i + 1 < argc) {
            options.config_file = argv[++i];
            continue;
        }
        if ((arg == "--threads" || arg == "--warmup" || arg == "--duration") && i + 1 < argc) {
            int* target = arg == "--threads" ? &options.threads :
                          arg == "--warmup" ? &options.warmup_seconds : &options.duration_seconds;
            if (!ParsePositive(argv[++i], *target)) return false;
        }
    }
    return true;
}

size_t Percentile(std::vector<long long>& values, int percentage) {
    if (values.empty()) return 0;
    const size_t index = (values.size() - 1) * percentage / 100;
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return static_cast<size_t>(values[index]);
}
}

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options) || options.config_file.empty()) {
        std::cerr << "usage: rpc_benchmark -i <config> [--threads N] [--warmup N] [--duration N]" << std::endl;
        return 2;
    }

    char option_name[] = "-i";
    char* framework_argv[] = {argv[0], option_name, const_cast<char*>(options.config_file.c_str())};
    MprpcApplication::Init(3, framework_argv);
    ServiceDiscovery::GetInstance().Start();

    std::atomic<bool> stop(false);
    std::atomic<bool> measure(false);
    std::atomic<unsigned long long> success(0), failure(0);
    std::mutex latency_mutex;
    std::vector<long long> latencies_us;
    std::vector<std::thread> workers;

    for (int i = 0; i < options.threads; ++i) {
        workers.emplace_back([&] {
            fixbug::OrderServiceRPC_Stub stub(new MprpcChannel());
            std::vector<long long> local_latencies;
            while (!stop.load(std::memory_order_relaxed)) {
                fixbug::orderMessageRequest request;
                request.set_ordernumber("001");
                fixbug::orderMessageResponse response;
                MprpcController controller;
                const auto start = std::chrono::steady_clock::now();
                stub.check(&controller, &request, &response, nullptr);
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();

                if (!measure.load(std::memory_order_relaxed)) continue;
                if (controller.Failed()) {
                    ++failure;
                } else {
                    ++success;
                    local_latencies.push_back(elapsed);
                }
            }
            std::lock_guard<std::mutex> lock(latency_mutex);
            latencies_us.insert(latencies_us.end(), local_latencies.begin(), local_latencies.end());
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(options.warmup_seconds));
    measure.store(true);
    const auto measure_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(options.duration_seconds));
    measure.store(false);
    stop.store(true);
    for (auto& worker : workers) worker.join();
    const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - measure_start).count();

    const auto total_success = success.load();
    const auto total_failure = failure.load();
    std::sort(latencies_us.begin(), latencies_us.end());
    const double average_us = latencies_us.empty() ? 0.0 :
        static_cast<double>(std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0)) / latencies_us.size();
    std::cout << "threads=" << options.threads
              << " duration_s=" << elapsed_seconds
              << " success=" << total_success
              << " failed=" << total_failure
              << " qps=" << total_success / elapsed_seconds
              << " avg_ms=" << average_us / 1000.0
              << " p50_ms=" << Percentile(latencies_us, 50) / 1000.0
              << " p95_ms=" << Percentile(latencies_us, 95) / 1000.0
              << " p99_ms=" << Percentile(latencies_us, 99) / 1000.0
              << " max_ms=" << (latencies_us.empty() ? 0.0 : latencies_us.back() / 1000.0)
              << std::endl;
    return total_failure == 0 ? 0 : 1;
}
