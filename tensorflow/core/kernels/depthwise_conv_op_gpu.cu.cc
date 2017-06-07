/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if GOOGLE_CUDA
#define EIGEN_USE_GPU

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/kernels/depthwise_conv_op.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/cuda_kernel_helper.h"
#include "tensorflow/core/util/tensor_format.h"

#if !defined(_MSC_VER)
#define UNROLL _Pragma("unroll")
#define NOUNROLL _Pragma("nounroll")
#else
#define UNROLL
#define NOUNROLL
#endif

namespace tensorflow {

using Eigen::GpuDevice;

// A Cuda kernel to compute the depthwise convolution forward pass
// in NHWC format.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(1024, 2)
    DepthwiseConv2dGPUKernelNHWC(const DepthwiseArgs args, const T* input,
                                 const T* filter, T* output, int num_outputs) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  CUDA_1D_KERNEL_LOOP(thread_id, num_outputs) {
    // Compute the indexes of this thread in the output.
    const int OD = thread_id % out_depth;
    const int OC = (thread_id / out_depth) % out_cols;
    const int OR = (thread_id / out_depth / out_cols) % out_rows;
    const int OB = thread_id / out_depth / out_cols / out_rows;
    // Compute the input depth and the index of depth multiplier.
    const int in_d = OD / depth_multiplier;
    const int multiplier = OD % depth_multiplier;

    // Decide if all input is valid, if yes, we can skip the boundary checks
    // for each input.
    const int input_row_start = OR * stride - pad_rows;
    const int input_col_start = OC * stride - pad_cols;
    const int input_row_end = input_row_start + filter_rows;
    const int input_col_end = input_col_start + filter_cols;

    T sum = 0;

    const int input_offset_temp = in_rows * OB;
    if (input_row_start >= 0 && input_col_start >= 0 &&
        input_row_end < in_rows && input_col_end < in_cols) {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = input_row_start + f_r;
        const int filter_offset_temp = filter_cols * f_r;
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = input_col_start + f_c;

          const int input_offset =
              in_d + in_depth * (in_c + in_cols * (in_r + input_offset_temp));
          const int filter_offset =
              multiplier +
              depth_multiplier * (in_d + in_depth * (f_c + filter_offset_temp));
          sum += ldg(input + input_offset) * ldg(filter + filter_offset);
        }
      }
    } else {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = input_row_start + f_r;
        const int filter_offset_temp = filter_cols * f_r;
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = input_col_start + f_c;
          if (in_r >= 0 && in_r < in_rows && in_c >= 0 && in_c < in_cols) {
            const int in_c = input_col_start + f_c;

            const int input_offset =
                in_d + in_depth * (in_c + in_cols * (in_r + input_offset_temp));
            const int filter_offset =
                multiplier + depth_multiplier *
                                 (in_d + in_depth * (f_c + filter_offset_temp));
            sum += ldg(input + input_offset) * ldg(filter + filter_offset);
          }
        }
      }
    }
    output[thread_id] = sum;
  }
}

