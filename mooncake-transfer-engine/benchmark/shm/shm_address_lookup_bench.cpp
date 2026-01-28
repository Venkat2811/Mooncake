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
#include <map>
#include <iostream>
#include <iomanip>
#include <random>

DEFINE_int32(num_segments, 100, "Number of SHM segments to simulate");
DEFINE_int32(num_lookups, 10000, "Number of lookup operations");
DEFINE_int32(segment_size_mb, 64, "Size of each segment in MB");

namespace mooncake {
namespace tent {
namespace benchmark {

// Simulate the OpenedShmEntry structure
struct SimulatedShmEntry {
    void* shm_addr;
    size_t length;
    int shm_fd;
};

class AddressLookupBenchmark {
public:
    void run() {
        std::cout << "\n=== SHM Address Lookup Benchmark ===" << std::endl;
        std::cout << "Number of segments: " << FLAGS_num_segments << std::endl;
        std::cout << "Number of lookups: " << FLAGS_num_lookups << std::endl;
        std::cout << "Segment size: " << FLAGS_segment_size_mb << " MB" << std::endl;
        std::cout << std::endl;

        setupSegments();
        generateLookupAddresses();

        benchmarkLinearScan();
        benchmarkMapLookup();
        benchmarkArithmeticTranslation();
    }

private:
    struct Segment {
        uint64_t base_addr;
        size_t length;
        void* shm_addr;
    };

    std::vector<Segment> segments_;
    std::vector<std::pair<uint64_t, size_t>> lookup_requests_;

    void setupSegments() {
        // Simulate segments with varying base addresses
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dis(0x100000000ULL, 0x7FFFFFFFFFFF);

        size_t segment_size = FLAGS_segment_size_mb * 1024ULL * 1024ULL;

        for (int i = 0; i < FLAGS_num_segments; ++i) {
            Segment seg;
            seg.base_addr = dis(gen);
            seg.length = segment_size;
            seg.shm_addr = reinterpret_cast<void*>(0x200000000ULL + i * segment_size);
            segments_.push_back(seg);
        }

        std::cout << "Created " << segments_.size() << " simulated segments" << std::endl;
    }

    void generateLookupAddresses() {
        std::random_device rd;
        std::mt19937_64 gen(rd());

        for (int i = 0; i < FLAGS_num_lookups; ++i) {
            // Pick a random segment
            int seg_idx = gen() % segments_.size();
            const auto& seg = segments_[seg_idx];

            // Pick a random offset within the segment
            std::uniform_int_distribution<uint64_t> offset_dis(0, seg.length - 4096);
            uint64_t offset = offset_dis(gen);

            lookup_requests_.push_back({seg.base_addr + offset, 4096});
        }

        std::cout << "Generated " << lookup_requests_.size() << " lookup requests" << std::endl;
    }

    void benchmarkLinearScan() {
        std::cout << "\n--- Linear Scan (Current Mooncake Implementation) ---" << std::endl;

        // Build a map simulating the current relocate_map structure
        std::map<uint64_t, SimulatedShmEntry> relocate_map;
        for (const auto& seg : segments_) {
            SimulatedShmEntry entry;
            entry.shm_addr = seg.shm_addr;
            entry.length = seg.length;
            entry.shm_fd = 0;
            relocate_map[seg.base_addr] = entry;
        }

        int found_count = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& [dest_addr, length] : lookup_requests_) {
            // Linear scan through all entries (O(n))
            for (const auto& [base_addr, entry] : relocate_map) {
                if (base_addr <= dest_addr &&
                    dest_addr + length <= base_addr + entry.length) {
                    uint64_t translated = dest_addr - base_addr +
                                         reinterpret_cast<uint64_t>(entry.shm_addr);
                    found_count++;
                    break;
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();

        std::cout << "Found: " << found_count << " / " << lookup_requests_.size() << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total time: " << (elapsed_ns / 1e6) << " ms" << std::endl;
        std::cout << "Avg time per lookup: " << (elapsed_ns / lookup_requests_.size()) << " ns" << std::endl;
        std::cout << "Throughput: " << (lookup_requests_.size() * 1e9 / elapsed_ns) << " lookups/sec" << std::endl;
    }

    void benchmarkMapLookup() {
        std::cout << "\n--- Map Lower Bound (O(log n)) ---" << std::endl;

        // Build map sorted by base address
        std::map<uint64_t, SimulatedShmEntry> relocate_map;
        for (const auto& seg : segments_) {
            SimulatedShmEntry entry;
            entry.shm_addr = seg.shm_addr;
            entry.length = seg.length;
            entry.shm_fd = 0;
            relocate_map[seg.base_addr] = entry;
        }

        int found_count = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& [dest_addr, length] : lookup_requests_) {
            // Use lower_bound for O(log n) lookup
            auto it = relocate_map.lower_bound(dest_addr);
            if (it != relocate_map.begin()) {
                --it;
                if (it->first <= dest_addr &&
                    dest_addr + length <= it->first + it->second.length) {
                    uint64_t translated = dest_addr - it->first +
                                         reinterpret_cast<uint64_t>(it->second.shm_addr);
                    found_count++;
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();

        std::cout << "Found: " << found_count << " / " << lookup_requests_.size() << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total time: " << (elapsed_ns / 1e6) << " ms" << std::endl;
        std::cout << "Avg time per lookup: " << (elapsed_ns / lookup_requests_.size()) << " ns" << std::endl;
        std::cout << "Throughput: " << (lookup_requests_.size() * 1e9 / elapsed_ns) << " lookups/sec" << std::endl;
    }

    void benchmarkArithmeticTranslation() {
        std::cout << "\n--- Arithmetic Translation (O(1) - Flow-IPC Style) ---" << std::endl;

        // Simulate a single contiguous arena
        uint64_t pool_base_virtual = 0x100000000ULL;
        void* pool_base_shm = reinterpret_cast<void*>(0x200000000ULL);

        // For this benchmark, we'll pretend all segments are in one pool
        int found_count = 0;
        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& [dest_addr, length] : lookup_requests_) {
            // O(1) arithmetic translation
            // In reality, we'd check if the address is within the pool bounds
            // For this benchmark, we just do the arithmetic
            uint64_t offset = dest_addr - pool_base_virtual;
            uint64_t translated = reinterpret_cast<uint64_t>(pool_base_shm) + offset;

            // Simulate bounds check (still O(1))
            if (offset < (FLAGS_num_segments * FLAGS_segment_size_mb * 1024ULL * 1024ULL)) {
                found_count++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();

        std::cout << "Found: " << found_count << " / " << lookup_requests_.size() << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total time: " << (elapsed_ns / 1e6) << " ms" << std::endl;
        std::cout << "Avg time per lookup: " << (elapsed_ns / lookup_requests_.size()) << " ns" << std::endl;
        std::cout << "Throughput: " << (lookup_requests_.size() * 1e9 / elapsed_ns) << " lookups/sec" << std::endl;

        // Calculate speedup
        std::cout << "\n--- Summary ---" << std::endl;
        std::cout << "Arithmetic translation is O(1) vs O(n) linear scan" << std::endl;
        std::cout << "Expected speedup: ~" << FLAGS_num_segments << "x for " << FLAGS_num_segments << " segments" << std::endl;
    }
};

}  // namespace benchmark
}  // namespace tent
}  // namespace mooncake

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("SHM Address Lookup Benchmark\n"
                           "Compares linear scan vs optimized lookup strategies");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    mooncake::tent::benchmark::AddressLookupBenchmark bench;
    bench.run();

    return 0;
}
