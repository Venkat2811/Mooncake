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

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <cstring>

using namespace mooncake::tent;

class ShmArenaTest : public ::testing::Test {
protected:
    void SetUp() override {
        arena_ = std::make_shared<ShmArena>();
    }

    void TearDown() override {
        arena_.reset();
    }

    std::shared_ptr<ShmArena> arena_;
};

TEST_F(ShmArenaTest, BasicInitialization) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;  // 16 MB for testing

    auto status = arena_->initialize(config);
    ASSERT_TRUE(status.ok()) << status.message();

    EXPECT_TRUE(arena_->isInitialized());
    EXPECT_EQ(arena_->getPoolSize(), config.pool_size);
    EXPECT_NE(arena_->getBaseAddress(), nullptr);
}

TEST_F(ShmArenaTest, BasicAllocation) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    ASSERT_TRUE(arena_->initialize(config).ok());

    ShmArena::Allocation alloc;
    auto status = arena_->allocate(4096, alloc);

    ASSERT_TRUE(status.ok());
    EXPECT_NE(alloc.addr, nullptr);
    EXPECT_EQ(alloc.offset, 0);  // First allocation
    EXPECT_GE(alloc.size, 4096);  // May be aligned up

    // Write and read data
    std::memset(alloc.addr, 0xAB, 4096);
    EXPECT_EQ(static_cast<uint8_t*>(alloc.addr)[0], 0xAB);
}

TEST_F(ShmArenaTest, MultipleAllocations) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    config.alignment = 64;
    ASSERT_TRUE(arena_->initialize(config).ok());

    std::vector<ShmArena::Allocation> allocs;

    // Allocate 100 blocks
    for (int i = 0; i < 100; ++i) {
        ShmArena::Allocation alloc;
        auto status = arena_->allocate(1024, alloc);
        ASSERT_TRUE(status.ok()) << "Allocation " << i << " failed";
        allocs.push_back(alloc);

        // Write unique pattern
        std::memset(alloc.addr, i & 0xFF, 1024);
    }

    // Verify all allocations
    for (size_t i = 0; i < allocs.size(); ++i) {
        uint8_t* ptr = static_cast<uint8_t*>(allocs[i].addr);
        EXPECT_EQ(ptr[0], i & 0xFF) << "Allocation " << i << " corrupted";
    }

    // Check stats
    auto stats = arena_->getStats();
    EXPECT_GE(stats.allocated_bytes, 100 * 1024);
    EXPECT_EQ(stats.num_allocations, 100);
}

TEST_F(ShmArenaTest, AddressTranslation) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    ASSERT_TRUE(arena_->initialize(config).ok());

    ShmArena::Allocation alloc;
    ASSERT_TRUE(arena_->allocate(4096, alloc).ok());

    // Test offset -> address translation
    void* translated_addr = nullptr;
    auto status = arena_->translateOffset(alloc.offset, alloc.size, &translated_addr);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(translated_addr, alloc.addr);

    // Test address -> offset translation
    uint64_t offset = arena_->getOffset(alloc.addr);
    EXPECT_EQ(offset, alloc.offset);
}

TEST_F(ShmArenaTest, OutOfMemory) {
    ShmArena::Config config;
    config.pool_size = 1024 * 1024;  // Small pool: 1 MB
    ASSERT_TRUE(arena_->initialize(config).ok());

    std::vector<ShmArena::Allocation> allocs;

    // Allocate until OOM
    bool hit_oom = false;
    for (int i = 0; i < 1000; ++i) {
        ShmArena::Allocation alloc;
        auto status = arena_->allocate(64 * 1024, alloc);  // 64 KB each

        if (!status.ok()) {
            EXPECT_EQ(status.code(), Status::Code::kInternalError);
            hit_oom = true;
            break;
        }

        allocs.push_back(alloc);
    }

    EXPECT_TRUE(hit_oom) << "Should have hit OOM";

    // Verify stats
    auto stats = arena_->getStats();
    EXPECT_GT(stats.num_failed_allocs, 0);
}