// CUDA kernel to compute the depthwise convolution forward pass in NCHW format,
// tailored for small images up to 16x16. Stride and depth multiplier must be 1.
// Padding must be 'SAME', which allows to reuse the index computation.
// Tiles of the input and filter tensors are loaded into shared memory before
// performing the convolution. Each thread handles two elements per iteration,
// one each in the lower and upper half of a tile.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          bool kKnownEvenRows>
__global__ __launch_bounds__(1024, 2) void DepthwiseConv2dGPUKernelNHWCSmall(
    const DepthwiseArgs args, const T* input, const T* filter, T* output) {
  // Holds block plus halo and filter data for blockDim.x depths.
  extern __shared__ __align__(sizeof(T)) unsigned char shared_memory[];
  T* const shared_data = reinterpret_cast<T*>(shared_memory);

  const int batches = args.batch;
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;

  // Fixed blockDim.x, corresponding to Pascal's global load granularity of 32B.
  const int block_slices = 8;
  const int block_cols = blockDim.y;
  const int block_rows = blockDim.z;

  // These values are the same for all threads and could
  // be precomputed on the CPU.
  const int block_size = block_rows * block_cols * block_slices;
  const int in_row_size = in_cols * in_depth;
  const int in_size = in_rows * in_row_size;
  const int in_increment = (in_cols - 1) * block_slices;
  const int filter_size = filter_rows * filter_cols;
  const int tile_cols = in_cols + filter_cols - 1;
  const int even_rows = kKnownEvenRows || (1 & ~in_rows);
  const int tile_rows = in_rows + filter_rows - even_rows;
  const int tile_row_size = tile_cols * block_slices;
  const int tile_size = tile_rows * tile_row_size;
  const int tile_offset = block_rows * tile_row_size;
  const int pad_offset = pad_rows * tile_cols + pad_cols;
  const int batch_blocks = (in_depth + block_slices - 1) / block_slices;
  const int in_blocks = batch_blocks * batches;
  const int tensor_offset =
      kKnownEvenRows ? in_size / 2 : block_rows * in_row_size;

  const int thread_depth = threadIdx.x;
  const int thread_col = threadIdx.y;
  const int thread_row = threadIdx.z;

  // Position in block.
  const int thread_pix = thread_row * block_cols + thread_col;
  const int thread_idx = thread_pix * block_slices + thread_depth;

  // Initialize tile, in particular the padding.
  for (int i = thread_idx; i < tile_size; i += block_size) {
    shared_data[i] = T(0);
  }
  __syncthreads();

  // Position in tensors.
  const int tensor_idx = thread_pix * in_depth + thread_depth;

  // Position in (padded) shared memory.
  const int data_pix = thread_row * tile_cols + thread_col;
  const int data_idx = data_pix * block_slices + thread_depth;

  // Position in shared memory, offset by pad_rows / pad_cols.
  const int tile_pix = data_pix + pad_offset;
  const int tile_idx = tile_pix * block_slices + thread_depth;

  const int max_depth = in_depth - thread_depth;
  const int filter_write_offset =
      thread_pix < filter_size ? tile_size + thread_idx : 0;
  const int filter_read_offset = tile_size + thread_depth;
  const bool skip_second =
      !kKnownEvenRows && thread_row + (in_rows & 1) == block_rows;

  for (int b = blockIdx.x; b < in_blocks; b += gridDim.x) {
    const int batch = b / batch_blocks;
    const int stack = b - batch * batch_blocks;

    const int start_depth = stack * block_slices;
    const int filter_offset = tensor_idx + start_depth;
    const int inout_offset = batch * in_size + filter_offset;
    const bool depth_in_range = start_depth < max_depth;

    if (depth_in_range) {
      const T* const in_ptr = inout_offset + input;
      T* const tile_ptr = tile_idx + shared_data;
      tile_ptr[0] = ldg(in_ptr);
      if (!skip_second) {
        tile_ptr[tile_offset] = ldg(tensor_offset + in_ptr);
      }

      if (filter_write_offset != 0) {
        shared_data[filter_write_offset] = ldg(filter_offset + filter);
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();

    if (depth_in_range) {
      T sum1 = 0;
      T sum2 = 0;
      int shared_offset = data_idx;
      const T* filter_ptr = filter_read_offset + shared_data;
      UNROLL for (int r = 0; r < filter_rows; ++r) {
        UNROLL for (int c = 0; c < filter_cols; ++c) {
          const T filter_value = *filter_ptr;
          const T* const tile_ptr = shared_offset + shared_data;
          sum1 += filter_value * tile_ptr[0];
          sum2 += filter_value * tile_ptr[tile_offset];
          shared_offset += block_slices;
          filter_ptr += block_slices;
        }
        shared_offset += in_increment;
      }
      T* const out_ptr = inout_offset + output;
      out_ptr[0] = sum1;
      if (!skip_second) {
        out_ptr[tensor_offset] = sum2;
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();
  }
}

// A Cuda kernel to compute the depthwise convolution forward pass
// in NCHW format.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(1024, 2)
    DepthwiseConv2dGPUKernelNCHW(const DepthwiseArgs args, const T* input,
                                 const T* filter, T* output, int num_outputs) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  CUDA_1D_KERNEL_LOOP(thread_id, num_outputs) {
    // Compute the indexes of this thread in the output.
    //
    // We want coalesced reads so we make sure that each warp reads
    // a contiguous chunk of memory.
    //
    // THIS IS PROBABLY WRONG, we are not doing coalesced reads
    // into the input, because of the depth multiplier division...
    const int OC = thread_id % out_cols;
    const int OR = (thread_id / out_cols) % out_rows;
    const int OD = (thread_id / out_cols / out_rows) % out_depth;
    const int OB = thread_id / out_cols / out_rows / out_depth;

    // Compute the input depth and the index of depth multiplier
    // based off the output depth index that this thread is
    // computing n.
    const int in_d = OD / depth_multiplier;
    const int multiplier = OD % depth_multiplier;

    // Data is stored in the following format (let's assume we
    // flatten the height and width into one contiguous dimension
    // called "P".
    //
    // B1C1P1 B1C1P2 ..... B1C2P1 B1C2P2 ....
    // B2C1P1 B2C1P2 ..... B2C2P1 B2C2P2 ....
    //
    // Each row contains in_depth * in_rows * in_cols values
    // for each sample in the batch.
    //
    // We can further flatten it into:
    //
    // B1C1P1 B1C1P2 .....
    // B1C2P1 B1C2P2 ....
    // B2C1P1 B2C1P2 .....
    // B2C2P1 B2C2P2 ....
    //
    // where each row is a contiguous array of all of the spatial
    // pixels for a given batch and input depth.  The following
    // loop unrolls across the filter dimensions for a given thread,
    // indexing into the filter value and the corresponding input
    // patch.
    //
    // We can compute the index into the patch once right here.
    const int input_offset_temp = (OB * in_depth + in_d) * (in_rows * in_cols);

    // Finally, we can iterate over the spatial dimensions and perform the
    // convolution, writing into the output at the end.
    //
    // We perform an additional optimization, where we can determine
    // whether the patch fits within the image indices statically, and
    // avoid boundary checking within the loop.
    const int input_row_start = OR * stride - pad_rows;
    const int input_col_start = OC * stride - pad_cols;
    const int input_row_end = input_row_start + filter_rows;
    const int input_col_end = input_col_start + filter_cols;

    T sum = 0;
    if (input_row_start >= 0 && input_col_start >= 0 &&
        input_row_end < in_rows && input_col_end < in_cols) {
      // Loop that doesn't need to check for boundary conditions.
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = input_row_start + f_r;
        const int filter_offset_temp = filter_cols * f_r;
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = input_col_start + f_c;

          const int input_offset =
              (input_offset_temp) + (in_r * in_cols) + in_c;
          const int filter_offset =
              multiplier +
              depth_multiplier * (in_d + in_depth * (f_c + filter_offset_temp));
          sum += ldg(input + input_offset) * ldg(filter + filter_offset);
        }
      }
    } else {
      // Loop that needs to check for boundary conditions.
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = input_row_start + f_r;
        const int filter_offset_temp = filter_cols * f_r;
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = input_col_start + f_c;
          // TODO(vrv): the in_r check can be done outside of this loop;
          // benchmark both methods to determine the better decision.
          if (in_r >= 0 && in_r < in_rows && in_c >= 0 && in_c < in_cols) {
            const int in_c = input_col_start + f_c;

            // input_offset_temp indexes into the start of memory
            // where the spatial data starts.
            const int input_offset =
                (input_offset_temp) + (in_r * in_cols) + in_c;

            const int filter_offset =
                multiplier + depth_multiplier *
                                 (in_d + in_depth * (f_c + filter_offset_temp));
            sum += ldg(input + input_offset) * ldg(filter + filter_offset);
          }
        }
      }
    }

    output[thread_id] = sum;
  }
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight>
bool TryLaunchDepthwiseConv2dGPUSmall(const GpuDevice& d,
                                      const DepthwiseArgs args, const T* input,
                                      const T* filter, T* output,
                                      TensorFormat data_format) {
  if (data_format != FORMAT_NHWC || args.depth_multiplier != 1 ||
      args.stride != 1 || args.in_rows > 16 || args.in_cols > 16 ||
      args.in_rows != args.out_rows || args.in_cols != args.out_cols ||
      args.pad_rows < 0 || args.pad_rows >= args.filter_rows ||
      args.pad_cols < 0 || args.pad_cols >= args.filter_cols) {
    return false;
  }

  const int block_rows = (args.in_rows + 1) / 2;
  if (args.filter_rows * args.filter_cols > args.in_cols * block_rows) {
    return false;
  }

  const int tile_cols = args.in_cols + args.filter_cols - 1;
  const int tile_rows = block_rows * 2 + args.filter_rows - 1;
  const int tile_size = tile_rows * tile_cols;
  const int filter_size = args.filter_rows * args.filter_cols;
  dim3 block_dim = dim3(8, args.in_cols, block_rows);
  const int shared_memory_size =
      block_dim.x * (tile_size + filter_size) * sizeof(T);

  const int num_outputs =
      args.batch * args.out_rows * args.out_cols * args.out_depth;
  if (args.in_rows & 1) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_outputs, d,
        DepthwiseConv2dGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                          kKnownFilterHeight, false>,
        shared_memory_size, block_dim.x * block_dim.y * block_dim.z);
    DepthwiseConv2dGPUKernelNHWCSmall<T, kKnownFilterWidth, kKnownFilterHeight,
                                      false>
        <<<config.block_count, block_dim, shared_memory_size, d.stream()>>>(
            args, input, filter, output);
  } else {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_outputs, d,
        DepthwiseConv2dGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                          kKnownFilterHeight, true>,
        shared_memory_size, block_dim.x * block_dim.y * block_dim.z);
    DepthwiseConv2dGPUKernelNHWCSmall<T, kKnownFilterWidth, kKnownFilterHeight,
                                      true>
        <<<config.block_count, block_dim, shared_memory_size, d.stream()>>>(
            args, input, filter, output);
  }
  return true;
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
void LaunchDepthwiseConv2dGPU(const GpuDevice& d, const DepthwiseArgs args,
                              const T* input, const T* filter, T* output,
                              TensorFormat data_format) {
  if (TryLaunchDepthwiseConv2dGPUSmall<T, kKnownFilterWidth,
                                       kKnownFilterHeight>(
          d, args, input, filter, output, data_format)) {
    return;
  }
  const int num_outputs =
      args.batch * args.out_rows * args.out_cols * args.out_depth;
  // The compile-time constant version runs faster with a single block.
  const int max_block_count = kKnownFilterWidth < 0 || kKnownFilterHeight < 0 ||
                                      kKnownDepthMultiplier < 0
                                  ? std::numeric_limits<int>::max()
                                  : d.getNumCudaMultiProcessors();
  if (data_format == FORMAT_NHWC) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_outputs, d,
        DepthwiseConv2dGPUKernelNHWC<T, kKnownFilterWidth, kKnownFilterHeight,
                                     kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dGPUKernelNHWC<T, kKnownFilterWidth, kKnownFilterHeight,
                                 kKnownDepthMultiplier>
        <<<std::min(max_block_count, config.block_count),
           config.thread_per_block, 0, d.stream()>>>(args, input, filter,
                                                     output, num_outputs);
  } else if (data_format == FORMAT_NCHW) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_outputs, d,
        DepthwiseConv2dGPUKernelNCHW<T, kKnownFilterWidth, kKnownFilterHeight,
                                     kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dGPUKernelNCHW<T, kKnownFilterWidth, kKnownFilterHeight,
                                 kKnownDepthMultiplier>
        <<<std::min(max_block_count, config.block_count),
           config.thread_per_block, 0, d.stream()>>>(args, input, filter,
                                                     output, num_outputs);
  } else {
    assert(false);
  }
}

