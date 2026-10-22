#include "utils.h"

#include "config.h"
#include "ub_allocator.h"

#include <cstdlib>
#include <glog/logging.h>

#ifdef USE_CUDA
#include <cuda_runtime.h>
#endif
#if defined(USE_VRAM_SEGMENT) && defined(USE_MNNVL)
#include <cuda.h>
#endif
#ifdef USE_INTRA_NVLINK
#include "gpu_vendor/intra_nvlink.h"
#endif
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
#include "ascend_allocator.h"
#endif
#if defined(USE_SUNRISE)
#include "sunrise_allocator.h"
#endif
#ifdef USE_NOF
#include "spdk/spdk_wrapper.h"
#endif

namespace mooncake {

#if defined(USE_VRAM_SEGMENT) && defined(USE_MNNVL)
namespace {
// Store segment in GPU memory that other nodes of the same NVLink domain (MNNVL,
// e.g. the trays of a GB200/GB300 NVL72 rack) can import: cuMemCreate with a
// fabric handle, mapped on `device`. The NVLink transport exports it when the
// segment is registered (registerLocalMemory -> cuMemRetainAllocationHandle ->
// cuMemExportToShareableHandle(FABRIC)), and consumers map it on demand.
// Same recipe as AllocateFabricMemory in nvlink-allocator/nvlink_allocator.cpp.
void *allocate_fabric_vram(size_t size, int device) {
    CUdevice dev;
    if (cuDeviceGet(&dev, device) != CUDA_SUCCESS) return nullptr;
    int fabric = 0, rdma = 0;
    cuDeviceGetAttribute(&fabric,
                         CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED, dev);
    if (!fabric) {
        LOG(ERROR) << "VRAM Segment: device " << device
                   << " has no fabric handle support";
        return nullptr;
    }
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = dev;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;
    cuDeviceGetAttribute(
        &rdma, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED, dev);
    if (rdma) prop.allocFlags.gpuDirectRDMACapable = 1;
    size_t gran = 0;
    if (cuMemGetAllocationGranularity(&gran, &prop,
                                      CU_MEM_ALLOC_GRANULARITY_RECOMMENDED) !=
        CUDA_SUCCESS)
        return nullptr;
    size = (size + gran - 1) / gran * gran;
    CUmemGenericAllocationHandle handle;
    CUresult res = cuMemCreate(&handle, size, &prop, 0);
    if (res != CUDA_SUCCESS) {
        LOG(ERROR) << "VRAM Segment: cuMemCreate(FABRIC) of " << size
                   << " bytes failed: " << res;
        return nullptr;
    }
    CUdeviceptr ptr = 0;
    if (cuMemAddressReserve(&ptr, size, gran, 0, 0) != CUDA_SUCCESS) {
        cuMemRelease(handle);
        return nullptr;
    }
    if (cuMemMap(ptr, size, 0, handle, 0) != CUDA_SUCCESS) {
        cuMemAddressFree(ptr, size);
        cuMemRelease(handle);
        return nullptr;
    }
    CUmemAccessDesc access = {};
    access.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    access.location.id = dev;
    access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    if (cuMemSetAccess(ptr, size, &access, 1) != CUDA_SUCCESS) {
        cuMemUnmap(ptr, size);
        cuMemAddressFree(ptr, size);
        cuMemRelease(handle);
        return nullptr;
    }
    LOG(INFO) << "VRAM Segment: " << size << " bytes of fabric memory on device "
              << device;
    return reinterpret_cast<void *>(ptr);
}

// Returns false if ptr is not cuMemCreate memory (then the caller cudaFree()s).
bool free_fabric_vram(void *ptr) {
    CUmemGenericAllocationHandle handle;
    if (cuMemRetainAllocationHandle(&handle, ptr) != CUDA_SUCCESS) return false;
    CUdeviceptr base = 0;
    size_t size = 0;
    if (cuMemGetAddressRange(&base, &size, reinterpret_cast<CUdeviceptr>(ptr)) ==
        CUDA_SUCCESS) {
        cuMemUnmap(base, size);
        cuMemAddressFree(base, size);
    }
    cuMemRelease(handle);  // the retain above
    cuMemRelease(handle);  // the cuMemCreate
    return true;
}
}  // namespace
#endif

#ifdef USE_VRAM_SEGMENT
tl::expected<void *, std::string> allocate_vram_memory(
    size_t total_size, const std::string &protocol) {
    cudaError_t res;
    int device;
    void *ptr = nullptr;
    res = cudaGetDevice(&device);
    if (res != cudaSuccess) {
        LOG(ERROR) << "VRAM Segment cudaGetDevice failed.";
        return tl::make_unexpected("VRAM Segment cudaGetDevice failed.");
    }
    if (protocol == "nvlink_intra") {
#ifdef USE_INTRA_NVLINK
        ptr = allocateFabricMemory_intra(total_size);
        return ptr;
#else
        LOG(ERROR) << "Protocol nvlink_intra need USE_INTRA_NVLINK=ON. Please "
                      "rebuild mooncake from source.";
        return tl::make_unexpected("Protocol not supported");
#endif
    }
#ifdef USE_MNNVL
    if (protocol == "nvlink") {
        ptr = allocate_fabric_vram(total_size, device);
        if (!ptr) return tl::make_unexpected("VRAM Segment fabric alloc failed.");
        return ptr;
    }
#endif
    res = cudaMalloc((void **)&ptr, total_size);
    if (res != cudaSuccess) {
        LOG(ERROR) << "VRAM Segment cudaMalloc failed.";
        return tl::make_unexpected("VRAM Segment cudaMalloc failed.");
    }
    return ptr;
}
#endif

void *allocate_buffer_allocator_memory(size_t total_size,
                                       const std::string &protocol,
                                       size_t alignment, bool use_spdk_dma) {
    const size_t default_alignment = facebook::cachelib::Slab::kSize;
    // Ensure total_size is a multiple of alignment
    if (alignment == default_alignment && total_size < alignment) {
        LOG(ERROR) << "Total size must be at least " << alignment;
        return nullptr;
    }
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    if (protocol == "ascend" || protocol == "ubshmem") {
        return ascend_allocate_memory(total_size, protocol);
    }
#endif
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_allocate_memory(
            total_size, alignment,
            mooncake::globalConfig().sunrise_use_device_mem);
    }
#endif
#if defined(USE_UB)
    if (protocol == "ub") {
        return mooncake::ub_allocate_memory(alignment, total_size);
    }
#endif
#ifdef USE_NOF
    if (use_spdk_dma && total_size > 0) {
        return mooncake::SpdkWrapper::GetInstance().Alloc(total_size, alignment,
                                                          -1);
    }
#endif
#ifdef USE_VRAM_SEGMENT
    auto ret = allocate_vram_memory(total_size, protocol);
    if (!ret) {
        LOG(ERROR) << ret.error();
        return nullptr;
    }
    return *ret;
#endif
    // Allocate aligned memory
    return aligned_alloc(alignment, total_size);
}

void free_memory(const std::string &protocol, void *ptr, bool use_spdk_dma) {
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    if (protocol == "ascend" || protocol == "ubshmem") {
        return ascend_free_memory(protocol, ptr);
    }
#endif
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_free_memory(ptr);
    }
#endif
#if defined(USE_UB)
    if (protocol == "ub") {
        mooncake::ub_free_memory(ptr);
        return;
    }
#endif
#ifdef USE_NOF
    // Mirror allocate_buffer_allocator_memory(): a buffer taken from the SPDK
    // hugepage pool (spdk_zmalloc) must be released with spdk_free, not glibc
    // free(), which would abort with "free(): invalid pointer".
    if (use_spdk_dma) {
        mooncake::SpdkWrapper::GetInstance().Free(ptr);
        return;
    }
#endif
#ifdef USE_VRAM_SEGMENT
#ifdef USE_INTRA_NVLINK
    freeFabricMemory_intra(ptr);
#else
#ifdef USE_MNNVL
    if (free_fabric_vram(ptr)) return;
#endif
    cudaFree(ptr);
#endif
    return;
#endif
    free(ptr);
}

}  // namespace mooncake
