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

#ifndef TENT_TRANSPORT_SHM_ARENA_H_
#define TENT_TRANSPORT_SHM_ARENA_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

#include "tent/common/status.h"

namespace mooncake {
namespace tent {

/**
 * @brief SHM Arena Pool - Flow-IPC inspired arena allocator
 *
 * This implementation provides key optimizations from Flow-IPC's jemalloc arena:
 * 1. Pre-allocated SHM pool eliminates per-allocation syscalls
 * 2. O(1) address translation via offset arithmetic
 * 3. Lock-free allocation path using bump allocation
 * 4. Thread-safe design suitable for multi-process scenarios
 *
 * Design decisions:
 * - Uses simple bump allocator instead of full jemalloc (for initial implementation)
 * - Single large SHM pool per arena instance
 * - Address translation is pure arithmetic (offset from base)
 * - No fragmentation handling in v1 (can be added later with free list)
 *
 * Performance targets (vs current shm_open/mmap approach):
 * - Allocation: 100x faster (~100ns vs ~10μs)
 * - Address translation: 100x faster (O(1) vs O(n))
 */
class ShmArena {
public:
    /**
     * @brief Configuration for SHM arena
     */
    struct Config {
        /// Total pool size in bytes (default: 64 GB)
        size_t pool_size = 64ULL * 1024 * 1024 * 1024;

        /// SHM object name prefix (will append machine ID)
        std::string shm_name_prefix = "/mooncake_arena_";

        /// Whether to use huge pages (2MB) for better TLB performance
        bool use_huge_pages = false;

        /// Alignment for allocations (default: 64 bytes for cache line)
        size_t alignment = 64;

        /// Whether to pre-fault pages (touch all pages at init)
        bool prefault_pages = false;
    };

    /**
     * @brief Allocation handle returned to users
     *
     * Contains both the virtual address and metadata needed for
     * remote process address translation.
     */
    struct Allocation {
        /// Virtual address in local process
        void* addr = nullptr;

        /// Offset from arena base (for remote translation)
        uint64_t offset = 0;

        /// Size of allocation
        size_t size = 0;

        /// Arena ID (for multi-arena scenarios)
        uint32_t arena_id = 0;

        bool isValid() const { return addr != nullptr; }
    };

    ShmArena();
    ~ShmArena();

    // Non-copyable, non-movable (manages SHM resources)
    ShmArena(const ShmArena&) = delete;
    ShmArena& operator=(const ShmArena&) = delete;

    /**
     * @brief Initialize the arena with pre-allocated SHM pool
     *
     * This performs the one-time setup:
     * 1. Creates large SHM segment (shm_open + ftruncate)
     * 2. Maps it into process address space (mmap)
     * 3. Optionally prefaults pages
     * 4. Initializes allocation metadata
     *
     * @param config Arena configuration
     * @return Status::OK() on success
     */
    Status initialize(const Config& config);

    /**
     * @brief Attach to an existing arena (for remote processes)
     *
     * Opens and maps the same SHM segment created by another process.
     * Used by worker processes to access the arena created by master.
     *
     * @param arena_name SHM name of the arena to attach
     * @param expected_size Expected pool size (for validation)
     * @return Status::OK() on success
     */
    Status attach(const std::string& arena_name, size_t expected_size);

    /**
     * @brief Allocate memory from the arena
     *
     * Fast path: Lock-free bump allocation using atomic fetch_add.
     * Returns both local address and offset for remote translation.
     *
     * @param size Number of bytes to allocate
     * @param alloc Output allocation handle
     * @return Status::OK() on success, Status::OutOfMemory() if pool exhausted
     */
    Status allocate(size_t size, Allocation& alloc);

    /**
     * @brief Deallocate memory (NOP in bump allocator)
     *
     * Current implementation uses bump allocator, so deallocate is a no-op.
     * Future implementations can add free list for reuse.
     *
     * @param alloc Allocation to free
     * @return Status::OK()
     */
    Status deallocate(const Allocation& alloc);

