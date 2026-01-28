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
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <random>
#include <cstring>
#include <functional>

DEFINE_int32(num_iterations, 1000, "Number of allocation iterations");
DEFINE_int32(min_size_kb, 4, "Minimum allocation size in KB");
DEFINE_int32(max_size_kb, 1024, "Maximum allocation size in KB");
DEFINE_bool(measure_mmap_only, false, "Measure only mmap, not shm_open");
DEFINE_bool(cleanup, true, "Clean up SHM segments after test");

namespace mooncake {
namespace tent {
namespace benchmark {

struct AllocationStats {
    double min_ns = 1e9;
    double max_ns = 0;
    double sum_ns = 0;
    int count = 0;

    void record(double ns) {
        min_ns = std::min(min_ns, ns);
        max_ns = std::max(max_ns, ns);
        sum_ns += ns;
        count++;
    }

    double mean() const { return count > 0 ? sum_ns / count : 0; }

    void print(const std::string& label) const {
        std::cout << std::setw(30) << label << ": "
                  << std::fixed << std::setprecision(2)
                  << "mean=" << mean() << " ns, "
                  << "min=" << min_ns << " ns, "
                  << "max=" << max_ns << " ns, "
                  << "total=" << (sum_ns / 1e6) << " ms"
                  << std::endl;
    }
};

class ShmAllocationBenchmark {
public:
    void run() {
        std::cout << "\n=== SHM Allocation Benchmark ===" << std::endl;
        std::cout << "Iterations: " << FLAGS_num_iterations << std::endl;
        std::cout << "Size range: " << FLAGS_min_size_kb << " KB - "
                  << FLAGS_max_size_kb << " KB" << std::endl;
        std::cout << std::endl;

        if (FLAGS_measure_mmap_only) {
            benchmarkMmapOnly();
        } else {
            benchmarkFullAllocation();
        }

        benchmarkBySize();
    }

private:
    double timeNanoseconds(const std::function<void()>& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end - start).count();
    }

    std::string generateShmName() {
        static int counter = 0;
        return "/mooncake_bench_" + std::to_string(getpid()) + "_" +
               std::to_string(counter++);
    }

    void benchmarkFullAllocation() {
        std::cout << "--- Full Allocation (shm_open + ftruncate + mmap) ---" << std::endl;

        AllocationStats shm_open_stats;
        AllocationStats ftruncate_stats;
        AllocationStats mmap_stats;
        AllocationStats total_stats;
        AllocationStats cleanup_stats;

        std::vector<std::string> shm_names;
        shm_names.reserve(FLAGS_num_iterations);

        size_t size = FLAGS_min_size_kb * 1024;

        for (int i = 0; i < FLAGS_num_iterations; ++i) {
            std::string shm_name = generateShmName();
            shm_names.push_back(shm_name);

            int shm_fd = -1;
            void* addr = nullptr;

            // Measure shm_open
            double shm_open_time = timeNanoseconds([&]() {
                shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0644);
            });

            if (shm_fd < 0) {
                PLOG(ERROR) << "shm_open failed";
                continue;
            }
            shm_open_stats.record(shm_open_time);

            // Measure ftruncate
            double ftruncate_time = timeNanoseconds([&]() {
                ftruncate64(shm_fd, size);
            });
            ftruncate_stats.record(ftruncate_time);

            // Measure mmap
            double mmap_time = timeNanoseconds([&]() {
                addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            });

            if (addr == MAP_FAILED) {
                PLOG(ERROR) << "mmap failed";
                close(shm_fd);
                continue;
            }
            mmap_stats.record(mmap_time);

            total_stats.record(shm_open_time + ftruncate_time + mmap_time);

            // Cleanup
            double cleanup_time = timeNanoseconds([&]() {
                munmap(addr, size);
                close(shm_fd);
                if (FLAGS_cleanup) {
                    shm_unlink(shm_name.c_str());
                }
            });
            cleanup_stats.record(cleanup_time);
        }

        std::cout << "\nResults:" << std::endl;
        shm_open_stats.print("shm_open()");
        ftruncate_stats.print("ftruncate()");
        mmap_stats.print("mmap()");
        total_stats.print("Total (all 3 syscalls)");
        cleanup_stats.print("Cleanup (munmap + close + unlink)");

        std::cout << "\nThroughput: "
                  << std::fixed << std::setprecision(2)
                  << (FLAGS_num_iterations * 1e9) / total_stats.sum_ns
                  << " allocations/sec" << std::endl;
    }

    void benchmarkMmapOnly() {
        std::cout << "--- mmap() Only (pre-created SHM) ---" << std::endl;

        // Pre-create a SHM segment
        std::string shm_name = generateShmName();
        size_t size = FLAGS_max_size_kb * 1024;

        int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0644);
        if (shm_fd < 0) {
            PLOG(ERROR) << "Failed to create SHM for benchmark";
            return;
        }

        if (ftruncate64(shm_fd, size) < 0) {
            PLOG(ERROR) << "Failed to resize SHM";
            close(shm_fd);
            return;
        }

        AllocationStats mmap_stats;

        for (int i = 0; i < FLAGS_num_iterations; ++i) {
            void* addr = nullptr;

            double mmap_time = timeNanoseconds([&]() {
                addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            });

            if (addr == MAP_FAILED) {
                PLOG(ERROR) << "mmap failed";
                continue;
            }

            mmap_stats.record(mmap_time);
            munmap(addr, size);
        }

        std::cout << "\nResults:" << std::endl;
        mmap_stats.print("mmap() only");

        close(shm_fd);
        if (FLAGS_cleanup) {
            shm_unlink(shm_name.c_str());
        }
    }

    void benchmarkBySize() {
        std::cout << "\n--- Allocation Performance by Size ---" << std::endl;
        std::cout << std::setw(15) << "Size (KB)"
                  << std::setw(20) << "Mean Time (ns)"
                  << std::setw(20) << "Throughput (MB/s)"
                  << std::endl;
        std::cout << std::string(55, '-') << std::endl;

        for (int size_kb = FLAGS_min_size_kb; size_kb <= FLAGS_max_size_kb; size_kb *= 2) {
            size_t size = size_kb * 1024;
            AllocationStats stats;

            for (int i = 0; i < 100; ++i) {  // 100 iterations per size
                std::string shm_name = generateShmName();

                double alloc_time = timeNanoseconds([&]() {
                    int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0644);
                    if (shm_fd < 0) return;
                    ftruncate64(shm_fd, size);
                    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, shm_fd, 0);
                    if (addr != MAP_FAILED) {
                        munmap(addr, size);
                    }
                    close(shm_fd);
                    if (FLAGS_cleanup) {
                        shm_unlink(shm_name.c_str());
                    }
                });

                stats.record(alloc_time);
            }

            double throughput_mbs = (size / (1024.0 * 1024.0)) / (stats.mean() / 1e9);

            std::cout << std::setw(15) << size_kb
                      << std::setw(20) << std::fixed << std::setprecision(2) << stats.mean()
                      << std::setw(20) << std::fixed << std::setprecision(2) << throughput_mbs
                      << std::endl;
        }
    }
};

}  // namespace benchmark
}  // namespace tent
}  // namespace mooncake

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("SHM Allocation Benchmark\n"
                           "Measures shm_open, ftruncate, and mmap performance");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    mooncake::tent::benchmark::ShmAllocationBenchmark bench;
    bench.run();

    return 0;
}
