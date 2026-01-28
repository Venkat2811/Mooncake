// Integrity validation benchmark - verify both methods actually work
#include "tent/transport/shm/shm_arena.h"
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
#include <random>

using namespace mooncake::tent;

DEFINE_int32(num_iterations, 10000, "Number of iterations for validation");
DEFINE_int32(test_size_kb, 64, "Test allocation size in KB");

class IntegrityBenchmark {
public:
    void run() {
        std::cout << "=== Integrity Validation Benchmark ===" << std::endl;
        std::cout << "Iterations: " << FLAGS_num_iterations << std::endl;
        std::cout << "Allocation size: " << FLAGS_test_size_kb << " KB" << std::endl;
        std::cout << std::endl;

        // Test 1: Verify baseline actually allocates and works
        testBaselineIntegrity();

        // Test 2: Verify arena actually allocates and works
        testArenaIntegrity();

        // Test 3: Side-by-side comparison
        comparePerformance();

        // Test 4: Memory correctness test
        testMemoryCorrectness();
    }

private:
    void testBaselineIntegrity() {
        std::cout << "--- Test 1: Baseline Integrity Check ---" << std::endl;

        const size_t size = FLAGS_test_size_kb * 1024;
        const int iterations = 100;

        for (int i = 0; i < iterations; ++i) {
            std::string shm_name = "/integrity_baseline_" + std::to_string(getpid()) + "_" + std::to_string(i);

            // Allocate
            int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0644);
            if (shm_fd < 0) {
                std::cout << "ERROR: shm_open failed at iteration " << i << std::endl;
                return;
            }

            if (ftruncate64(shm_fd, size) == -1) {
                std::cout << "ERROR: ftruncate failed at iteration " << i << std::endl;
                close(shm_fd);
                return;
            }

            void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            if (addr == MAP_FAILED) {
                std::cout << "ERROR: mmap failed at iteration " << i << std::endl;
                close(shm_fd);
                return;
            }

            // Write test pattern
            uint64_t* ptr = static_cast<uint64_t*>(addr);
            ptr[0] = 0xDEADBEEFCAFEBABE;
            ptr[size/8 - 1] = 0xFEEDFACEDEADC0DE;

            // Verify
            if (ptr[0] != 0xDEADBEEFCAFEBABE || ptr[size/8 - 1] != 0xFEEDFACEDEADC0DE) {
                std::cout << "ERROR: Memory corruption at iteration " << i << std::endl;
                munmap(addr, size);
                close(shm_fd);
                return;
            }

            // Cleanup
            munmap(addr, size);
            close(shm_fd);
            shm_unlink(shm_name.c_str());
        }

