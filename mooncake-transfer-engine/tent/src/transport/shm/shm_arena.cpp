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

#include <glog/logging.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace mooncake {
namespace tent {

std::atomic<uint32_t> ShmArena::next_arena_id_{0};

ShmArena::ShmArena() : arena_id_(next_arena_id_.fetch_add(1)) {
}

ShmArena::~ShmArena() {
    if (pool_base_ != nullptr && pool_base_ != MAP_FAILED) {
        munmap(pool_base_, pool_size_);
        pool_base_ = nullptr;
    }

    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }

    // Only unlink if we created the SHM (owner)
    if (is_owner_ && !shm_name_.empty()) {
        shm_unlink(shm_name_.c_str());
    }

    initialized_ = false;
}

Status ShmArena::initialize(const Config& config) {
    if (initialized_) {
        return Status::InvalidArgument("Arena already initialized");
    }

    config_ = config;
    pool_size_ = config.pool_size;

    // Generate unique SHM name
    shm_name_ = config.shm_name_prefix + std::to_string(getpid()) + "_" +
                std::to_string(arena_id_);

    // Create SHM object
    int flags = O_CREAT | O_RDWR | O_EXCL;
    shm_fd_ = shm_open(shm_name_.c_str(), flags, 0644);
    if (shm_fd_ < 0) {
        int err = errno;
        LOG(ERROR) << "shm_open failed for " << shm_name_
                   << ": " << strerror(err);
        return Status::InternalError("shm_open failed: " + std::string(strerror(err)));
    }

    is_owner_ = true;

    // Resize SHM to pool size
    if (ftruncate64(shm_fd_, pool_size_) < 0) {
        int err = errno;
        LOG(ERROR) << "ftruncate failed: " << strerror(err);
        close(shm_fd_);
        shm_fd_ = -1;
        shm_unlink(shm_name_.c_str());
        return Status::InternalError("ftruncate failed: " + std::string(strerror(err)));
    }

    // Map SHM into address space
    int mmap_flags = MAP_SHARED;
    if (config.use_huge_pages) {
        mmap_flags |= MAP_HUGETLB;
    }

    pool_base_ = mmap(nullptr, pool_size_, PROT_READ | PROT_WRITE,
                      mmap_flags, shm_fd_, 0);

    if (pool_base_ == MAP_FAILED) {
        int err = errno;
        LOG(ERROR) << "mmap failed: " << strerror(err);
        close(shm_fd_);
        shm_fd_ = -1;
        shm_unlink(shm_name_.c_str());
        pool_base_ = nullptr;
        return Status::InternalError("mmap failed: " + std::string(strerror(err)));
    }

    LOG(INFO) << "Created SHM arena '" << shm_name_ << "' at " << pool_base_
              << ", size=" << (pool_size_ / (1024.0 * 1024 * 1024)) << " GB";

    // Optionally prefault pages
    if (config.prefault_pages) {
        auto status = prefaultPages();
        if (!status.ok()) {
            LOG(WARNING) << "Prefault failed: " << status.message();
            // Not fatal, continue
        }
    }

    // Initialize allocation cursor
    alloc_cursor_.store(0, std::memory_order_relaxed);
    peak_allocated_.store(0, std::memory_order_relaxed);
    num_allocations_.store(0, std::memory_order_relaxed);
    num_failed_allocs_.store(0, std::memory_order_relaxed);

    initialized_ = true;
    return Status::OK();
}

