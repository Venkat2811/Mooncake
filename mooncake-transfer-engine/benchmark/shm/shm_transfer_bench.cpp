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
#include <cstring>

DEFINE_int32(transfer_size_kb, 4, "Transfer size in KB");
DEFINE_int32(max_transfer_size_mb, 64, "Maximum transfer size for sweep in MB");
DEFINE_int32(num_transfers, 1000, "Number of transfers per size");
DEFINE_bool(use_memcpy, true, "Use memcpy for transfers");
DEFINE_bool(verify_data, false, "Verify data after transfer");

namespace mooncake {
namespace tent {
namespace benchmark {

class ShmTransferBenchmark {
public:
    void run() {
        std::cout << "\n=== SHM Transfer Benchmark ===" << std::endl;
        std::cout << "Number of transfers: " << FLAGS_num_transfers << std::endl;
        std::cout << std::endl;

        if (!setupSharedMemory()) {
            LOG(ERROR) << "Failed to setup shared memory";
            return;
        }

        benchmarkSingleSize(FLAGS_transfer_size_kb * 1024);
        benchmarkSizeSweep();

        cleanup();
    }

private:
    void* src_shm_ = nullptr;
    void* dst_shm_ = nullptr;
    size_t shm_size_ = 128ULL * 1024 * 1024;  // 128 MB
    int src_fd_ = -1;
    int dst_fd_ = -1;

    bool setupSharedMemory() {
        // Create source SHM
        src_fd_ = shm_open("/mooncake_bench_src", O_CREAT | O_RDWR, 0644);
        if (src_fd_ < 0) {
            PLOG(ERROR) << "Failed to create source SHM";
            return false;
        }

        if (ftruncate64(src_fd_, shm_size_) < 0) {
            PLOG(ERROR) << "Failed to resize source SHM";
            return false;
        }

        src_shm_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, src_fd_, 0);
        if (src_shm_ == MAP_FAILED) {
            PLOG(ERROR) << "Failed to map source SHM";
            return false;
        }

        // Create destination SHM
        dst_fd_ = shm_open("/mooncake_bench_dst", O_CREAT | O_RDWR, 0644);
        if (dst_fd_ < 0) {
            PLOG(ERROR) << "Failed to create destination SHM";
            return false;
        }

        if (ftruncate64(dst_fd_, shm_size_) < 0) {
            PLOG(ERROR) << "Failed to resize destination SHM";
            return false;
        }

        dst_shm_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, dst_fd_, 0);
        if (dst_shm_ == MAP_FAILED) {
            PLOG(ERROR) << "Failed to map destination SHM";
            return false;
        }

        // Fill source with test data
        memset(src_shm_, 0xAB, shm_size_);

        std::cout << "Created " << (shm_size_ / (1024 * 1024)) << " MB SHM regions" << std::endl;
        return true;
    }

    void cleanup() {
        if (src_shm_ != nullptr && src_shm_ != MAP_FAILED) {
            munmap(src_shm_, shm_size_);
        }
        if (dst_shm_ != nullptr && dst_shm_ != MAP_FAILED) {
            munmap(dst_shm_, shm_size_);
        }
        if (src_fd_ >= 0) {
            close(src_fd_);
        }
        if (dst_fd_ >= 0) {
            close(dst_fd_);
        }
        shm_unlink("/mooncake_bench_src");
        shm_unlink("/mooncake_bench_dst");
    }

    void benchmarkSingleSize(size_t transfer_size) {
        std::cout << "\n--- Single Transfer Size: "
                  << (transfer_size / 1024) << " KB ---" << std::endl;

        double total_time_ns = 0;
        size_t total_bytes = 0;

        for (int i = 0; i < FLAGS_num_transfers; ++i) {
            auto start = std::chrono::high_resolution_clock::now();

            if (FLAGS_use_memcpy) {
                memcpy(dst_shm_, src_shm_, transfer_size);
            } else {
                // Manual copy
                volatile char* src = static_cast<volatile char*>(src_shm_);
                volatile char* dst = static_cast<volatile char*>(dst_shm_);
                for (size_t j = 0; j < transfer_size; ++j) {
                    dst[j] = src[j];
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();

            total_time_ns += elapsed_ns;
            total_bytes += transfer_size;

            if (FLAGS_verify_data && i == 0) {
                if (memcmp(src_shm_, dst_shm_, transfer_size) != 0) {
                    LOG(ERROR) << "Data verification failed!";
                }
            }
        }

        double avg_time_ns = total_time_ns / FLAGS_num_transfers;
        double bandwidth_gbps = (total_bytes / (1024.0 * 1024 * 1024)) /
                                (total_time_ns / 1e9);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Average time per transfer: " << avg_time_ns << " ns" << std::endl;
        std::cout << "Average time per transfer: " << (avg_time_ns / 1e3) << " μs" << std::endl;
        std::cout << "Bandwidth: " << bandwidth_gbps << " GB/s" << std::endl;
        std::cout << "Total transfers: " << FLAGS_num_transfers << std::endl;
        std::cout << "Total data: " << (total_bytes / (1024.0 * 1024)) << " MB" << std::endl;
    }

    void benchmarkSizeSweep() {
        std::cout << "\n--- Transfer Performance by Size ---" << std::endl;
        std::cout << std::setw(20) << "Size"
                  << std::setw(20) << "Avg Time (μs)"
                  << std::setw(20) << "Bandwidth (GB/s)"
                  << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        std::vector<size_t> sizes = {
            1024,           // 1 KB
            4096,           // 4 KB
            16 * 1024,      // 16 KB
            64 * 1024,      // 64 KB
            256 * 1024,     // 256 KB
            1024 * 1024,    // 1 MB
            4 * 1024 * 1024,    // 4 MB
            16 * 1024 * 1024,   // 16 MB
        };

        if (FLAGS_max_transfer_size_mb * 1024ULL * 1024 <= shm_size_) {
            sizes.push_back(FLAGS_max_transfer_size_mb * 1024ULL * 1024);
        }

        for (size_t transfer_size : sizes) {
            if (transfer_size > shm_size_) {
                continue;
            }

            double total_time_ns = 0;
            int iterations = std::min(FLAGS_num_transfers, 100);

            for (int i = 0; i < iterations; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                memcpy(dst_shm_, src_shm_, transfer_size);
                auto end = std::chrono::high_resolution_clock::now();
                total_time_ns += std::chrono::duration<double, std::nano>(end - start).count();
            }

            double avg_time_ns = total_time_ns / iterations;
            double bandwidth_gbps = (transfer_size / (1024.0 * 1024 * 1024)) /
                                    (avg_time_ns / 1e9);

            std::string size_str;
            if (transfer_size < 1024 * 1024) {
                size_str = std::to_string(transfer_size / 1024) + " KB";
            } else {
                size_str = std::to_string(transfer_size / (1024 * 1024)) + " MB";
            }

            std::cout << std::setw(20) << size_str
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << (avg_time_ns / 1e3)
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << bandwidth_gbps
                      << std::endl;
        }
    }
};

}  // namespace benchmark
}  // namespace tent
}  // namespace mooncake

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("SHM Transfer Benchmark\n"
                           "Measures memcpy throughput in shared memory");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    mooncake::tent::benchmark::ShmTransferBenchmark bench;
    bench.run();

    return 0;
}