// A simple launch pad to launch the Cuda kernel for depthwise convolution.
template <typename T>
struct DepthwiseConv2dGPULaunch {
  static void Run(const GpuDevice& d, const DepthwiseArgs args, const T* input,
                  const T* filter, T* output, TensorFormat data_format) {
    if (args.filter_rows == 3 && args.filter_cols == 3 &&
        args.depth_multiplier == 1) {
      LaunchDepthwiseConv2dGPU<T, 3, 3, 1>(d, args, input, filter, output,
                                           data_format);
    } else {
      LaunchDepthwiseConv2dGPU<T, -1, -1, -1>(d, args, input, filter, output,
                                              data_format);
    }
  }
};

template struct DepthwiseConv2dGPULaunch<float>;
template struct DepthwiseConv2dGPULaunch<double>;

// A Cuda kernel to compute the depthwise convolution backprop w.r.t. input.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(640, 2)
    DepthwiseConv2dBackpropInputGPUKernelNHWC(const DepthwiseArgs args,
                                              const T* out_backprop,
                                              const T* filter, T* in_backprop,
                                              int num_in_backprop) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  CUDA_1D_KERNEL_LOOP(thread_id, num_in_backprop) {
    // Compute the indexes of this thread in the output.
    const int in_d = thread_id % in_depth;
    const int in_c = (thread_id / in_depth) % in_cols;
    const int in_r = (thread_id / in_depth / in_cols) % in_rows;
    const int b = thread_id / in_depth / in_cols / in_rows;

    T sum = 0;

    const int out_r_start =
        tf_max<int>(0, (in_r - filter_rows + pad_rows + stride) / stride);
    const int out_r_end = tf_min(out_rows - 1, (in_r + pad_rows) / stride);
    const int out_c_start =
        tf_max(0, (in_c - filter_cols + pad_cols + stride) / stride);
    const int out_c_end = tf_min(out_cols - 1, (in_c + pad_cols) / stride);

    NOUNROLL for (int out_r = out_r_start; out_r <= out_r_end; ++out_r) {
      const int f_r = in_r + pad_rows - out_r * stride;
      const int temp_out_backprop_offset =
          out_depth * out_cols * (out_r + out_rows * b);
      const int temp_filter_offset = filter_cols * f_r;
      NOUNROLL for (int out_c = out_c_start; out_c <= out_c_end; ++out_c) {
        const int f_c = in_c + pad_cols - out_c * stride;
        int filter_offset =
            depth_multiplier * (in_d + in_depth * (f_c + temp_filter_offset));
        const int out_backprop_offset =
            out_depth * out_c + temp_out_backprop_offset;
#pragma unroll 6
        for (int i = 0; i < depth_multiplier; ++i) {
          sum += ldg(out_backprop + out_backprop_offset +
                     in_d * depth_multiplier + i) *
                 ldg(filter + filter_offset + i);
        }
      }
    }
    const int in_backprop_offset =
        in_d + in_depth * (in_c + in_cols * (in_r + in_rows * b));
    in_backprop[in_backprop_offset] = sum;
  }
}