Status ShmArena::attach(const std::string& arena_name, size_t expected_size) {
    if (initialized_) {
        return Status::InvalidArgument("Arena already initialized");
    }

    shm_name_ = arena_name;
    pool_size_ = expected_size;

    // Open existing SHM object
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0644);
    if (shm_fd_ < 0) {
        int err = errno;
        LOG(ERROR) << "shm_open failed for " << shm_name_
                   << ": " << strerror(err);
        return Status::InternalError("shm_open failed: " + std::string(strerror(err)));
    }

    is_owner_ = false;

    // Verify size
    struct stat sb;
    if (fstat(shm_fd_, &sb) < 0) {
        int err = errno;
        close(shm_fd_);
        shm_fd_ = -1;
        return Status::InternalError("fstat failed: " + std::string(strerror(err)));
    }

    if (static_cast<size_t>(sb.st_size) != expected_size) {
        LOG(ERROR) << "Size mismatch: expected " << expected_size
                   << ", got " << sb.st_size;
        close(shm_fd_);
        shm_fd_ = -1;
        return Status::InvalidArgument("Arena size mismatch");
    }

    // Map SHM into address space
    pool_base_ = mmap(nullptr, pool_size_, PROT_READ | PROT_WRITE,
                      MAP_SHARED, shm_fd_, 0);

    if (pool_base_ == MAP_FAILED) {
        int err = errno;
        LOG(ERROR) << "mmap failed: " << strerror(err);
        close(shm_fd_);
        shm_fd_ = -1;
        pool_base_ = nullptr;
        return Status::InternalError("mmap failed: " + std::string(strerror(err)));
    }

    LOG(INFO) << "Attached to SHM arena '" << shm_name_ << "' at " << pool_base_
              << ", size=" << (pool_size_ / (1024.0 * 1024 * 1024)) << " GB";

    initialized_ = true;
    return Status::OK();
}

Status ShmArena::allocate(size_t size, Allocation& alloc) {
    if (!initialized_) {
        return Status::InvalidArgument("Arena not initialized");
    }

    if (size == 0) {
        return Status::InvalidArgument("Cannot allocate 0 bytes");
    }

    // Align size to configured alignment
    size_t aligned_size = alignUp(size, config_.alignment);

    // Lock-free bump allocation using atomic fetch_add
    uint64_t offset = alloc_cursor_.fetch_add(aligned_size, std::memory_order_relaxed);

    // Check if we exceeded pool capacity
    if (offset + aligned_size > pool_size_) {
        // Out of memory - undo the allocation
        alloc_cursor_.fetch_sub(aligned_size, std::memory_order_relaxed);
        num_failed_allocs_.fetch_add(1, std::memory_order_relaxed);

        LOG_EVERY_N(WARNING, 100) << "Arena OOM: requested " << aligned_size
                                  << " bytes, only " << (pool_size_ - offset)
                                  << " bytes remaining";

        return Status::InternalError("Arena pool exhausted");
    }

    // Calculate virtual address
    void* addr = static_cast<char*>(pool_base_) + offset;

    // Update stats
    num_allocations_.fetch_add(1, std::memory_order_relaxed);

    uint64_t new_peak = offset + aligned_size;
    uint64_t old_peak = peak_allocated_.load(std::memory_order_relaxed);
    while (new_peak > old_peak &&
           !peak_allocated_.compare_exchange_weak(old_peak, new_peak,
                                                   std::memory_order_relaxed)) {
        // Retry if another thread updated peak
    }

    // Fill allocation structure
    alloc.addr = addr;
    alloc.offset = offset;
    alloc.size = aligned_size;
    alloc.arena_id = arena_id_;

    return Status::OK();
}

Status ShmArena::deallocate(const Allocation& alloc) {
    // Current implementation uses bump allocator, so dealloc is a no-op
    // Future: implement free list for memory reuse
    (void)alloc;  // Unused

    // Note: We could track deallocations for statistics,
    // but for now keep it zero-overhead
    return Status::OK();
}

Status ShmArena::translateOffset(uint64_t offset, size_t size, void** local_addr) {
    if (!initialized_) {
        return Status::InvalidArgument("Arena not initialized");
    }

    // Bounds check
    if (!isValidRange(offset, size)) {
        return Status::InvalidArgument("Offset out of bounds");
    }

    // O(1) arithmetic translation
    *local_addr = static_cast<char*>(pool_base_) + offset;

    return Status::OK();
}