TEST_F(ShmArenaTest, ConcurrentAllocation) {
    ShmArena::Config config;
    config.pool_size = 64 * 1024 * 1024;  // 64 MB
    ASSERT_TRUE(arena_->initialize(config).ok());

    const int num_threads = 8;
    const int allocs_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < allocs_per_thread; ++i) {
                ShmArena::Allocation alloc;
                auto status = arena_->allocate(4096, alloc);

                if (status.ok()) {
                    success_count++;
                    // Write thread-specific pattern
                    std::memset(alloc.addr, t, 4096);
                } else {
                    failure_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count + failure_count, num_threads * allocs_per_thread);
    EXPECT_GT(success_count, 0);

    // Check stats
    auto stats = arena_->getStats();
    EXPECT_EQ(stats.num_allocations, success_count.load());
}

TEST_F(ShmArenaTest, AttachToArena) {
    // Create arena in first instance
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    ASSERT_TRUE(arena_->initialize(config).ok());

    std::string arena_name = arena_->getName();
    size_t pool_size = arena_->getPoolSize();

    // Allocate and write data
    ShmArena::Allocation alloc;
    ASSERT_TRUE(arena_->allocate(4096, alloc).ok());
    std::memset(alloc.addr, 0xCD, 4096);

    // Create second instance and attach
    auto arena2 = std::make_shared<ShmArena>();
    auto status = arena2->attach(arena_name, pool_size);

    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_TRUE(arena2->isInitialized());

    // Translate offset in second arena
    void* addr2 = nullptr;
    ASSERT_TRUE(arena2->translateOffset(alloc.offset, alloc.size, &addr2).ok());

    // Verify data is visible
    EXPECT_EQ(static_cast<uint8_t*>(addr2)[0], 0xCD);
}

TEST_F(ShmArenaTest, Reset) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    ASSERT_TRUE(arena_->initialize(config).ok());

    // Allocate some memory
    ShmArena::Allocation alloc1;
    ASSERT_TRUE(arena_->allocate(4096, alloc1).ok());
    EXPECT_EQ(alloc1.offset, 0);

    ShmArena::Allocation alloc2;
    ASSERT_TRUE(arena_->allocate(4096, alloc2).ok());
    EXPECT_GT(alloc2.offset, 0);

    // Reset arena
    ASSERT_TRUE(arena_->reset().ok());

    // Next allocation should start from 0 again
    ShmArena::Allocation alloc3;
    ASSERT_TRUE(arena_->allocate(4096, alloc3).ok());
    EXPECT_EQ(alloc3.offset, 0);
}

TEST_F(ShmArenaTest, BoundsChecking) {
    ShmArena::Config config;
    config.pool_size = 1024 * 1024;  // 1 MB
    ASSERT_TRUE(arena_->initialize(config).ok());

    // Try to translate offset beyond pool
    void* addr = nullptr;
    auto status = arena_->translateOffset(2 * 1024 * 1024, 4096, &addr);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), Status::Code::kInvalidArgument);
}

TEST_F(ShmArenaTest, Statistics) {
    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;
    ASSERT_TRUE(arena_->initialize(config).ok());

    auto stats1 = arena_->getStats();
    EXPECT_EQ(stats1.allocated_bytes, 0);
    EXPECT_EQ(stats1.num_allocations, 0);

    // Make some allocations
    for (int i = 0; i < 10; ++i) {
        ShmArena::Allocation alloc;
        arena_->allocate(1024, alloc);
    }

    auto stats2 = arena_->getStats();
    EXPECT_GE(stats2.allocated_bytes, 10 * 1024);
    EXPECT_EQ(stats2.num_allocations, 10);
    EXPECT_EQ(stats2.peak_allocated, stats2.allocated_bytes);
}

// Test arena pool manager
TEST(ShmArenaPoolManagerTest, GetOrCreateArena) {
    auto& manager = ShmArenaPoolManager::getInstance();

    ShmArena::Config config;
    config.pool_size = 16 * 1024 * 1024;

    std::shared_ptr<ShmArena> arena1;
    auto status = manager.getOrCreateArena("test_arena", config, arena1);

    ASSERT_TRUE(status.ok());
    ASSERT_NE(arena1, nullptr);
    EXPECT_TRUE(arena1->isInitialized());

    // Get again - should return same instance
    std::shared_ptr<ShmArena> arena2;
    status = manager.getOrCreateArena("test_arena", config, arena2);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(arena1, arena2);  // Same pointer

    // Clean up
    manager.removeArena("test_arena");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
