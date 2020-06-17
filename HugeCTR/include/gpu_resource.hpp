/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "HugeCTR/include/common.hpp"
#include "HugeCTR/include/device_map.hpp"
#include "HugeCTR/include/utils.hpp"
#include "ctpl/ctpl_stl.h"

#include <cudnn.h>
#include <curand.h>
#include <nccl.h>

#ifdef ENABLE_MPI
#include <mpi.h>
#endif

namespace HugeCTR {

/**
 * @brief GPU resource allocated on a target gpu.
 *
 * This class implement unified resource managment on the target GPU.
 */
class GPUResource {
 private:
  cudaStream_t stream_;              /**< cuda stream for computation */
  cudaStream_t data_copy_stream_[2]; /**< cuda stream for data copy */
  cublasHandle_t cublas_handle_;
  curandGenerator_t curand_generator_;
  cudnnHandle_t cudnn_handle_;
  const int device_id_;
  const ncclComm_t* comm_;

 public:
  /**
   * Ctor
   */
  GPUResource(int device_id, const ncclComm_t* comm) : device_id_(device_id), comm_(comm) {
    CudaDeviceContext context(device_id_);
    CK_CUBLAS_THROW_(cublasCreate(&cublas_handle_));
    CK_CURAND_THROW_(curandCreateGenerator(&curand_generator_, CURAND_RNG_PSEUDO_DEFAULT));
    CK_CUDNN_THROW_(cudnnCreate(&cudnn_handle_));
    CK_CUDA_THROW_(cudaStreamCreate(&stream_));
    CK_CUDA_THROW_(cudaStreamCreate(&data_copy_stream_[0]));
    CK_CUDA_THROW_(cudaStreamCreate(&data_copy_stream_[1]));
    return;
  }

  GPUResource(const GPUResource&) = delete;
  GPUResource& operator=(const GPUResource&) = delete;

  /*
   * Dtor
   */
  ~GPUResource() {
    try {
      CudaDeviceContext context(device_id_);
      CK_CUBLAS_THROW_(cublasDestroy(cublas_handle_));
      CK_CURAND_THROW_(curandDestroyGenerator(curand_generator_));
      CK_CUDNN_THROW_(cudnnDestroy(cudnn_handle_));
      CK_CUDA_THROW_(cudaStreamDestroy(stream_));
      CK_CUDA_THROW_(cudaStreamDestroy(data_copy_stream_[0]));
      CK_CUDA_THROW_(cudaStreamDestroy(data_copy_stream_[1]));
    } catch (const std::runtime_error& rt_err) {
      std::cerr << rt_err.what() << std::endl;
    }
    return;
  }
  int get_device_id() const { return device_id_; }
  const cudaStream_t& get_stream() const { return stream_; }
  const cudaStream_t& get_data_copy_stream(int id) const { return data_copy_stream_[0]; }
  const cublasHandle_t& get_cublas_handle() const { return cublas_handle_; }
  const curandGenerator_t& get_curand_generator() const { return curand_generator_; }
  const cudnnHandle_t& get_cudnn_handle() const { return cudnn_handle_; }
  const ncclComm_t* get_nccl_ptr() const { return comm_; }
};

/**
 * @brief GPU resources container.
 *
 * A GPU resource container in one node. An instant includes:
 * GPU resource vector, thread pool for training, nccl communicators.
 */
class GPUResourceGroup {
 private:
  std::unique_ptr<ncclComm_t[]> comms_;
  std::shared_ptr<const DeviceMap> device_map_;
  std::vector<std::shared_ptr<const GPUResource>> gpu_resources_; /**< GPU resource vector */

 public:
  ctpl::thread_pool train_thread_pool; /**< cpu thread pool for training */
  std::vector<std::future<void>> results;

