#include "taichi/rhi/cuda/cuda_device.h"

namespace taichi::lang {

namespace cuda {

CudaDevice::AllocInfo CudaDevice::get_alloc_info(
    const DeviceAllocation handle) {
  validate_device_alloc(handle);
  return allocations_[handle.alloc_id];
}

RhiResult CudaDevice::allocate_memory(const AllocParams &params,
                                      DeviceAllocation *out_devalloc) {
  AllocInfo info;

  if (params.host_read || params.host_write) {
    CUDADriver::get_instance().malloc_managed(&info.ptr, params.size,
                                              CU_MEM_ATTACH_GLOBAL);
  } else {
    CUDADriver::get_instance().malloc(&info.ptr, params.size);
  }

  if (info.ptr == nullptr) {
    return RhiResult::out_of_memory;
  }

  info.size = params.size;
  info.is_imported = false;
  info.use_cached = false;
  info.use_preallocated = false;

  *out_devalloc = DeviceAllocation{};
  out_devalloc->alloc_id = allocations_.size();
  out_devalloc->device = this;

  allocations_.push_back(info);
  return RhiResult::success;
}

DeviceAllocation CudaDevice::allocate_memory_runtime(
    const LlvmRuntimeAllocParams &params) {
  AllocInfo info;
  info.size = taichi::iroundup(params.size, taichi_page_size);
  if (params.use_cached) {
    if (caching_allocator_ == nullptr) {
      caching_allocator_ = std::make_unique<CudaCachingAllocator>(this);
    }
    info.ptr = caching_allocator_->allocate(params);
    CUDADriver::get_instance().memset((void *)info.ptr, 0, info.size);
  } else {
    info.ptr = allocate_llvm_runtime_memory_jit(params);
  }
  info.is_imported = false;
  info.use_cached = params.use_cached;
  info.use_preallocated = true;

  DeviceAllocation alloc;
  alloc.alloc_id = allocations_.size();
  alloc.device = this;

  allocations_.push_back(info);
  return alloc;
}

void CudaDevice::dealloc_memory(DeviceAllocation handle) {
  validate_device_alloc(handle);
  AllocInfo &info = allocations_[handle.alloc_id];
  if (info.ptr == nullptr) {
    TI_ERROR("the DeviceAllocation is already deallocated");
  }
  TI_ASSERT(!info.is_imported);
  if (info.use_cached) {
    if (caching_allocator_ == nullptr) {
      TI_ERROR("the CudaCachingAllocator is not initialized");
    }
    caching_allocator_->release(info.size, (uint64_t *)info.ptr);
  } else if (!info.use_preallocated) {
    CUDADriver::get_instance().mem_free(info.ptr);
    info.ptr = nullptr;
  }
}

RhiResult CudaDevice::upload_data(DevicePtr *device_ptr,
                                  const void **data,
                                  size_t *size,
                                  int num_alloc) noexcept {
  if (!device_ptr || !data || !size) {
    return RhiResult::invalid_usage;
  }

  for (int i = 0; i < num_alloc; i++) {
    if (device_ptr[i].device != this || !data[i]) {
      return RhiResult::invalid_usage;
    }

    AllocInfo &info = allocations_[device_ptr[i].alloc_id];
    CUDADriver::get_instance().memcpy_host_to_device(
        (uint8_t *)info.ptr + device_ptr[i].offset, (void *)data[i], size[i]);
  }

  return RhiResult::success;
}

RhiResult CudaDevice::readback_data(
    DevicePtr *device_ptr,
    void **data,
    size_t *size,
    int num_alloc,
    const std::vector<StreamSemaphore> &wait_sema) noexcept {
  if (!device_ptr || !data || !size) {
    return RhiResult::invalid_usage;
  }

  for (int i = 0; i < num_alloc; i++) {
    if (device_ptr[i].device != this || !data[i]) {
      return RhiResult::invalid_usage;
    }

    AllocInfo &info = allocations_[device_ptr[i].alloc_id];
    CUDADriver::get_instance().memcpy_device_to_host(
        data[i], (uint8_t *)info.ptr + device_ptr[i].offset, size[i]);
  }

  return RhiResult::success;
}

RhiResult CudaDevice::map(DeviceAllocation alloc, void **mapped_ptr) {
  AllocInfo &info = allocations_[alloc.alloc_id];
  size_t size = info.size;
  info.mapped = new char[size];
  // FIXME: there should be a better way to do this...
  CUDADriver::get_instance().memcpy_device_to_host(info.mapped, info.ptr, size);
  *mapped_ptr = info.mapped;
  return RhiResult::success;
}

void CudaDevice::unmap(DeviceAllocation alloc) {
  AllocInfo &info = allocations_[alloc.alloc_id];
  CUDADriver::get_instance().memcpy_host_to_device(info.ptr, info.mapped,
                                                   info.size);
  delete[] static_cast<char *>(info.mapped);
  return;
}

void CudaDevice::memcpy_internal(DevicePtr dst, DevicePtr src, uint64_t size) {
  void *dst_ptr =
      static_cast<char *>(allocations_[dst.alloc_id].ptr) + dst.offset;
  void *src_ptr =
      static_cast<char *>(allocations_[src.alloc_id].ptr) + src.offset;
  CUDADriver::get_instance().memcpy_device_to_device(dst_ptr, src_ptr, size);
}

DeviceAllocation CudaDevice::import_memory(void *ptr, size_t size) {
  AllocInfo info;
  info.ptr = ptr;
  info.size = size;
  info.is_imported = true;

  DeviceAllocation alloc;
  alloc.alloc_id = allocations_.size();
  alloc.device = this;

  allocations_.push_back(info);
  return alloc;
}

uint64 CudaDevice::fetch_result_uint64(int i, uint64 *result_buffer) {
  CUDADriver::get_instance().stream_synchronize(nullptr);
  uint64 ret;
  CUDADriver::get_instance().memcpy_device_to_host(&ret, result_buffer + i,
                                                   sizeof(uint64));
  return ret;
}
}  // namespace cuda
}  // namespace taichi::lang
