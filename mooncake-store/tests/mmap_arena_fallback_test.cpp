// Copyright 2026 KVCache.AI
// Fallback-path tests for the global mmap allocator wrapper.

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "utils.h"

namespace mooncake {

class MmapArenaFallbackTest : public ::testing::Test {
   protected:
    void SetUp() override {
        FLAGS_logtostderr = 1;
        FLAGS_minloglevel = google::WARNING;
        setenv("MC_DISABLE_MMAP_ARENA", "1", 1);
    }

    void TearDown() override { unsetenv("MC_DISABLE_MMAP_ARENA"); }
};

TEST_F(MmapArenaFallbackTest, HonorsPageAlignment) {
    const size_t alloc_size = 64 * 1024;
    constexpr size_t alignment = 64;

    void* ptr = allocate_buffer_mmap_memory(alloc_size, alignment);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0u);

    memset(ptr, 0xAB, alloc_size);
    EXPECT_EQ(static_cast<uint8_t*>(ptr)[0], 0xAB);
    EXPECT_EQ(static_cast<uint8_t*>(ptr)[alloc_size - 1], 0xAB);

    free_buffer_mmap_memory(ptr, alloc_size);
}

TEST_F(MmapArenaFallbackTest, NoHugepagesAllocFree) {
    unsetenv("MC_STORE_USE_HUGEPAGE");

    const size_t alloc_size = 65000;
    constexpr size_t alignment = 64;

    void* ptr = allocate_buffer_mmap_memory(alloc_size, alignment);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 4096, 0u);

    memset(ptr, 0xCD, alloc_size);
    EXPECT_EQ(static_cast<uint8_t*>(ptr)[0], 0xCD);

    free_buffer_mmap_memory(ptr, alloc_size);
}

TEST_F(MmapArenaFallbackTest, AllocateFreeCycle) {
    constexpr int kCycles = 8;
    constexpr size_t alloc_size = 128 * 1024;
    constexpr size_t alignment = 64;

    for (int i = 0; i < kCycles; ++i) {
        void* ptr = allocate_buffer_mmap_memory(alloc_size, alignment);
        ASSERT_NE(ptr, nullptr) << "Allocation failed on cycle " << i;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0u);
        memset(ptr, static_cast<uint8_t>(i), alloc_size);
        free_buffer_mmap_memory(ptr, alloc_size);
    }
}

}  // namespace mooncake

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    return RUN_ALL_TESTS();
}