  GPUResourceGroup(const std::shared_ptr<const DeviceMap>& device_map)
      : comms_(nullptr),
        device_map_(device_map),
        train_thread_pool(device_map->get_device_list().size()),
        results(device_map->get_device_list().size()) {
    // set threads affinity
    for (unsigned int i = 0; i < device_map->get_device_list().size(); i++) {
      set_affinity(train_thread_pool.get_thread(i), {}, true);
    }

    auto& device_list = device_map->get_device_list();
    size_t local_gpu_count = device_list.size();

    if (local_gpu_count == 0) {
      CK_THROW_(Error_t::WrongInput, "Empty device_list");
    }
    if (local_gpu_count != size()) {
      CK_THROW_(Error_t::WrongInput, "local_gpu_count != size()");
    }
    int dev_count;
    cudaGetDeviceCount(&dev_count);
    for (int dev : device_list) {
      if (dev >= dev_count) {
        CK_THROW_(Error_t::WrongInput, "Invalid device id: " + std::to_string(dev));
      }
    }

    if (device_map->get_device_list().size() != local_gpu_count) {
      CK_THROW_(Error_t::WrongInput, "device_map->get_device_list().size() != local_gpu_count");
    }
    // int total_gpu_count = get_total_gpu_count();
    // if ther are multiple GPUs within a node or/and across nodes
    // if (total_gpu_count > 1) {
    comms_.reset(new ncclComm_t[local_gpu_count]());
#ifdef ENABLE_MPI
    int my_rank = 0;
    int n_ranks = 1;
    CK_MPI_THROW_(MPI_Comm_rank(MPI_COMM_WORLD, &my_rank));
    CK_MPI_THROW_(MPI_Comm_size(MPI_COMM_WORLD, &n_ranks));
    ncclUniqueId nid;
    if (my_rank == 0) CK_NCCL_THROW_(ncclGetUniqueId(&nid));
    CK_MPI_THROW_(MPI_Bcast((void*)&nid, sizeof(nid), MPI_BYTE, 0, MPI_COMM_WORLD));

    CK_NCCL_THROW_(ncclGroupStart());
    for (size_t i = 0; i < local_gpu_count; i++) {
      CK_CUDA_THROW_(cudaSetDevice(device_list[i]));
      CK_NCCL_THROW_(ncclCommInitRank(comms_.get() + i, total_gpu_count, nid,
                                      device_map->get_global_id(device_list[i])));
    }
    CK_NCCL_THROW_(ncclGroupEnd());
#else
    CK_NCCL_THROW_(ncclCommInitAll(comms_.get(), device_list.size(), device_list.data()));
#endif
    //}
    for (size_t i = 0; i < local_gpu_count; i++) {
      gpu_resources_.emplace_back(new GPUResource(device_list[i], comms_.get() + i));
    }
  }

  GPUResourceGroup(const GPUResourceGroup& C) = delete;
  GPUResourceGroup& operator=(const GPUResourceGroup&) = delete;

  const std::shared_ptr<const GPUResource>& operator[](int idx) const {
    return gpu_resources_[idx];
  }
  size_t size() const {
    // return gpu_resources_.size();
    return device_map_->get_device_list().size();
  }
  bool empty() const { return size() == 0; }
  ~GPUResourceGroup() {
    try {
      if (gpu_resources_.size() > 1) {
        for (unsigned int i = 0; i < gpu_resources_.size(); i++) {
          CK_NCCL_THROW_(ncclCommDestroy(comms_[i]));
        }
      }
    } catch (const std::runtime_error& rt_err) {
      std::cerr << rt_err.what() << std::endl;
    }
  }

  const std::vector<int>& get_device_list() const { return device_map_->get_device_list(); }
  int get_global_id(int local_device_id) const {
    return device_map_->get_global_id(local_device_id);
  }
  int get_local_id(int global_id) const {  // sequential GPU indices
    return device_map_->get_local_id(global_id);
  }
  int get_local_device_id(int global_id) const {  // the actual GPU ids
    return device_map_->get_local_device_id(global_id);
  }
  int get_total_gpu_count() const { return device_map_->size(); }
  int get_node_count() const { return device_map_->num_nodes(); }
  int get_pid(int global_id) const { return device_map_->get_pid(global_id); }
};

}  // namespace HugeCTR