// CUDA kernel to compute the depthwise convolution backward w.r.t. input in
// NCHW format, tailored for small images up to 16x16. Stride and depth
// multiplier must be 1. Padding must be 'SAME', which allows to reuse the index
// computation.
// Implementation is the same as the forward pass, except that the filter is
// rotate by 180°, see filter_read_offset and filter_ptr.
// Tiles of the input and filter tensors are loaded into shared memory before
// performing the convolution. Each thread handles two elements per iteration,
// one each in the lower and upper half of a tile.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          bool kKnownEvenRows>
__global__
__launch_bounds__(1024, 2) void DepthwiseConv2dBackpropInputGPUKernelNHWCSmall(
    const DepthwiseArgs args, const T* input, const T* filter, T* output) {
  // Holds block plus halo and filter data for blockDim.x depths.
  extern __shared__ __align__(sizeof(T)) unsigned char shared_memory[];
  T* const shared_data = reinterpret_cast<T*>(shared_memory);

  const int batches = args.batch;
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;

  // Fixed blockDim.x, corresponding to Pascal's global load granularity of 32B.
  const int block_slices = 8;
  const int block_cols = blockDim.y;
  const int block_rows = blockDim.z;

  // These values are the same for all threads and could
  // be precomputed on the CPU.
  const int block_size = block_rows * block_cols * block_slices;
  const int in_row_size = in_cols * in_depth;
  const int in_size = in_rows * in_row_size;
  const int in_increment = (in_cols - 1) * block_slices;
  const int filter_size = filter_rows * filter_cols;
  const int tile_cols = in_cols + filter_cols - 1;
  const int even_rows = kKnownEvenRows || (1 & ~in_rows);
  const int tile_rows = in_rows + filter_rows - even_rows;
  const int tile_row_size = tile_cols * block_slices;
  const int tile_size = tile_rows * tile_row_size;
  const int tile_offset = block_rows * tile_row_size;
  const int pad_offset = pad_rows * tile_cols + pad_cols;
  const int batch_blocks = (in_depth + block_slices - 1) / block_slices;
  const int in_blocks = batch_blocks * batches;
  const int tensor_offset =
      kKnownEvenRows ? in_size / 2 : block_rows * in_row_size;

  const int thread_depth = threadIdx.x;
  const int thread_col = threadIdx.y;
  const int thread_row = threadIdx.z;

  // Position in block.
  const int thread_pix = thread_row * block_cols + thread_col;
  const int thread_idx = thread_pix * block_slices + thread_depth;

  // Initialize tile, in particular the padding.
  for (int i = thread_idx; i < tile_size; i += block_size) {
    shared_data[i] = T(0);
  }
  __syncthreads();

  // Position in tensors.
  const int tensor_idx = thread_pix * in_depth + thread_depth;

  // Position in (padded) shared memory.
  const int data_pix = thread_row * tile_cols + thread_col;
  const int data_idx = data_pix * block_slices + thread_depth;

  // Position in shared memory, offset by pad_rows / pad_cols.
  const int tile_pix = data_pix + pad_offset;
  const int tile_idx = tile_pix * block_slices + thread_depth;

  const int max_depth = in_depth - thread_depth;
  const int filter_write_offset =
      thread_pix < filter_size ? tile_size + thread_idx : 0;
  const int filter_read_offset =
      tile_size + filter_size * block_slices + thread_depth;
  const bool skip_second =
      !kKnownEvenRows && thread_row + (in_rows & 1) == block_rows;

  for (int b = blockIdx.x; b < in_blocks; b += gridDim.x) {
    const int batch = b / batch_blocks;
    const int stack = b - batch * batch_blocks;

    const int start_depth = stack * block_slices;
    const int filter_offset = tensor_idx + start_depth;
    const int inout_offset = batch * in_size + filter_offset;
    const bool depth_in_range = start_depth < max_depth;

    if (depth_in_range) {
      const T* const in_ptr = inout_offset + input;
      T* const tile_ptr = tile_idx + shared_data;
      tile_ptr[0] = ldg(in_ptr);
      if (!skip_second) {
        tile_ptr[tile_offset] = ldg(tensor_offset + in_ptr);
      }

      if (filter_write_offset != 0) {
        shared_data[filter_write_offset] = ldg(filter_offset + filter);
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();

    if (depth_in_range) {
      T sum1 = 0;
      T sum2 = 0;
      int shared_offset = data_idx;
      const T* filter_ptr = filter_read_offset + shared_data;
      UNROLL for (int r = 0; r < filter_rows; ++r) {
        UNROLL for (int c = 0; c < filter_cols; ++c) {
          filter_ptr -= block_slices;
          const T filter_value = *filter_ptr;
          const T* const tile_ptr = shared_offset + shared_data;
          sum1 += filter_value * tile_ptr[0];
          sum2 += filter_value * tile_ptr[tile_offset];
          shared_offset += block_slices;
        }
        shared_offset += in_increment;
      }
      T* const out_ptr = inout_offset + output;
      out_ptr[0] = sum1;
      if (!skip_second) {
        out_ptr[tensor_offset] = sum2;
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();
  }
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(640, 2)
    DepthwiseConv2dBackpropInputGPUKernelNCHW(const DepthwiseArgs args,
                                              const T* out_backprop,
                                              const T* filter, T* in_backprop,
                                              int num_in_backprop) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  // TODO(vrv): Consider assigning threads to output and using
  // atomics for accumulation, similar to the filter case.
  CUDA_1D_KERNEL_LOOP(thread_id, num_in_backprop) {
    // Compute the indexes of this thread in the input.
    const int in_c = thread_id % in_cols;
    const int in_r = (thread_id / in_cols) % in_rows;
    const int in_d = (thread_id / in_cols / in_rows) % in_depth;
    const int b = thread_id / in_depth / in_cols / in_rows;

    T sum = 0;
    const int out_d_start = in_d * depth_multiplier;
    const int out_d_end = out_d_start + depth_multiplier;

    const int out_r_start =
        tf_max<int>(0, (in_r - filter_rows + pad_rows + stride) / stride);
    const int out_r_end = tf_min(out_rows - 1, (in_r + pad_rows) / stride);
    const int out_c_start =
        tf_max(0, (in_c - filter_cols + pad_cols + stride) / stride);
    const int out_c_end = tf_min(out_cols - 1, (in_c + pad_cols) / stride);

    UNROLL for (int out_d = out_d_start; out_d < out_d_end; ++out_d) {
      UNROLL for (int out_r = out_r_start; out_r <= out_r_end; ++out_r) {
        const int f_r = in_r + pad_rows - out_r * stride;
        const int filter_dm = out_d - out_d_start;

        const int temp_filter_offset = filter_cols * f_r;
        for (int out_c = out_c_start; out_c <= out_c_end; ++out_c) {
          const int f_c = in_c + pad_cols - out_c * stride;
          const int filter_offset =
              filter_dm + args.depth_multiplier *
                              (in_d + in_depth * (f_c + temp_filter_offset));

          const int out_backprop_offset =
              (b * out_depth * out_rows * out_cols) +
              (out_d * out_rows * out_cols) + (out_r * out_cols) + (out_c);

          sum += ldg(out_backprop + out_backprop_offset) *
                 ldg(filter + filter_offset);
        }
      }
    }
    const int in_backprop_offset = (b * in_rows * in_cols * in_depth) +
                                   (in_d * in_rows * in_cols) +
                                   (in_r * in_cols) + (in_c);
    in_backprop[in_backprop_offset] = sum;
  }
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight>
bool TryLaunchDepthwiseConv2dBackpropInputGPUSmall(
    const GpuDevice& d, const DepthwiseArgs args, const T* out_backprop,
    const T* filter, T* in_backprop, TensorFormat data_format) {
  if (data_format != FORMAT_NHWC || args.depth_multiplier != 1 ||
      args.stride != 1 || args.in_rows > 16 || args.in_cols > 16 ||
      args.in_rows != args.out_rows || args.in_cols != args.out_cols ||
      args.pad_rows < 0 || args.pad_rows >= args.filter_rows ||
      args.pad_cols < 0 || args.pad_cols >= args.filter_cols) {
    return false;
  }

  const int block_rows = (args.in_rows + 1) / 2;
  if (args.filter_rows * args.filter_cols > args.in_cols * block_rows) {
    return false;
  }

  const int tile_cols = args.in_cols + args.filter_cols - 1;
  const int tile_rows = block_rows * 2 + args.filter_rows - 1;
  const int tile_size = tile_rows * tile_cols;
  const int filter_size = args.filter_rows * args.filter_cols;
  dim3 block_dim = dim3(8, args.in_cols, block_rows);
  const int shared_memory_size =
      block_dim.x * (tile_size + filter_size) * sizeof(T);

  const int num_in_backprop =
      args.batch * args.in_rows * args.in_cols * args.in_depth;
  if (args.in_rows & 1) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_in_backprop, d,
        DepthwiseConv2dBackpropInputGPUKernelNHWCSmall<
            T, kKnownFilterWidth, kKnownFilterHeight, false>,
        shared_memory_size, block_dim.x * block_dim.y * block_dim.z);
    DepthwiseConv2dBackpropInputGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                                   kKnownFilterHeight, false>
        <<<config.block_count, block_dim, shared_memory_size, d.stream()>>>(
            args, out_backprop, filter, in_backprop);
  } else {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_in_backprop, d,
        DepthwiseConv2dBackpropInputGPUKernelNHWCSmall<
            T, kKnownFilterWidth, kKnownFilterHeight, true>,
        shared_memory_size, block_dim.x * block_dim.y * block_dim.z);
    DepthwiseConv2dBackpropInputGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                                   kKnownFilterHeight, true>
        <<<config.block_count, block_dim, shared_memory_size, d.stream()>>>(
            args, out_backprop, filter, in_backprop);
  }

  return true;
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
void LaunchDepthwiseConv2dBackpropInputGPU(const GpuDevice& d,
                                           const DepthwiseArgs args,
                                           const T* out_backprop,
                                           const T* filter, T* in_backprop,
                                           TensorFormat data_format) {
  if (TryLaunchDepthwiseConv2dBackpropInputGPUSmall<T, kKnownFilterWidth,
                                                    kKnownFilterHeight>(
          d, args, out_backprop, filter, in_backprop, data_format)) {
    return;
  }
  const int num_in_backprop =
      args.batch * args.in_rows * args.in_cols * args.in_depth;
  if (data_format == FORMAT_NHWC) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_in_backprop, d,
        DepthwiseConv2dBackpropInputGPUKernelNHWC<
            T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dBackpropInputGPUKernelNHWC<
        T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>
        <<<config.block_count, config.thread_per_block, 0, d.stream()>>>(
            args, out_backprop, filter, in_backprop, num_in_backprop);
  } else if (data_format == FORMAT_NCHW) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_in_backprop, d,
        DepthwiseConv2dBackpropInputGPUKernelNCHW<
            T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dBackpropInputGPUKernelNCHW<
        T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>
        <<<config.block_count, config.thread_per_block, 0, d.stream()>>>(
            args, out_backprop, filter, in_backprop, num_in_backprop);
  } else {
    assert(false);
  }
}

// A simple launch pad to launch the Cuda kernel for depthwise convolution.
template <typename T>
struct DepthwiseConv2dBackpropInputGPULaunch {
  static void Run(const GpuDevice& d, const DepthwiseArgs args,
                  const T* out_backprop, const T* filter, T* in_backprop,
                  TensorFormat data_format) {
    if (args.depth_multiplier == 1) {
      if (args.filter_rows == 3 && args.filter_cols == 3) {
        LaunchDepthwiseConv2dBackpropInputGPU<T, 3, 3, 1>(
            d, args, out_backprop, filter, in_backprop, data_format);
      } else {
        LaunchDepthwiseConv2dBackpropInputGPU<T, -1, -1, 1>(
            d, args, out_backprop, filter, in_backprop, data_format);
      }
    } else {
      LaunchDepthwiseConv2dBackpropInputGPU<T, -1, -1, -1>(
          d, args, out_backprop, filter, in_backprop, data_format);
    }
  }
};

template struct DepthwiseConv2dBackpropInputGPULaunch<float>;
template struct DepthwiseConv2dBackpropInputGPULaunch<double>;

// A Cuda kernel to compute the depthwise convolution backprop w.r.t. filter.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(640, 2)
    DepthwiseConv2dBackpropFilterGPUKernelNHWC(const DepthwiseArgs args,
                                               const T* out_backprop,
                                               const T* input,
                                               T* filter_backprop,
                                               int num_out_backprop) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  CUDA_1D_KERNEL_LOOP(thread_id, num_out_backprop) {
    // Compute the indexes of this thread in the output.
    const int out_d = thread_id % out_depth;
    const int out_c = (thread_id / out_depth) % out_cols;
    const int out_r = (thread_id / out_depth / out_cols) % out_rows;
    const int b = thread_id / out_depth / out_cols / out_rows;
    // Compute the input depth and the index of depth multiplier.
    const int in_d = out_d / depth_multiplier;
    const int dm = out_d % depth_multiplier;

    // Decide if all input is valid, if yes, we can skip the boundary checks
    // for each input.
    const int in_r_start = out_r * stride - pad_rows;
    const int in_c_start = out_c * stride - pad_cols;
    const int in_r_end = in_r_start + filter_rows;
    const int in_c_end = in_c_start + filter_cols;

    const int out_backprop_offset =
        out_d + out_depth * (out_c + out_cols * (out_r + out_rows * b));
    const T out_bp = ldg(out_backprop + out_backprop_offset);
    if (in_r_start >= 0 && in_c_start >= 0 && in_r_end < in_rows &&
        in_c_end < in_cols) {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = in_r_start + f_r;
        // Avoid repeated computation.
        const int input_offset_temp = in_cols * (in_r + in_rows * b);
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = in_c_start + f_c;

          const int input_offset = in_d + in_depth * (in_c + input_offset_temp);
          T partial_sum = ldg(input + input_offset) * out_bp;
          T* addr = filter_backprop +
                    (dm + depth_multiplier *
                              (in_d + in_depth * (f_c + filter_cols * f_r)));
          CudaAtomicAdd(addr, partial_sum);
        }
      }
    } else {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = in_r_start + f_r;
        // Avoid repeated computation.
        const int input_offset_temp = in_cols * (in_r + in_rows * b);
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = in_c_start + f_c;
          const int addr_temp = filter_cols * f_r;

          if (in_r >= 0 && in_r < in_rows && in_c >= 0 && in_c < in_cols) {
            const int input_offset =
                in_d + in_depth * (in_c + input_offset_temp);
            T partial_sum = ldg(input + input_offset) * out_bp;
            T* addr =
                filter_backprop +
                (dm + depth_multiplier * (in_d + in_depth * (f_c + addr_temp)));
            // Potentially many threads can add to the same address so we have
            // to use atomic add here.
            // TODO(jmchen): If atomic add turns out to be slow, we can:
            // 1. allocate multiple buffers for the gradients (one for each
            // example in a batch, for example). This can reduce the
            // contention on the destination; 2. Have each thread compute one
            // gradient for an element in the filters. This should work well
            // when the input depth is big and filter size is not too small.
            CudaAtomicAdd(addr, partial_sum);
          }
        }
      }
    }
  }
}