    /**
     * @brief Translate remote offset to local address
     *
     * O(1) arithmetic operation: local_addr = base_addr + offset
     * This is the key optimization vs O(n) map lookup in original impl.
     *
     * @param offset Offset from arena base
     * @param size Expected size (for bounds checking)
     * @param local_addr Output local virtual address
     * @return Status::OK() on success, Status::InvalidArgument() if out of bounds
     */
    Status translateOffset(uint64_t offset, size_t size, void** local_addr);

    /**
     * @brief Get offset from local address
     *
     * Inverse of translateOffset: offset = addr - base_addr
     *
     * @param addr Local virtual address
     * @return Offset from arena base, or -1 if addr not in arena
     */
    uint64_t getOffset(void* addr) const;

    /**
     * @brief Reset arena for reuse (advanced)
     *
     * Resets the allocation cursor to beginning.
     * WARNING: Only safe if all allocations have been abandoned.
     *
     * @return Status::OK()
     */
    Status reset();

    /**
     * @brief Get arena statistics
     */
    struct Stats {
        size_t pool_size;           ///< Total pool size
        size_t allocated_bytes;     ///< Currently allocated bytes
        size_t peak_allocated;      ///< Peak allocation
        size_t num_allocations;     ///< Total allocation count
        size_t num_failed_allocs;   ///< Failed allocations (OOM)
        double fragmentation_ratio; ///< Reserved for future use
    };

    Stats getStats() const;

    /**
     * @brief Get arena name (SHM object name)
     */
    std::string getName() const { return shm_name_; }

    /**
     * @brief Check if arena is initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Get base address of arena
     */
    void* getBaseAddress() const { return pool_base_; }

    /**
     * @brief Get pool size
     */
    size_t getPoolSize() const { return pool_size_; }

private:
    /// Align size up to alignment boundary
    size_t alignUp(size_t size, size_t alignment) const {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    /// Validate allocation is within bounds
    bool isValidRange(uint64_t offset, size_t size) const {
        return offset + size <= pool_size_;
    }

    /// Prefault all pages (touch each page to force mapping)
    Status prefaultPages();

private:
    // Arena state
    bool initialized_ = false;
    bool is_owner_ = false;  // true if we created the SHM, false if attached

    // SHM resources
    std::string shm_name_;
    int shm_fd_ = -1;
    void* pool_base_ = nullptr;
    size_t pool_size_ = 0;

    // Allocation state (lock-free bump allocator)
    std::atomic<uint64_t> alloc_cursor_{0};  ///< Next allocation offset

    // Statistics (for monitoring)
    std::atomic<uint64_t> peak_allocated_{0};
    std::atomic<uint64_t> num_allocations_{0};
    std::atomic<uint64_t> num_failed_allocs_{0};

    // Configuration
    Config config_;

    // Arena ID (for multi-arena support)
    static std::atomic<uint32_t> next_arena_id_;
    uint32_t arena_id_ = 0;
};

/**
 * @brief Arena Pool Manager - Manages multiple arenas
 *
 * For complex scenarios with multiple SHM pools per machine.
 * Currently simple, can be extended to support:
 * - NUMA-aware arena placement
 * - Per-GPU arenas
 * - Automatic arena creation on demand
 */
class ShmArenaPoolManager {
public:
    static ShmArenaPoolManager& getInstance();

    /**
     * @brief Create or get arena by name
     */
    Status getOrCreateArena(const std::string& name,
                           const ShmArena::Config& config,
                           std::shared_ptr<ShmArena>& arena);

    /**
     * @brief Attach to existing arena
     */
    Status attachArena(const std::string& name,
                      size_t expected_size,
                      std::shared_ptr<ShmArena>& arena);

    /**
     * @brief Remove arena from manager
     */
    Status removeArena(const std::string& name);

    /**
     * @brief Get all arena names
     */
    std::vector<std::string> getArenaNames() const;

private:
    ShmArenaPoolManager() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ShmArena>> arenas_;
};

}  // namespace tent
}  // namespace mooncake

#endif  // TENT_TRANSPORT_SHM_ARENA_H_
