// Copyright 2026 KVCache.AI
// Simple arena allocator implementation

#include "mmap_arena.h"
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace mooncake {

// Helper to align size up to alignment boundary
static inline size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

MmapArena::MmapArena()
    : pool_base_(nullptr)
    , pool_size_(0)
    , alignment_(64)
    , alloc_cursor_(0)
    , peak_allocated_(0)
    , num_allocations_(0)
    , num_failed_allocs_(0)
{
}

MmapArena::~MmapArena() {
    if (pool_base_ != nullptr) {
        if (munmap(pool_base_, pool_size_) != 0) {
            LOG(ERROR) << "Arena munmap failed: " << strerror(errno);
        }
        pool_base_ = nullptr;
    }
}

bool MmapArena::initialize(size_t pool_size, size_t alignment) {
    if (pool_base_ != nullptr) {
        LOG(WARNING) << "Arena already initialized";
        return false;
    }

    alignment_ = std::max(alignment, size_t(64));

    // Align pool size to 2MB for huge pages
    const size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
    pool_size_ = align_up(pool_size, HUGE_PAGE_SIZE);

    // Allocate pool with mmap
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

    // Try huge pages for better performance
    #ifdef MAP_HUGETLB
    flags |= MAP_HUGETLB;
    #endif

    pool_base_ = mmap(nullptr, pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (pool_base_ == MAP_FAILED) {
        // Retry without huge pages
        flags &= ~MAP_HUGETLB;
        pool_base_ = mmap(nullptr, pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);

        if (pool_base_ == MAP_FAILED) {
            LOG(ERROR) << "Arena mmap failed: size=" << pool_size_
                      << ", errno=" << errno << " (" << strerror(errno) << ")";
            pool_base_ = nullptr;
            return false;
        }
        LOG(INFO) << "Arena initialized without huge pages";
    } else {
        LOG(INFO) << "Arena initialized with huge pages";
    }

    LOG(INFO) << "Arena initialized: " << (pool_size_ / (1024.0 * 1024.0 * 1024.0))
              << " GB, alignment=" << alignment_ << " bytes";

    return true;
}

void* MmapArena::allocate(size_t size) {
    if (pool_base_ == nullptr) {
        LOG(ERROR) << "Arena not initialized";
        return nullptr;
    }

    if (size == 0) {
        return nullptr;
    }

    // Align allocation size
    size_t aligned_size = align_up(size, alignment_);

    // Atomic bump allocation - lock-free!
    size_t offset = alloc_cursor_.fetch_add(aligned_size, std::memory_order_relaxed);

    // Check for overflow
    if (offset + aligned_size > pool_size_) {
        num_failed_allocs_.fetch_add(1, std::memory_order_relaxed);
        LOG(ERROR) << "Arena OOM: requested=" << size
                  << ", aligned=" << aligned_size
                  << ", offset=" << offset
                  << ", pool_size=" << pool_size_;
        return nullptr;
    }

    // Update stats
    num_allocations_.fetch_add(1, std::memory_order_relaxed);

    size_t new_peak = offset + aligned_size;
    size_t old_peak = peak_allocated_.load(std::memory_order_relaxed);
    while (new_peak > old_peak &&
           !peak_allocated_.compare_exchange_weak(old_peak, new_peak,
                                                   std::memory_order_relaxed)) {
        // CAS loop for peak tracking
    }

    void* ptr = static_cast<char*>(pool_base_) + offset;

    VLOG(2) << "[ARENA] Allocated: size=" << size
            << ", aligned=" << aligned_size
            << ", offset=" << offset
            << ", ptr=" << ptr
            << ", utilization=" << (100.0 * (offset + aligned_size) / pool_size_) << "%";

    return ptr;
}

MmapArena::Stats MmapArena::getStats() const {
    Stats stats;
    stats.pool_size = pool_size_;
    stats.allocated_bytes = alloc_cursor_.load(std::memory_order_relaxed);
    stats.peak_allocated = peak_allocated_.load(std::memory_order_relaxed);
    stats.num_allocations = num_allocations_.load(std::memory_order_relaxed);
    stats.num_failed_allocs = num_failed_allocs_.load(std::memory_order_relaxed);
    return stats;
}

} // namespace mooncake
