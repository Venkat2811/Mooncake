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

#include "tent/transport/shm/shm_arena.h"
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <random>

using namespace mooncake::tent;

DEFINE_int32(num_iterations, 100, "Number of allocation iterations");
DEFINE_int32(min_size_kb, 4, "Minimum allocation size in KB");
DEFINE_int32(max_size_kb, 1024, "Maximum allocation size in KB");

class ArenaAllocationBenchmark {
public:
    void run() {
        std::cout << "=== SHM Arena Allocation Benchmark ===" << std::endl;
        std::cout << "Iterations: " << FLAGS_num_iterations << std::endl;
        std::cout << "Size range: " << FLAGS_min_size_kb << " KB - "
                  << FLAGS_max_size_kb << " KB" << std::endl;
        std::cout << std::endl;

        benchmarkArenaAllocation();
        benchmarkBySize();
    }

private:
    void benchmarkArenaAllocation() {
        std::cout << "--- Arena Allocation (atomic bump allocator) ---" << std::endl;
        std::cout << std::endl;

        // Create arena
        ShmArena::Config config;
        config.pool_size = 64ULL * 1024 * 1024 * 1024;  // 64 GB
        config.shm_name_prefix = "/mooncake_arena_bench_";

        auto arena = std::make_shared<ShmArena>();
        auto status = arena->initialize(config);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to initialize arena: " << status.ToString();
            return;
        }

        std::vector<double> alloc_times;
        std::vector<ShmArena::Allocation> allocs;
        alloc_times.reserve(FLAGS_num_iterations);
        allocs.reserve(FLAGS_num_iterations);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> size_dist(
            FLAGS_min_size_kb * 1024,
            FLAGS_max_size_kb * 1024
        );

        // Warmup
        for (int i = 0; i < 10; ++i) {
            ShmArena::Allocation alloc;
            arena->allocate(4096, alloc);
        }

        // Benchmark allocations
        for (int i = 0; i < FLAGS_num_iterations; ++i) {
            size_t size = size_dist(gen);

            auto start = std::chrono::high_resolution_clock::now();
            ShmArena::Allocation alloc;
            auto alloc_status = arena->allocate(size, alloc);
            auto end = std::chrono::high_resolution_clock::now();

            if (!alloc_status.ok()) {
                LOG(WARNING) << "Allocation failed: " << alloc_status.ToString();
                continue;
            }

            double time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count();
            alloc_times.push_back(time_ns);
            allocs.push_back(alloc);
        }

        // Calculate statistics
        double mean = 0.0, min = 1e9, max = 0.0, total = 0.0;
        for (double t : alloc_times) {
            mean += t;
            total += t;
            if (t < min) min = t;
            if (t > max) max = t;
        }
        mean /= alloc_times.size();

        std::cout << "Results:" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Arena allocate(): mean=" << mean << " ns, "
                  << "min=" << min << " ns, "
                  << "max=" << max << " ns, "
                  << "total=" << total / 1e6 << " ms" << std::endl;
        std::cout << std::endl;
        std::cout << "Throughput: " << std::fixed << std::setprecision(2)
                  << (FLAGS_num_iterations / (total / 1e9)) << " allocations/sec"
                  << std::endl;
        std::cout << std::endl;
    }

    void benchmarkBySize() {
        std::cout << "--- Arena Allocation Performance by Size ---" << std::endl;

        // Create arena
        ShmArena::Config config;
        config.pool_size = 64ULL * 1024 * 1024 * 1024;  // 64 GB
        config.shm_name_prefix = "/mooncake_arena_bench_bysize_";

        auto arena = std::make_shared<ShmArena>();
        auto status = arena->initialize(config);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to initialize arena: " << status.ToString();
            return;
        }

        std::vector<size_t> sizes = {
            4 * 1024,      // 4 KB
            8 * 1024,      // 8 KB
            16 * 1024,     // 16 KB
            32 * 1024,     // 32 KB
            64 * 1024,     // 64 KB
            128 * 1024,    // 128 KB
            256 * 1024,    // 256 KB
            512 * 1024,    // 512 KB
            1024 * 1024    // 1024 KB
        };

        std::cout << std::setw(15) << "Size (KB)"
                  << std::setw(20) << "Mean Time (ns)"
                  << std::setw(20) << "Throughput (MB/s)" << std::endl;
        std::cout << std::string(55, '-') << std::endl;

        for (size_t size : sizes) {
            std::vector<double> times;
            times.reserve(FLAGS_num_iterations);

            for (int i = 0; i < FLAGS_num_iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                ShmArena::Allocation alloc;
                auto alloc_status = arena->allocate(size, alloc);
                auto end = std::chrono::high_resolution_clock::now();

                if (!alloc_status.ok()) {
                    break;
                }

                double time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start).count();
                times.push_back(time_ns);
            }

            if (times.empty()) continue;

            double mean = 0.0;
            for (double t : times) mean += t;
            mean /= times.size();

            double throughput_mbps = (size / (1024.0 * 1024.0)) / (mean / 1e9);

            std::cout << std::setw(15) << (size / 1024)
                      << std::setw(20) << std::fixed << std::setprecision(2) << mean
                      << std::setw(20) << std::fixed << std::setprecision(2) << throughput_mbps
                      << std::endl;
        }

        std::cout << std::endl;
    }
};

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    ArenaAllocationBenchmark bench;
    bench.run();

    return 0;
}
