// Honest benchmark - separate allocation vs page fault costs
#include "tent/transport/shm/shm_arena.h"
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <functional>

using namespace mooncake::tent;

DEFINE_int32(iterations, 1000, "Iterations");
DEFINE_int32(size_kb, 64, "Size in KB");

double timeOperation(std::function<void()> op, int iterations) {
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        op();
    }
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / (double)iterations;
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    const size_t size = FLAGS_size_kb * 1024;
    const int iters = FLAGS_iterations;

    std::cout << "=== Honest Performance Benchmark ===" << std::endl;
    std::cout << "Iterations: " << iters << ", Size: " << FLAGS_size_kb << " KB" << std::endl;
    std::cout << std::endl;

    // ===== BASELINE =====
    std::cout << "--- Baseline (shm_open/ftruncate/mmap) ---" << std::endl;

    // Test 1: Allocation only (no touch)
    double baseline_alloc_only = timeOperation([&]() {
        static int counter = 0;
        std::string name = "/bench_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0644);
        ftruncate64(fd, size);
        void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        munmap(addr, size);
        close(fd);
        shm_unlink(name.c_str());
    }, iters);

    std::cout << "Allocation (no touch):  " << std::fixed << std::setprecision(2)
              << baseline_alloc_only << " ns" << std::endl;

    // Test 2: Allocation + first touch
    double baseline_with_touch = timeOperation([&]() {
        static int counter = 0;
        std::string name = "/bench2_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0644);
        ftruncate64(fd, size);
        void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        // Touch every page (4KB pages)
        char* ptr = static_cast<char*>(addr);
        for (size_t i = 0; i < size; i += 4096) {
            ptr[i] = 0;
        }

        munmap(addr, size);
        close(fd);
        shm_unlink(name.c_str());
    }, iters);

    std::cout << "Allocation + touch:     " << baseline_with_touch << " ns" << std::endl;
    std::cout << "Touch overhead:         " << (baseline_with_touch - baseline_alloc_only) << " ns" << std::endl;
    std::cout << std::endl;

    // ===== ARENA =====
    std::cout << "--- Arena (atomic bump allocator) ---" << std::endl;

    ShmArena::Config config;
    config.pool_size = 1ULL * 1024 * 1024 * 1024;
    config.shm_name_prefix = "/arena_bench_";

    auto arena = std::make_shared<ShmArena>();
    if (!arena->initialize(config).ok()) {
        std::cerr << "Failed to init arena" << std::endl;
        return 1;
    }

    // Test 1: Allocation only
    double arena_alloc_only = timeOperation([&]() {
        ShmArena::Allocation alloc;
        arena->allocate(size, alloc);
        // Don't touch memory
    }, iters);

    std::cout << "Allocation (no touch):  " << arena_alloc_only << " ns" << std::endl;

    // Reset arena for next test
    arena.reset();
    arena = std::make_shared<ShmArena>();
    arena->initialize(config);

    // Test 2: Allocation + touch
    double arena_with_touch = timeOperation([&]() {
        ShmArena::Allocation alloc;
        arena->allocate(size, alloc);

        // Touch every page
        char* ptr = static_cast<char*>(alloc.addr);
        for (size_t i = 0; i < size; i += 4096) {
            ptr[i] = 0;
        }
    }, iters);

    std::cout << "Allocation + touch:     " << arena_with_touch << " ns" << std::endl;
    std::cout << "Touch overhead:         " << (arena_with_touch - arena_alloc_only) << " ns" << std::endl;
    std::cout << std::endl;

    // ===== COMPARISON =====
    std::cout << "--- Comparison ---" << std::endl;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Speedup (alloc only):       " << (baseline_alloc_only / arena_alloc_only) << "x" << std::endl;
    std::cout << "Speedup (alloc + touch):    " << (baseline_with_touch / arena_with_touch) << "x" << std::endl;
    std::cout << std::endl;

    std::cout << "--- Analysis ---" << std::endl;
    std::cout << "Baseline touch overhead: " << std::fixed << std::setprecision(1)
              << ((baseline_with_touch - baseline_alloc_only) / baseline_with_touch * 100) << "%" << std::endl;
    std::cout << "Arena touch overhead:    "
              << ((arena_with_touch - arena_alloc_only) / arena_with_touch * 100) << "%" << std::endl;
    std::cout << std::endl;

    std::cout << "CONCLUSION:" << std::endl;
    std::cout << "- Pure allocation speedup (metadata): " << (baseline_alloc_only / arena_alloc_only) << "x" << std::endl;
    std::cout << "- Real-world speedup (with page faults): " << (baseline_with_touch / arena_with_touch) << "x" << std::endl;

    return 0;
}
