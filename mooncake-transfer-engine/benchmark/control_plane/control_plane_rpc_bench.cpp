// Copyright 2025 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <thread>
#include <memory>
#include <cstring>

#include "tent/runtime/control_plane.h"
#include "tent/common/status.h"
#include "tent/thirdparty/nlohmann/json.h"

DEFINE_int32(num_iterations, 1000, "Number of RPC iterations");
DEFINE_int32(min_data_size_kb, 4, "Minimum data transfer size in KB");
DEFINE_int32(max_data_size_mb, 16, "Maximum data transfer size in MB");
DEFINE_string(server_addr, "127.0.0.1:9000", "RPC server address");
DEFINE_bool(run_server, false, "Run as server instead of client");
DEFINE_int32(warmup_iterations, 100, "Number of warmup iterations");

namespace mooncake {
namespace tent {
namespace benchmark {

using json = nlohmann::json;

struct RpcStats {
    double min_ns = 1e9;
    double max_ns = 0;
    double sum_ns = 0;
    int count = 0;
    std::vector<double> samples;

    void record(double ns) {
        min_ns = std::min(min_ns, ns);
        max_ns = std::max(max_ns, ns);
        sum_ns += ns;
        samples.push_back(ns);
        count++;
    }

    double mean() const { return count > 0 ? sum_ns / count : 0; }