uint64_t ShmArena::getOffset(void* addr) const {
    if (!initialized_ || addr == nullptr) {
        return static_cast<uint64_t>(-1);
    }

    // Check if address is within arena
    char* addr_char = static_cast<char*>(addr);
    char* base_char = static_cast<char*>(pool_base_);

    if (addr_char < base_char || addr_char >= base_char + pool_size_) {
        return static_cast<uint64_t>(-1);
    }

    return static_cast<uint64_t>(addr_char - base_char);
}

Status ShmArena::reset() {
    if (!initialized_) {
        return Status::InvalidArgument("Arena not initialized");
    }

    LOG(WARNING) << "Resetting arena " << shm_name_
                 << " - all allocations will be invalidated!";

    // Reset allocation cursor
    alloc_cursor_.store(0, std::memory_order_release);

    // Optionally zero out memory (expensive!)
    // memset(pool_base_, 0, pool_size_);

    return Status::OK();
}

ShmArena::Stats ShmArena::getStats() const {
    Stats stats;
    stats.pool_size = pool_size_;
    stats.allocated_bytes = alloc_cursor_.load(std::memory_order_relaxed);
    stats.peak_allocated = peak_allocated_.load(std::memory_order_relaxed);
    stats.num_allocations = num_allocations_.load(std::memory_order_relaxed);
    stats.num_failed_allocs = num_failed_allocs_.load(std::memory_order_relaxed);

    // Fragmentation is N/A for bump allocator (no reuse)
    stats.fragmentation_ratio = 0.0;

    return stats;
}

Status ShmArena::prefaultPages() {
    LOG(INFO) << "Prefaulting " << (pool_size_ / (1024 * 1024)) << " MB...";

    // Touch each page to force kernel to allocate physical pages
    const size_t page_size = 4096;  // Standard page size
    volatile char* ptr = static_cast<volatile char*>(pool_base_);

    for (size_t offset = 0; offset < pool_size_; offset += page_size) {
        ptr[offset] = 0;  // Write to force page allocation
    }

    LOG(INFO) << "Prefault complete";
    return Status::OK();
}

// ============================================================================
// ShmArenaPoolManager implementation
// ============================================================================

ShmArenaPoolManager& ShmArenaPoolManager::getInstance() {
    static ShmArenaPoolManager instance;
    return instance;
}

Status ShmArenaPoolManager::getOrCreateArena(const std::string& name,
                                              const ShmArena::Config& config,
                                              std::shared_ptr<ShmArena>& arena) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if arena already exists
    auto it = arenas_.find(name);
    if (it != arenas_.end()) {
        arena = it->second;
        return Status::OK();
    }

    // Create new arena
    auto new_arena = std::make_shared<ShmArena>();
    auto status = new_arena->initialize(config);
    if (!status.ok()) {
        return status;
    }

    arenas_[name] = new_arena;
    arena = new_arena;

    return Status::OK();
}

Status ShmArenaPoolManager::attachArena(const std::string& name,
                                         size_t expected_size,
                                         std::shared_ptr<ShmArena>& arena) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already attached
    auto it = arenas_.find(name);
    if (it != arenas_.end()) {
        arena = it->second;
        return Status::OK();
    }

    // Attach to existing arena
    auto new_arena = std::make_shared<ShmArena>();
    auto status = new_arena->attach(name, expected_size);
    if (!status.ok()) {
        return status;
    }

    arenas_[name] = new_arena;
    arena = new_arena;

    return Status::OK();
}

Status ShmArenaPoolManager::removeArena(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = arenas_.find(name);
    if (it == arenas_.end()) {
        return Status::InternalError("Arena not found: " + name);
    }

    arenas_.erase(it);
    return Status::OK();
}

std::vector<std::string> ShmArenaPoolManager::getArenaNames() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> names;
    names.reserve(arenas_.size());

    for (const auto& pair : arenas_) {
        names.push_back(pair.first);
    }

    return names;
}

}  // namespace tent
}  // namespace mooncake