        std::cout << "✓ Baseline: " << iterations << " allocations verified" << std::endl;
        std::cout << "✓ Memory reads/writes working correctly" << std::endl;
        std::cout << std::endl;
    }

    void testArenaIntegrity() {
        std::cout << "--- Test 2: Arena Integrity Check ---" << std::endl;

        const size_t size = FLAGS_test_size_kb * 1024;
        const int iterations = 100;

        // Create arena
        ShmArena::Config config;
        config.pool_size = 1ULL * 1024 * 1024 * 1024;  // 1 GB
        config.shm_name_prefix = "/integrity_arena_";

        auto arena = std::make_shared<ShmArena>();
        auto status = arena->initialize(config);
        if (!status.ok()) {
            std::cout << "ERROR: Arena initialization failed: " << status.ToString() << std::endl;
            return;
        }

        std::vector<ShmArena::Allocation> allocs;

        for (int i = 0; i < iterations; ++i) {
            ShmArena::Allocation alloc;
            auto alloc_status = arena->allocate(size, alloc);
            if (!alloc_status.ok()) {
                std::cout << "ERROR: Arena allocation failed at iteration " << i << ": "
                          << alloc_status.ToString() << std::endl;
                return;
            }

            if (alloc.addr == nullptr) {
                std::cout << "ERROR: Got null address at iteration " << i << std::endl;
                return;
            }

            // Write test pattern
            uint64_t* ptr = static_cast<uint64_t*>(alloc.addr);
            ptr[0] = 0xDEADBEEFCAFEBABE;
            ptr[size/8 - 1] = 0xFEEDFACEDEADC0DE;

            // Verify immediately
            if (ptr[0] != 0xDEADBEEFCAFEBABE || ptr[size/8 - 1] != 0xFEEDFACEDEADC0DE) {
                std::cout << "ERROR: Memory corruption at iteration " << i << std::endl;
                return;
            }

            allocs.push_back(alloc);
        }

        // Verify all allocations are still valid
        for (size_t i = 0; i < allocs.size(); ++i) {
            uint64_t* ptr = static_cast<uint64_t*>(allocs[i].addr);
            if (ptr[0] != 0xDEADBEEFCAFEBABE || ptr[size/8 - 1] != 0xFEEDFACEDEADC0DE) {
                std::cout << "ERROR: Memory corruption in allocation " << i << " after all allocations" << std::endl;
                return;
            }
        }

        std::cout << "✓ Arena: " << iterations << " allocations verified" << std::endl;
        std::cout << "✓ Memory reads/writes working correctly" << std::endl;
        std::cout << "✓ All allocations remain valid" << std::endl;
        std::cout << std::endl;
    }

    void comparePerformance() {
        std::cout << "--- Test 3: Side-by-Side Performance Comparison ---" << std::endl;

        const size_t size = FLAGS_test_size_kb * 1024;
        const int iterations = FLAGS_num_iterations;

        // Baseline timing
        auto baseline_start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            std::string shm_name = "/perf_baseline_" + std::to_string(getpid()) + "_" + std::to_string(i);
            int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0644);
            ftruncate64(shm_fd, size);
            void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

            // Touch memory to ensure it's real
            static_cast<char*>(addr)[0] = 'X';
            static_cast<char*>(addr)[size-1] = 'Y';

            munmap(addr, size);
            close(shm_fd);
            shm_unlink(shm_name.c_str());
        }
        auto baseline_end = std::chrono::high_resolution_clock::now();
        auto baseline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(baseline_end - baseline_start).count();
        double baseline_avg = static_cast<double>(baseline_ns) / iterations;

        // Arena timing
        ShmArena::Config config;
        config.pool_size = 10ULL * 1024 * 1024 * 1024;  // 10 GB
        config.shm_name_prefix = "/perf_arena_";

        auto arena = std::make_shared<ShmArena>();
        auto status = arena->initialize(config);
        if (!status.ok()) {
            std::cout << "ERROR: Arena init failed" << std::endl;
            return;
        }

        auto arena_start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            ShmArena::Allocation alloc;
            arena->allocate(size, alloc);

            // Touch memory to ensure it's real
            static_cast<char*>(alloc.addr)[0] = 'X';
            static_cast<char*>(alloc.addr)[alloc.size-1] = 'Y';
        }
        auto arena_end = std::chrono::high_resolution_clock::now();
        auto arena_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(arena_end - arena_start).count();
        double arena_avg = static_cast<double>(arena_ns) / iterations;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Baseline (shm_open+ftruncate+mmap+touch):" << std::endl;
        std::cout << "  Total time: " << (baseline_ns / 1e6) << " ms" << std::endl;
        std::cout << "  Average per allocation: " << baseline_avg << " ns" << std::endl;
        std::cout << "  Throughput: " << (iterations / (baseline_ns / 1e9)) << " alloc/sec" << std::endl;
        std::cout << std::endl;

        std::cout << "Arena (atomic+touch):" << std::endl;
        std::cout << "  Total time: " << (arena_ns / 1e6) << " ms" << std::endl;
        std::cout << "  Average per allocation: " << arena_avg << " ns" << std::endl;
        std::cout << "  Throughput: " << (iterations / (arena_ns / 1e9)) << " alloc/sec" << std::endl;
        std::cout << std::endl;

        double speedup = baseline_avg / arena_avg;
        std::cout << "SPEEDUP: " << speedup << "x faster" << std::endl;
        std::cout << std::endl;

        // Sanity check
        if (speedup < 10) {
            std::cout << "WARNING: Speedup is suspiciously low (< 10x)" << std::endl;
        } else if (speedup > 1000) {
            std::cout << "WARNING: Speedup is suspiciously high (> 1000x)" << std::endl;
        } else {
            std::cout << "✓ Speedup is in reasonable range (10-1000x)" << std::endl;
        }
        std::cout << std::endl;
    }

    void testMemoryCorrectness() {
        std::cout << "--- Test 4: Memory Correctness Test ---" << std::endl;

        const size_t size = 1024 * 1024;  // 1 MB
        const int num_allocs = 100;

        ShmArena::Config config;
        config.pool_size = 1ULL * 1024 * 1024 * 1024;  // 1 GB
        config.shm_name_prefix = "/correctness_arena_";

        auto arena = std::make_shared<ShmArena>();
        auto status = arena->initialize(config);
        if (!status.ok()) {
            std::cout << "ERROR: Arena init failed" << std::endl;
            return;
        }

        std::vector<ShmArena::Allocation> allocs;
        std::mt19937 rng(12345);
        std::uniform_int_distribution<uint64_t> dist;

        // Allocate and write unique patterns
        for (int i = 0; i < num_allocs; ++i) {
            ShmArena::Allocation alloc;
            arena->allocate(size, alloc);

            uint64_t* ptr = static_cast<uint64_t*>(alloc.addr);
            uint64_t pattern = dist(rng);

            // Fill entire allocation with pattern
            for (size_t j = 0; j < size / sizeof(uint64_t); ++j) {
                ptr[j] = pattern + j;
            }

            allocs.push_back(alloc);
        }

        // Verify all patterns are still correct
        rng.seed(12345);  // Reset RNG
        bool all_correct = true;
        for (size_t i = 0; i < allocs.size(); ++i) {
            uint64_t* ptr = static_cast<uint64_t*>(allocs[i].addr);
            uint64_t pattern = dist(rng);

            for (size_t j = 0; j < size / sizeof(uint64_t); ++j) {
                if (ptr[j] != pattern + j) {
                    std::cout << "ERROR: Memory corruption in allocation " << i
                              << " at offset " << j << std::endl;
                    std::cout << "  Expected: " << std::hex << (pattern + j)
                              << ", Got: " << ptr[j] << std::dec << std::endl;
                    all_correct = false;
                    break;
                }
            }
        }

        if (all_correct) {
            std::cout << "✓ All " << num_allocs << " allocations verified" << std::endl;
            std::cout << "✓ Total memory tested: " << (num_allocs * size / (1024.0 * 1024.0)) << " MB" << std::endl;
            std::cout << "✓ No memory corruption detected" << std::endl;
        }
        std::cout << std::endl;
    }
};

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    IntegrityBenchmark bench;
    bench.run();

    return 0;
}