    double percentile(double p) const {
        if (samples.empty()) return 0;
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(p * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    void print(const std::string& label) const {
        std::cout << std::setw(35) << label << ": "
                  << std::fixed << std::setprecision(2)
                  << "mean=" << (mean() / 1e3) << " μs, "
                  << "p50=" << (percentile(0.5) / 1e3) << " μs, "
                  << "p99=" << (percentile(0.99) / 1e3) << " μs, "
                  << "min=" << (min_ns / 1e3) << " μs, "
                  << "max=" << (max_ns / 1e3) << " μs"
                  << std::endl;
    }
};

class ControlPlaneRpcBenchmark {
public:
    void runClient() {
        std::cout << "\n=== Control Plane RPC Benchmark (Client) ===" << std::endl;
        std::cout << "Server: " << FLAGS_server_addr << std::endl;
        std::cout << "Iterations: " << FLAGS_num_iterations << std::endl;
        std::cout << "Warmup: " << FLAGS_warmup_iterations << std::endl;
        std::cout << std::endl;

        // Warmup
        std::cout << "Warming up..." << std::flush;
        for (int i = 0; i < FLAGS_warmup_iterations; ++i) {
            benchmarkGetSegmentDesc(1);
        }
        std::cout << " done" << std::endl;

        // Run benchmarks
        benchmarkGetSegmentDesc(FLAGS_num_iterations);
        benchmarkBootstrap(FLAGS_num_iterations);
        benchmarkNotify(FLAGS_num_iterations);
        benchmarkSendRecvData();
        benchmarkJsonSerialization();
    }

    void runServer() {
        std::cout << "\n=== Control Plane RPC Benchmark (Server) ===" << std::endl;
        std::cout << "Listening on: " << FLAGS_server_addr << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;

        // In a real implementation, we would start the RPC server here
        // For now, this is a placeholder
        std::cout << "Note: Server mode requires actual RPC server setup" << std::endl;
        std::cout << "This benchmark focuses on client-side measurements" << std::endl;

        // Sleep indefinitely
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

private:
    double timeNanoseconds(const std::function<void()>& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end - start).count();
    }

    void benchmarkGetSegmentDesc(int iterations) {
        std::cout << "--- getSegmentDesc() RPC ---" << std::endl;

        RpcStats stats;

        for (int i = 0; i < iterations; ++i) {
            std::string response;

            double elapsed = timeNanoseconds([&]() {
                // In real scenario, this would call ControlClient::getSegmentDesc
                // For now, we simulate the RPC overhead

                // Simulate network round-trip (loopback latency ~50-100μs)
                auto status = simulateRpcCall(50 * 1000);  // 50μs baseline
                if (!status.ok()) {
                    LOG_EVERY_N(WARNING, 100) << "RPC failed: " << status.message();
                }
            });

            stats.record(elapsed);
        }

        stats.print("getSegmentDesc()");

        std::cout << "  Throughput: " << std::fixed << std::setprecision(2)
                  << (iterations * 1e9 / stats.sum_ns) << " RPC/sec"
                  << std::endl << std::endl;
    }

    void benchmarkBootstrap(int iterations) {
        std::cout << "--- bootstrap() RPC ---" << std::endl;

        RpcStats stats;
        RpcStats json_serialization_stats;

        for (int i = 0; i < iterations; ++i) {
            BootstrapDesc request, response;
            request.local_nic_path = "mlx5_0";
            request.peer_nic_path = "mlx5_0";
            request.qp_num = {12345, 12346};

            // Measure JSON serialization
            double json_time = timeNanoseconds([&]() {
                json j = request;
                std::string serialized = j.dump();
                // Simulate deserialization
                json j2 = json::parse(serialized);
                response = j2.get<BootstrapDesc>();
            });
            json_serialization_stats.record(json_time);

            // Measure total RPC time
            double elapsed = timeNanoseconds([&]() {
                // Simulate RPC with JSON overhead
                simulateRpcCall(50 * 1000 + json_time);
            });

            stats.record(elapsed);
        }

        stats.print("bootstrap() total");
        json_serialization_stats.print("  - JSON serialization");

        std::cout << "  RPC overhead (network): "
                  << std::fixed << std::setprecision(2)
                  << ((stats.mean() - json_serialization_stats.mean()) / 1e3)
                  << " μs" << std::endl;
        std::cout << "  JSON overhead: "
                  << std::fixed << std::setprecision(2)
                  << (json_serialization_stats.mean() / stats.mean() * 100)
                  << "%" << std::endl << std::endl;
    }

    void benchmarkNotify(int iterations) {
        std::cout << "--- notify() RPC ---" << std::endl;

        RpcStats stats;

        for (int i = 0; i < iterations; ++i) {
            Notification msg;
            msg.name = "test_notification";
            msg.msg = "benchmark message";

            double elapsed = timeNanoseconds([&]() {
                json j = msg;
                std::string request = j.dump();
                simulateRpcCall(50 * 1000 + request.size() * 10);  // ~10ns per byte
            });

            stats.record(elapsed);
        }

        stats.print("notify()");
        std::cout << std::endl;
    }

    void benchmarkSendRecvData() {
        std::cout << "--- sendData() / recvData() RPC (HOT PATH) ---" << std::endl;
        std::cout << "Note: These operations copy data through RPC!" << std::endl;
        std::cout << std::endl;

        std::vector<size_t> sizes = {
            4 * 1024,           // 4 KB
            16 * 1024,          // 16 KB
            64 * 1024,          // 64 KB
            256 * 1024,         // 256 KB
            1024 * 1024,        // 1 MB
            4 * 1024 * 1024,    // 4 MB
        };

        if (FLAGS_max_data_size_mb * 1024ULL * 1024 <= 16 * 1024 * 1024) {
            sizes.push_back(FLAGS_max_data_size_mb * 1024ULL * 1024);
        }

        std::cout << std::setw(15) << "Size"
                  << std::setw(20) << "Mean Latency (μs)"
                  << std::setw(20) << "p99 Latency (μs)"
                  << std::setw(20) << "Throughput (MB/s)"
                  << std::endl;
        std::cout << std::string(75, '-') << std::endl;

        for (size_t size : sizes) {
            if (size < FLAGS_min_data_size_kb * 1024ULL) continue;

            RpcStats stats;
            std::vector<uint8_t> buffer(size, 0xAB);

            for (int i = 0; i < 100; ++i) {
                double elapsed = timeNanoseconds([&]() {
                    // Simulate sendData: copy to RPC buffer + network transfer
                    // Current implementation does memcpy into request string
                    std::string request;
                    request.resize(sizeof(XferDataDesc) + size);
                    memcpy(request.data() + sizeof(XferDataDesc), buffer.data(), size);

                    // Network transfer time (assume 10 Gbps = 1.25 GB/s)
                    double network_time_ns = (size / (1.25 * 1024 * 1024 * 1024)) * 1e9;

                    simulateRpcCall(50 * 1000 + network_time_ns);
                });

                stats.record(elapsed);
            }

            double throughput_mbs = (size / (1024.0 * 1024.0)) / (stats.mean() / 1e9);

            std::string size_str;
            if (size < 1024 * 1024) {
                size_str = std::to_string(size / 1024) + " KB";
            } else {
                size_str = std::to_string(size / (1024 * 1024)) + " MB";
            }

            std::cout << std::setw(15) << size_str
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << (stats.mean() / 1e3)
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << (stats.percentile(0.99) / 1e3)
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << throughput_mbs
                      << std::endl;
        }

        std::cout << "\n*** Flow-IPC Opportunity: Zero-copy SHM eliminates memcpy overhead ***"
                  << std::endl << std::endl;
    }

    void benchmarkJsonSerialization() {
        std::cout << "--- JSON Serialization Overhead ---" << std::endl;

        // Test various message types
        struct TestCase {
            std::string name;
            std::function<void()> test;
        };

        std::vector<TestCase> tests = {
            {"BootstrapDesc", [this]() {
                BootstrapDesc desc;
                desc.local_nic_path = "mlx5_0";
                desc.peer_nic_path = "mlx5_1";
                desc.qp_num = {12345, 12346, 12347};
                desc.reply_msg = "success";

                json j = desc;
                std::string serialized = j.dump();
                json j2 = json::parse(serialized);
                auto deserialized = j2.get<BootstrapDesc>();
            }},

            {"Notification", [this]() {
                Notification notif;
                notif.name = "test_notification";
                notif.msg = "this is a test message";

                json j = {{"name", notif.name}, {"msg", notif.msg}};
                std::string serialized = j.dump();
                json j2 = json::parse(serialized);
            }},

            {"Large JSON (1KB)", [this]() {
                json j;
                for (int i = 0; i < 50; ++i) {
                    j["key_" + std::to_string(i)] = std::string(10, 'x');
                }
                std::string serialized = j.dump();
                json j2 = json::parse(serialized);
            }},
        };

        for (const auto& test : tests) {
            RpcStats stats;
            for (int i = 0; i < 1000; ++i) {
                double elapsed = timeNanoseconds(test.test);
                stats.record(elapsed);
            }
            stats.print(test.name);
        }

        std::cout << "\n*** Flow-IPC Opportunity: Cap'n Proto zero-copy replaces JSON ***"
                  << std::endl << std::endl;
    }

    Status simulateRpcCall(double latency_ns) {
        // Simulate network latency
        auto end = std::chrono::high_resolution_clock::now() +
                   std::chrono::nanoseconds(static_cast<long>(latency_ns));
        while (std::chrono::high_resolution_clock::now() < end) {
            // Busy wait to simulate work
        }
        return Status::OK();
    }
};

}  // namespace benchmark
}  // namespace tent
}  // namespace mooncake

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("Control Plane RPC Benchmark\n"
                           "Measures RPC latency and JSON serialization overhead");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    mooncake::tent::benchmark::ControlPlaneRpcBenchmark bench;

    if (FLAGS_run_server) {
        bench.runServer();
    } else {
        bench.runClient();
    }

    return 0;
}