// CUDA kernel to compute the depthwise convolution backward w.r.t. filter in
// NCHW format, tailored for small images up to 16x16. Stride and depth
// multiplier must be 1. Padding must be 'SAME'.
// Tiles of the input tensor are loaded into shared memory before performing the
// convolution. Per iteration and filter element, each thread first performs
// a partial convolution for two elements, one each in the lower and upper half
// of a tile. The intermediate result of 4 consecutive columns are then
// accumulated and written to shared memory. Finally, the values in shared
// memory are warp-accumulated (in chunks of 32 elements) and summed up in
// global memory using atomics.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight>
__global__
__launch_bounds__(1024, 2) void DepthwiseConv2dBackpropFilterGPUKernelNHWCSmall(
    const DepthwiseArgs args, const T* output, const T* input, T* filter) {
  // Holds block plus halo and filter data for blockDim.x depths.
  extern __shared__ __align__(sizeof(T)) unsigned char shared_memory[];
  T* const shared_data = reinterpret_cast<T*>(shared_memory);

  const int batches = args.batch;
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;

  // Fixed blockDim.x, corresponding to Pascal's global load granularity of 32B.
  const int block_slices = 8;
  const int block_cols = blockDim.y;
  const int block_rows = blockDim.z;

  // These values are the same for all threads and could
  // be precomputed on the CPU.
  const int block_size = block_rows * block_cols * block_slices;
  const int in_row_size = in_cols * in_depth;
  const int in_size = in_rows * in_row_size;
  const int in_increment = (in_cols - 1) * block_slices;
  const int filter_size = filter_rows * filter_cols;
  const int tile_cols = in_cols + filter_cols - 1;
  const int tile_rows = 2 * block_rows + filter_rows - 1;
  const int tile_row_size = tile_cols * block_slices;
  const int tile_size = tile_rows * tile_row_size;
  const int tile_offset = block_rows * tile_row_size;
  const int pad_offset = pad_rows * tile_cols + pad_cols;
  const int batch_blocks = (in_depth + block_slices - 1) / block_slices;
  const int in_blocks = batch_blocks * batches;
  const int tensor_offset = block_rows * in_row_size;
  const int accum_pixels = 32;
  const int accum_increment = accum_pixels * block_slices;
  const int accum_size = filter_size * accum_increment;

  const int thread_depth = threadIdx.x;
  const int thread_col = threadIdx.y;
  const int thread_row = threadIdx.z;

  // Position in block.
  const int thread_pix = thread_row * block_cols + thread_col;
  const int thread_idx = thread_pix * block_slices + thread_depth;

  // Initialize tile, in particular the padding and accumulator.
  for (int i = thread_idx; i < tile_size + accum_size; i += block_size) {
    shared_data[i] = T(0);
  }
  __syncthreads();

  // Position in tensors.
  const int tensor_idx = thread_pix * in_depth + thread_depth;

  // Position in (padded) shared memory.
  const int data_pix = thread_row * tile_cols + thread_col;
  const int data_idx = data_pix * block_slices + thread_depth;

  // Position in shared memory, offset by pad_rows / pad_cols.
  const int tile_pix = data_pix + pad_offset;
  const int tile_idx = tile_pix * block_slices + thread_depth;

  // Position in accumulator (1 per 4 threads, depth major).
  const int accum_pix = thread_pix / 4;
  const int accum_idx = thread_depth * accum_pixels + accum_pix;

  const int max_depth = in_depth - thread_depth;
  const int accum_offset = tile_size + accum_idx;
  const bool skip_second = block_rows + thread_row >= in_rows;

  for (int b = blockIdx.x; b < in_blocks; b += gridDim.x) {
    const int batch = b / batch_blocks;
    const int stack = b - batch * batch_blocks;

    const int start_depth = stack * block_slices;
    const int filter_offset = tensor_idx + start_depth;
    const int inout_offset = batch * in_size + filter_offset;
    const bool depth_in_range = start_depth < max_depth;

    if (depth_in_range) {
      const T* const in_ptr = inout_offset + input;
      T* const tile_ptr = tile_idx + shared_data;
      tile_ptr[0] = ldg(in_ptr);
      if (!skip_second) {
        tile_ptr[tile_offset] = ldg(tensor_offset + in_ptr);
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();

    if (depth_in_range) {
      const T* const out_ptr = inout_offset + output;
      const T out1 = ldg(out_ptr);
      const T out2 = skip_second ? T(0) : ldg(tensor_offset + out_ptr);
      int shared_offset = data_idx;
      T* accum_ptr = accum_offset + shared_data;
      UNROLL for (int r = 0; r < filter_rows; ++r) {
        UNROLL for (int c = 0; c < filter_cols; ++c) {
          const T* const tile_ptr = shared_offset + shared_data;
          T val = out1 * tile_ptr[0] + out2 * tile_ptr[tile_offset];
          val += CudaShuffleDown(val, 16);
          val += CudaShuffleDown(val, 8);
          if (!(thread_idx & 24) /* i.e. 'lane_idx < 8' */) {
            *accum_ptr = val;
          }
          shared_offset += block_slices;
          accum_ptr += accum_increment;
        }
        shared_offset += in_increment;
      }
    }

    // Note: the condition to reach this is uniform across the entire block.
    __syncthreads();

    const T* const accum_data = tile_size + shared_data;
    for (int i = thread_idx; i < accum_size; i += block_size) {
      const int filter_idx = i / 32;
      const int filter_pix = filter_idx / block_slices;
      const int filter_depth = filter_idx % block_slices + start_depth;
      const int filter_offset = filter_pix * in_depth + filter_depth;
      if (filter_depth < in_depth) {
        T val = accum_data[i];
        val += CudaShuffleDown(val, 16);
        val += CudaShuffleDown(val, 8);
        val += CudaShuffleDown(val, 4);
        val += CudaShuffleDown(val, 2);
        val += CudaShuffleDown(val, 1);
        if (!(thread_idx & 31) /* i.e. 'lane_idx == 0' */) {
          CudaAtomicAdd(filter_offset + filter, val);
        }
      }
    }
  }
}

// A Cuda kernel to compute the depthwise convolution backprop w.r.t. filter.
template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
__global__ void __launch_bounds__(640, 2)
    DepthwiseConv2dBackpropFilterGPUKernelNCHW(const DepthwiseArgs args,
                                               const T* out_backprop,
                                               const T* input,
                                               T* filter_backprop,
                                               int num_out_backprop) {
  const int in_rows = args.in_rows;
  const int in_cols = args.in_cols;
  const int in_depth = args.in_depth;
  const int filter_rows =
      kKnownFilterHeight < 0 ? args.filter_rows : kKnownFilterHeight;
  const int filter_cols =
      kKnownFilterWidth < 0 ? args.filter_cols : kKnownFilterWidth;
  const int depth_multiplier =
      kKnownDepthMultiplier < 0 ? args.depth_multiplier : kKnownDepthMultiplier;
  const int stride = args.stride;
  const int pad_rows = args.pad_rows;
  const int pad_cols = args.pad_cols;
  const int out_rows = args.out_rows;
  const int out_cols = args.out_cols;
  const int out_depth = args.out_depth;

  CUDA_1D_KERNEL_LOOP(thread_id, num_out_backprop) {
    // Compute the indexes of this thread in the output.
    const int out_c = thread_id % out_cols;
    const int out_r = (thread_id / out_cols) % out_rows;
    const int out_d = (thread_id / out_cols / out_rows) % out_depth;

    const int b = thread_id / out_depth / out_cols / out_rows;
    // Compute the input depth and the index of depth multiplier.
    const int in_d = out_d / depth_multiplier;
    const int dm = out_d % depth_multiplier;

    // Decide if all input is valid, if yes, we can skip the boundary checks
    // for each input.
    const int in_r_start = out_r * stride - pad_rows;
    const int in_c_start = out_c * stride - pad_cols;
    const int in_r_end = in_r_start + filter_rows;
    const int in_c_end = in_c_start + filter_cols;

    const int out_backprop_offset = (b * out_depth * out_rows * out_cols) +
                                    (out_d * out_rows * out_cols) +
                                    (out_r * out_cols) + (out_c);

    const T out_bp = ldg(out_backprop + out_backprop_offset);
    if (in_r_start >= 0 && in_c_start >= 0 && in_r_end < in_rows &&
        in_c_end < in_cols) {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = in_r_start + f_r;
        // Avoid repeated computation.
        const int input_offset_temp = (b * in_depth * in_rows * in_cols) +
                                      (in_d * in_rows * in_cols) +
                                      (in_r * in_cols);

        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = in_c_start + f_c;
          const int input_offset = input_offset_temp + in_c;
          T partial_sum = ldg(input + input_offset) * out_bp;
          T* addr = filter_backprop +
                    (dm + depth_multiplier *
                              (in_d + in_depth * (f_c + filter_cols * f_r)));
          CudaAtomicAdd(addr, partial_sum);
        }
      }
    } else {
      UNROLL for (int f_r = 0; f_r < filter_rows; ++f_r) {
        const int in_r = in_r_start + f_r;
        // Avoid repeated computation.
        const int input_offset_temp = (b * in_depth * in_rows * in_cols) +
                                      (in_d * in_rows * in_cols) +
                                      (in_r * in_cols);
        UNROLL for (int f_c = 0; f_c < filter_cols; ++f_c) {
          const int in_c = in_c_start + f_c;
          const int addr_temp = filter_cols * f_r;

          if (in_r >= 0 && in_r < in_rows && in_c >= 0 && in_c < in_cols) {
            const int input_offset = input_offset_temp + in_c;
            T partial_sum = ldg(input + input_offset) * out_bp;
            T* addr =
                filter_backprop +
                (dm + depth_multiplier * (in_d + in_depth * (f_c + addr_temp)));
            // Potentially many threads can add to the same address so we have
            // to use atomic add here.
            // TODO(jmchen): If atomic add turns out to be slow, we can:
            // 1. allocate multiple buffers for the gradients (one for each
            // example in a batch, for example). This can reduce the
            // contention on the destination; 2. Have each thread compute one
            // gradient for an element in the filters. This should work well
            // when the input depth is big and filter size is not too small.
            CudaAtomicAdd(addr, partial_sum);
          }
        }
      }
    }
  }
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight>
bool TryLaunchDepthwiseConv2dBackpropFilterGPUSmall(
    const GpuDevice& d, const DepthwiseArgs args, const T* out_backprop,
    const T* input, T* filter_backprop, TensorFormat data_format) {
  if (data_format != FORMAT_NHWC || args.depth_multiplier != 1 ||
      args.stride != 1 || args.in_rows > 16 || args.in_cols > 16 ||
      args.in_rows != args.out_rows || args.in_cols != args.out_cols ||
      args.pad_rows < 0 || args.pad_rows >= args.filter_rows ||
      args.pad_cols < 0 || args.pad_cols >= args.filter_cols) {
    return false;
  }

  const int lookup_table[] = {0, 3, 1, 3};
  const int rows_mask = lookup_table[args.in_cols & 3];
  const int block_rows = (args.in_rows + 1) / 2 + rows_mask & ~rows_mask;
  const int tile_cols = args.in_cols + args.filter_cols - 1;
  const int tile_rows = block_rows * 2 + args.filter_rows - 1;
  const int tile_size = tile_rows * tile_cols;
  const int accum_size = args.filter_rows * args.filter_cols * 32;
  dim3 block_dim = dim3(8, args.in_cols, block_rows);
  const int shared_memory_size =
      block_dim.x * (tile_size + accum_size) * sizeof(T);

  if (block_rows > args.in_rows ||
      args.filter_rows * args.filter_cols > args.in_cols * block_rows ||
      shared_memory_size > d.sharedMemPerBlock()) {
    return false;
  }

  const int num_out_backprop =
      args.batch * args.out_rows * args.out_cols * args.out_depth;
  CudaLaunchConfig config = GetCudaLaunchConfig(
      num_out_backprop, d,
      DepthwiseConv2dBackpropFilterGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                                      kKnownFilterHeight>,
      shared_memory_size, block_dim.x * block_dim.y * block_dim.z);
  DepthwiseConv2dBackpropFilterGPUKernelNHWCSmall<T, kKnownFilterWidth,
                                                  kKnownFilterHeight>
      <<<config.block_count, block_dim, shared_memory_size, d.stream()>>>(
          args, out_backprop, input, filter_backprop);
  return true;
}

template <typename T, int kKnownFilterWidth, int kKnownFilterHeight,
          int kKnownDepthMultiplier>
void LaunchDepthwiseConv2dBackpropFilterGPU(const GpuDevice& d,
                                            const DepthwiseArgs args,
                                            const T* out_backprop,
                                            const T* input, T* filter_backprop,
                                            TensorFormat data_format) {
  if (TryLaunchDepthwiseConv2dBackpropFilterGPUSmall<T, kKnownFilterWidth,
                                                     kKnownFilterHeight>(
          d, args, out_backprop, input, filter_backprop, data_format)) {
    return;
  }
  const int num_out_backprop =
      args.batch * args.out_rows * args.out_cols * args.out_depth;
  if (data_format == FORMAT_NHWC) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_out_backprop, d,
        DepthwiseConv2dBackpropFilterGPUKernelNHWC<
            T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dBackpropFilterGPUKernelNHWC<
        T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>
        <<<config.block_count, config.thread_per_block, 0, d.stream()>>>(
            args, out_backprop, input, filter_backprop, num_out_backprop);
  } else if (data_format == FORMAT_NCHW) {
    CudaLaunchConfig config = GetCudaLaunchConfig(
        num_out_backprop, d,
        DepthwiseConv2dBackpropFilterGPUKernelNCHW<
            T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>,
        0, 0);
    DepthwiseConv2dBackpropFilterGPUKernelNCHW<
        T, kKnownFilterWidth, kKnownFilterHeight, kKnownDepthMultiplier>
        <<<config.block_count, config.thread_per_block, 0, d.stream()>>>(
            args, out_backprop, input, filter_backprop, num_out_backprop);
  } else {
    assert(false);
  }
}

// A simple launch pad to launch the Cuda kernel for depthwise convolution.
template <typename T>
struct DepthwiseConv2dBackpropFilterGPULaunch {
  static void Run(const GpuDevice& d, const DepthwiseArgs args,
                  const T* out_backprop, const T* input, T* filter_backprop,
                  TensorFormat data_format) {
    if (args.filter_rows == 3 && args.filter_cols == 3 &&
        args.depth_multiplier == 1) {
      LaunchDepthwiseConv2dBackpropFilterGPU<T, 3, 3, 1>(
          d, args, out_backprop, input, filter_backprop, data_format);
    } else {
      LaunchDepthwiseConv2dBackpropFilterGPU<T, -1, -1, -1>(
          d, args, out_backprop, input, filter_backprop, data_format);
    }
  }
};

template struct DepthwiseConv2dBackpropFilterGPULaunch<float>;
template struct DepthwiseConv2dBackpropFilterGPULaunch<double>;
}  // namespace tensorflow
#endif  // GOOGLE_CUDA
