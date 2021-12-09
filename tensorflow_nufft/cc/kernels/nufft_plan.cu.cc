/* Copyright 2021 University College London. All Rights Reserved.

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

/* Copyright 2017-2021 The Simons Foundation. All Rights Reserved.

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

#include "tensorflow_nufft/cc/kernels/nufft_plan.h"

#include <thrust/device_ptr.h>
#include <thrust/scan.h>

#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"

#include "tensorflow_nufft/third_party/cuda_samples/helper_cuda.h"
#include "tensorflow_nufft/cc/kernels/nufft_util.h"
#include "tensorflow_nufft/cc/kernels/omp_api.h"

// NU coord handling macro: if p is true, rescales from [-pi,pi] to [0,N], then
// folds *only* one period below and above, ie [-N,2N], into the domain [0,N]...
#define RESCALE(x, N, p) (p ? \
		     ((x * kOneOverTwoPi<FloatType> + (x < -kPi<FloatType> ? 1.5 : \
         (x >= kPi<FloatType> ? -0.5 : 0.5)))*N) : \
		     (x < 0 ? x + N : (x >= N ? x - N : x)))

namespace tensorflow {
namespace nufft {

namespace {

template<typename FloatType>
constexpr cufftType kCufftType = CUFFT_C2C;
template<>
constexpr cufftType kCufftType<float> = CUFFT_C2C;
template<>
constexpr cufftType kCufftType<double> = CUFFT_Z2Z;

template<typename FloatType>
Status setup_spreader(int rank, FloatType eps, double upsampling_factor,
                      KernelEvaluationMethod kernel_evaluation_method,
                      SpreadParameters<FloatType>& spread_params);

template<typename FloatType>
Status setup_spreader_for_nufft(int rank, FloatType eps,
                                const Options& options,
                                SpreadParameters<FloatType> &spread_params);

void set_bin_sizes(TransformType type, int rank, Options& options);

template<typename FloatType>
Status set_grid_size(int ms,
                     int bin_size,
                     const Options& options,
                     const SpreadParameters<FloatType>& spread_params,
                     int* grid_size);

template<typename FloatType>
Status allocate_gpu_memory_2d(Plan<GPUDevice, FloatType>* d_plan);

template<typename FloatType>
Status allocate_gpu_memory_3d(Plan<GPUDevice, FloatType>* d_plan);

template<typename FloatType>
void free_gpu_memory(Plan<GPUDevice, FloatType>* d_plan);

__device__ int CalcGlobalIdxV2(int xidx, int yidx, int zidx, int nbinx, int nbiny, int nbinz) {
	return xidx + yidx*nbinx + zidx*nbinx*nbiny;
}

template<typename FloatType>
__global__ void CalcBinSizeNoGhost2DKernel(int M, int nf1, int nf2, int  bin_size_x, 
    int bin_size_y, int nbinx, int nbiny, int* bin_size, FloatType *x, FloatType *y, 
    int* sortidx, int pirange) {
	int binidx, binx, biny;
	int oldidx;
	FloatType x_rescaled,y_rescaled;
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<M; i+=gridDim.x*blockDim.x) {
		x_rescaled=RESCALE(x[i], nf1, pirange);
		y_rescaled=RESCALE(y[i], nf2, pirange);
		binx = floor(x_rescaled/bin_size_x);
		binx = binx >= nbinx ? binx-1 : binx;
		binx = binx < 0 ? 0 : binx;
		biny = floor(y_rescaled/bin_size_y);
		biny = biny >= nbiny ? biny-1 : biny;
		biny = biny < 0 ? 0 : biny;
		binidx = binx+biny*nbinx;
		oldidx = atomicAdd(&bin_size[binidx], 1);
		sortidx[i] = oldidx;
		if (binx >= nbinx || biny >= nbiny) {
			sortidx[i] = -biny;
		}
	}
}

template<typename FloatType>
__global__ void CalcBinSizeNoGhost3DKernel(int M, int nf1, int nf2, int nf3,
    int bin_size_x, int bin_size_y, int bin_size_z,
    int nbinx, int nbiny, int nbinz, int* bin_size, FloatType *x, FloatType *y, FloatType *z,
    int* sortidx, int pirange) {
	int binidx, binx, biny, binz;
	int oldidx;
	FloatType x_rescaled,y_rescaled,z_rescaled;
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<M; i+=gridDim.x*blockDim.x) {
		x_rescaled=RESCALE(x[i], nf1, pirange);
		y_rescaled=RESCALE(y[i], nf2, pirange);
		z_rescaled=RESCALE(z[i], nf3, pirange);
		binx = floor(x_rescaled/bin_size_x);
		binx = binx >= nbinx ? binx-1 : binx;
		binx = binx < 0 ? 0 : binx;

		biny = floor(y_rescaled/bin_size_y);
		biny = biny >= nbiny ? biny-1 : biny;
		biny = biny < 0 ? 0 : biny;

		binz = floor(z_rescaled/bin_size_z);
		binz = binz >= nbinz ? binz-1 : binz;
		binz = binz < 0 ? 0 : binz;
		binidx = binx+biny*nbinx+binz*nbinx*nbiny;
		oldidx = atomicAdd(&bin_size[binidx], 1);
		sortidx[i] = oldidx;
	}
}

template<typename FloatType>
__global__ void CalcInvertofGlobalSortIdx2DKernel(int M, int bin_size_x, int bin_size_y, 
    int nbinx,int nbiny, int* bin_startpts, int* sortidx, FloatType *x, FloatType *y, 
    int* index, int pirange, int nf1, int nf2) {
	int binx, biny;
	int binidx;
	FloatType x_rescaled, y_rescaled;
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<M; i+=gridDim.x*blockDim.x) {
		x_rescaled=RESCALE(x[i], nf1, pirange);
		y_rescaled=RESCALE(y[i], nf2, pirange);
		binx = floor(x_rescaled/bin_size_x);
		binx = binx >= nbinx ? binx-1 : binx;
		binx = binx < 0 ? 0 : binx;
		biny = floor(y_rescaled/bin_size_y);
		biny = biny >= nbiny ? biny-1 : biny;
		biny = biny < 0 ? 0 : biny;
		binidx = binx+biny*nbinx;

		index[bin_startpts[binidx]+sortidx[i]] = i;
	}
}

template<typename FloatType>
__global__ void CalcInvertofGlobalSortIdx3DKernel(int M, int bin_size_x, int bin_size_y,
    int bin_size_z, int nbinx, int nbiny, int nbinz, int* bin_startpts,
    int* sortidx, FloatType *x, FloatType *y, FloatType *z, int* index, int pirange, int nf1,
    int nf2, int nf3) {
	int binx,biny,binz;
	int binidx;
	FloatType x_rescaled,y_rescaled,z_rescaled;
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<M; i+=gridDim.x*blockDim.x) {
		x_rescaled=RESCALE(x[i], nf1, pirange);
		y_rescaled=RESCALE(y[i], nf2, pirange);
		z_rescaled=RESCALE(z[i], nf3, pirange);
		binx = floor(x_rescaled/bin_size_x);
		binx = binx >= nbinx ? binx-1 : binx;
		binx = binx < 0 ? 0 : binx;
		biny = floor(y_rescaled/bin_size_y);
		biny = biny >= nbiny ? biny-1 : biny;
		biny = biny < 0 ? 0 : biny;
		binz = floor(z_rescaled/bin_size_z);
		binz = binz >= nbinz ? binz-1 : binz;
		binz = binz < 0 ? 0 : binz;
		binidx = CalcGlobalIdxV2(binx,biny,binz,nbinx,nbiny,nbinz);

		index[bin_startpts[binidx]+sortidx[i]] = i;
	}
}

__global__ void TrivialGlobalSortIdxKernel(int M, int* index) {
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<M; i+=gridDim.x*blockDim.x) {
		index[i] = i;
	}
}

__global__ void CalcSubproblemKernel(int* bin_size, int* num_subprob, int maxsubprobsize,
	  int numbins) {
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<numbins;
		i+=gridDim.x*blockDim.x) {
		num_subprob[i]=ceil(bin_size[i]/(float) maxsubprobsize);
	}
}

__global__ void MapBinToSubproblemKernel(int* d_subprob_to_bin,int* d_subprobstartpts,
	  int* d_numsubprob,int numbins) {
	for (int i=threadIdx.x+blockIdx.x*blockDim.x; i<numbins;
		i+=gridDim.x*blockDim.x) {
		for (int j=0; j<d_numsubprob[i]; j++) {
			d_subprob_to_bin[d_subprobstartpts[i]+j]=i;
		}
	}
}

} // namespace

template<typename FloatType>
Plan<GPUDevice, FloatType>::Plan(
    OpKernelContext* context,
    TransformType type,
    int rank,
    gtl::InlinedVector<int, 4> num_modes,
    FftDirection fft_direction,
    int num_transforms,
    FloatType tol,
    const Options& options) 
    : PlanBase<GPUDevice, FloatType>(context) {

  OP_REQUIRES(context,
              type != TransformType::TYPE_3,
              errors::Unimplemented("type-3 transforms are not implemented"));
  OP_REQUIRES(context, rank >= 2 && rank <= 3,
              errors::InvalidArgument("rank must be 2 or 3"));
  OP_REQUIRES(context, num_transforms >= 1,
              errors::InvalidArgument("num_transforms must be >= 1"));
  OP_REQUIRES(context, rank == num_modes.size(),
              errors::InvalidArgument("num_modes must have size equal to rank"));

  // auto* stream = context->op_device_context()->stream();
  // OP_REQUIRES(context, stream, errors::Internal("No GPU stream available."));

  // int device_id = stream->parent()->device_ordinal();

  GPUDevice device = context->eigen_device<GPUDevice>();

  // std::cout << "device_id = " << device_id << std::endl;
  // cudaSetDevice(0);

  
  // TODO: check options.
  //  - If mode_order == FFT, raise unimplemented error.
  //  - If check_bounds == true, raise unimplemented error.

        // Mult-GPU support: set the CUDA Device ID:
        int orig_gpu_device_id;
        cudaGetDevice(& orig_gpu_device_id);
        // if (spread_params == NULL) {
        //     // options might not be supplied to this function => assume device
        //     // 0 by default
        //     cudaSetDevice(0);
        // } else {
        cudaSetDevice(options.gpu_device_id);
        // }


  // Initialize all values to 0. TODO: move to initialization list.
  this->nf1 = 0;
  this->nf2 = 0;
  this->nf3 = 0;
  this->ms = 0;
  this->mt = 0;
  this->mu = 0;
  this->totalnumsubprob = 0;
  this->c = nullptr;
  this->fk = nullptr;
  this->idxnupts = nullptr;
  this->sortidx = nullptr;
  this->numsubprob = nullptr;
  this->binsize = nullptr;
  this->binstartpts = nullptr;
  this->subprob_to_bin = nullptr;
  this->subprobstartpts = nullptr;
  this->finegridsize = nullptr;
  this->fgstartpts = nullptr;
  this->numnupts = nullptr;
  this->subprob_to_nupts = nullptr;

  // Copy options.
  this->options_ = options;

  // Select kernel evaluation method.
  if (this->options_.kernel_evaluation_method == KernelEvaluationMethod::AUTO) {
    this->options_.kernel_evaluation_method = KernelEvaluationMethod::DIRECT;
  }

  // Select upsampling factor. Currently always defaults to 2.
  if (this->options_.upsampling_factor == 0.0) {
    this->options_.upsampling_factor = 2.0;
  }

  // Configure threading (irrelevant for GPU computation, but is used for some
  // CPU computations).
  if (this->options_.num_threads == 0) {
    this->options_.num_threads = OMP_GET_MAX_THREADS();
  }

  // Select whether or not to sort points.
  if (this->options_.sort_points == SortPoints::AUTO) {
    this->options_.sort_points = SortPoints::YES;
  }

  // Select spreading method.
  if (this->options_.spread_method == SpreadMethod::AUTO) {
    if (rank == 2 && type == TransformType::TYPE_1)
      this->options_.spread_method = SpreadMethod::SUBPROBLEM;
    else if (rank == 2 && type == TransformType::TYPE_2)
      this->options_.spread_method = SpreadMethod::NUPTS_DRIVEN;
    else if (rank == 3 && type == TransformType::TYPE_1)
      this->options_.spread_method = SpreadMethod::SUBPROBLEM;
    else if (rank == 3 && type == TransformType::TYPE_2)
      this->options_.spread_method = SpreadMethod::NUPTS_DRIVEN;
  }

  // This must be set before calling setup_spreader_for_nufft.
  this->spread_params_.spread_only = this->options_.spread_only;

  // Setup spreading options.
  OP_REQUIRES_OK(context,
                 setup_spreader_for_nufft(
                    rank, tol, this->options_, this->spread_params_));

  this->rank_ = rank;
  this->ms = num_modes[0];
  if (rank > 1)
    this->mt = num_modes[1];
  if (rank > 2)
    this->mu = num_modes[2];

  // Set the bin sizes.
  set_bin_sizes(type, rank, this->options_);

  // Set the grid sizes.
  int nf1 = 1, nf2 = 1, nf3 = 1;
  OP_REQUIRES_OK(context,
                 set_grid_size(this->ms, this->options_.gpu_obin_size.x,
                               this->options_, this->spread_params_, &nf1));
  if (rank > 1) {
    OP_REQUIRES_OK(context,
                   set_grid_size(this->mt, this->options_.gpu_obin_size.y,
                                 this->options_, this->spread_params_, &nf2));
  }
  if (rank > 2) {
    OP_REQUIRES_OK(context,
                   set_grid_size(this->mu, this->options_.gpu_obin_size.z,
                                 this->options_, this->spread_params_, &nf3));
  }

  this->nf1 = nf1;
  this->nf2 = nf2;
  this->nf3 = nf3;
  this->grid_dims_[0] = nf1;
  this->grid_dims_[1] = nf2;
  this->grid_dims_[2] = nf3;
  this->grid_count_ = nf1 * nf2 * nf3;
  this->fft_direction_ = fft_direction;
  this->num_transforms_ = num_transforms;
  this->type_ = type;

  // Select maximum batch size.
  if (this->options_.max_batch_size == 0)
    // Heuristic from test codes.
    this->options_.max_batch_size = min(num_transforms, 8); 

  if (this->type_ == TransformType::TYPE_1)
    this->spread_params_.spread_direction = SpreadDirection::SPREAD;
  if (this->type_ == TransformType::TYPE_2)
    this->spread_params_.spread_direction = SpreadDirection::INTERP;

  switch (this->rank_) {
    case 2: {
      OP_REQUIRES_OK(context, allocate_gpu_memory_2d(this));
      break;
    }
    case 3: {
      OP_REQUIRES_OK(context, allocate_gpu_memory_3d(this));
      break;
    }
    default:
      OP_REQUIRES(context, false,
                  errors::Unimplemented("Invalid rank: ", this->rank_));
  }
  
  // Perform some actions not needed in spread/interp only mode.
  if (!this->options_.spread_only)
  {
    // Allocate fine grid and set convenience pointer.
    int num_grid_elements = this->nf1 * this->nf2 * this->nf3;
    OP_REQUIRES_OK(context, context->allocate_temp(
        DataTypeToEnum<std::complex<FloatType>>::value,
        TensorShape({num_grid_elements * this->options_.max_batch_size}),
        &this->fine_grid_));
    this->fine_grid_data_ = reinterpret_cast<DType*>(
        this->fine_grid_.flat<std::complex<FloatType>>().data());

    // For each dimension, calculate Fourier coefficients of the kernel for
    // deconvolution. The computation is performed on the CPU before
    // transferring the results the GPU.
    int grid_sizes[3] = {this->nf1, this->nf2, this->nf3};
    Tensor kernel_fseries_host[3];
    FloatType* kernel_fseries_host_data[3];
    for (int i = 0; i < this->rank_; i++) {

      // Number of Fourier coefficients.      
      int num_coeffs = grid_sizes[i] / 2 + 1;

      // Allocate host memory and calculate the Fourier series on the CPU.
      AllocatorAttributes attr;
      attr.set_on_host(true);
      OP_REQUIRES_OK(context, context->allocate_temp(
                                  DataTypeToEnum<FloatType>::value,
                                  TensorShape({num_coeffs}),
                                  &kernel_fseries_host[i], attr));
      kernel_fseries_host_data[i] = reinterpret_cast<FloatType*>(
          kernel_fseries_host[i].flat<FloatType>().data());
      kernel_fseries_1d(grid_sizes[i], this->spread_params_,
                        kernel_fseries_host_data[i]);

      // Allocate device memory and save convenience accessors.
      OP_REQUIRES_OK(context, context->allocate_temp(
                                  DataTypeToEnum<FloatType>::value,
                                  TensorShape({num_coeffs}),
                                  &this->kernel_fseries_[i]));
      this->kernel_fseries_data_[i] = reinterpret_cast<FloatType*>(
          this->kernel_fseries_[i].flat<FloatType>().data());

      // Now copy coefficients to device.
      size_t num_bytes = sizeof(FloatType) * num_coeffs;
      device.memcpyHostToDevice(
          reinterpret_cast<void*>(this->kernel_fseries_data_[i]),
          reinterpret_cast<void*>(kernel_fseries_host_data[i]),
          num_bytes);
    }

    // Make the cuFFT plan.
    int elem_count[3];
    int *input_embed = elem_count;
    int *output_embed = elem_count;
    int input_distance = 0;
    int output_distance = 0;
    int input_stride = 1;
    int output_stride = 1;
    int batch_size = this->options_.max_batch_size;
    switch (this->rank_) {
      case 2: {
        elem_count[0] = this->nf2;
        elem_count[1] = this->nf1;
        input_distance = input_embed[0] * input_embed[1];
        output_distance = input_distance;
        break;
      }
      case 3: {
        elem_count[0] = this->nf3;
        elem_count[1] = this->nf2;
        elem_count[2] = this->nf1;
        input_distance = input_embed[0] * input_embed[1] * input_embed[2];
        output_distance = input_distance;
        break;
      }
      default:
        OP_REQUIRES(context, false,
                    errors::Unimplemented("Invalid rank: ", this->rank_));
    }

    cufftResult result = cufftPlanMany(
        &this->fft_plan_, this->rank_, elem_count,
        input_embed, input_stride, input_distance,
        output_embed, output_stride, output_distance,
        kCufftType<FloatType>, batch_size);

    OP_REQUIRES(context, result == CUFFT_SUCCESS,
                errors::Internal(
                    "cufftPlanMany failed with code: ", result));
  }

  // Multi-GPU support: reset the device ID
  cudaSetDevice(orig_gpu_device_id);
}

template<typename FloatType>
Plan<GPUDevice, FloatType>::~Plan() {

  // Mult-GPU support: set the CUDA Device ID:
  int orig_gpu_device_id;
  cudaGetDevice(& orig_gpu_device_id);
  cudaSetDevice(this->options_.gpu_device_id);

  if (this->fft_plan_)
    cufftDestroy(this->fft_plan_);

  free_gpu_memory(this);

        // Multi-GPU support: reset the device ID
        cudaSetDevice(orig_gpu_device_id);
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::set_points(
    int num_points,
    FloatType* points_x,
    FloatType* points_y,
    FloatType* points_z) {

  // Mult-GPU support: set the CUDA Device ID:
  int orig_gpu_device_id;
  cudaGetDevice(& orig_gpu_device_id);
  cudaSetDevice(this->options_.gpu_device_id);

  this->num_points_ = num_points;
  this->points_[0] = points_x;
  this->points_[1] = this->rank_ > 1 ? points_y : nullptr;
  this->points_[2] = this->rank_ > 2 ? points_z : nullptr;

  if (this->sortidx ) checkCudaErrors(cudaFree(this->sortidx));
  if (this->idxnupts) checkCudaErrors(cudaFree(this->idxnupts));

  size_t num_bytes = sizeof(int) * this->num_points_;
  switch (this->options_.spread_method) {
    case SpreadMethod::NUPTS_DRIVEN:
      if (this->spread_params_.sort_points == SortPoints::YES)
        checkCudaErrors(cudaMalloc(&this->sortidx, num_bytes));
      checkCudaErrors(cudaMalloc(&this->idxnupts, num_bytes));
      break;
    case SpreadMethod::SUBPROBLEM:
      checkCudaErrors(cudaMalloc(&this->idxnupts, num_bytes));
      checkCudaErrors(cudaMalloc(&this->sortidx, num_bytes));
      break;
    case SpreadMethod::PAUL:
      checkCudaErrors(cudaMalloc(&this->idxnupts, num_bytes));
      checkCudaErrors(cudaMalloc(&this->sortidx, num_bytes));
      break;
    case SpreadMethod::BLOCK_GATHER:
      checkCudaErrors(cudaMalloc(&this->sortidx, num_bytes));
      break;
  }
  
  TF_RETURN_IF_ERROR(this->init_spreader());

  // Multi-GPU support: reset the device ID
  cudaSetDevice(orig_gpu_device_id);

  return Status::OK();
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::init_spreader() {

  switch(this->options_.spread_method) {
		case SpreadMethod::NUPTS_DRIVEN:
      TF_RETURN_IF_ERROR(this->init_spreader_nupts_driven());
      break;
		case SpreadMethod::SUBPROBLEM:
      TF_RETURN_IF_ERROR(this->init_spreader_subproblem());
      break;
    case SpreadMethod::PAUL:
      TF_RETURN_IF_ERROR(this->init_spreader_paul());
      break;
    case SpreadMethod::BLOCK_GATHER:
      TF_RETURN_IF_ERROR(this->init_spreader_block_gather());
		  break;
	}
  return Status::OK();
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::init_spreader_nupts_driven() {
  
  int num_blocks = (this->num_points_ + 1024 - 1) / 1024;
  int threads_per_block = 1024;

  if (this->spread_params_.sort_points == SortPoints::YES) {
    int bin_size[3];
    bin_size[0] = this->options_.gpu_bin_size.x;
    bin_size[1] = this->options_.gpu_bin_size.y;
    bin_size[2] = this->options_.gpu_bin_size.z;
    if (bin_size[0] < 0 || bin_size[1] < 0 || bin_size[2] < 0) {
      return errors::InvalidArgument(
          "Invalid bin size: (", bin_size[0], ", ", bin_size[1], ", ",
          bin_size[2], ")");
    }

    int num_bins[3] = {1, 1, 1};
    int bin_count = 1;
    for (int i = 0; i < this->rank_; i++) {
      num_bins[i] = (this->grid_dims_[i] + bin_size[i] - 1) / bin_size[i];
      bin_count *= num_bins[i];
    }

    // This may not be necessary.
    this->device_.synchronize();

    // Calculate bin sizes.
    this->device_.memset(this->binsize, 0, bin_count * sizeof(int));
    switch (this->rank_) {
      case 2:
        TF_CHECK_OK(GpuLaunchKernel(
            CalcBinSizeNoGhost2DKernel<FloatType>,
            num_blocks, threads_per_block, 0, this->device_.stream(),
            this->num_points_, this->grid_dims_[0], this->grid_dims_[1],
            bin_size[0], bin_size[1], num_bins[0], num_bins[1],
            this->binsize, this->points_[0], this->points_[1], this->sortidx,
            this->spread_params_.pirange));
        break;
      case 3:
        TF_CHECK_OK(GpuLaunchKernel(
            CalcBinSizeNoGhost3DKernel<FloatType>,
            num_blocks, threads_per_block, 0, this->device_.stream(),
            this->num_points_, this->grid_dims_[0], this->grid_dims_[1],
            this->grid_dims_[2],
            bin_size[0],bin_size[1],bin_size[2],num_bins[0],num_bins[1],num_bins[2],
            this->binsize,this->points_[0],this->points_[1],this->points_[2],
            this->sortidx,this->spread_params_.pirange));
        break;
      default:
        return errors::Unimplemented("Invalid rank: ", this->rank_);
    }

    thrust::device_ptr<int> d_bin_sizes(this->binsize);
    thrust::device_ptr<int> d_bin_start_points(this->binstartpts);
    thrust::exclusive_scan(d_bin_sizes, d_bin_sizes + bin_count,
                           d_bin_start_points);

    switch (this->rank_) {
      case 2:
        TF_CHECK_OK(GpuLaunchKernel(
            CalcInvertofGlobalSortIdx2DKernel<FloatType>,
            num_blocks, threads_per_block, 0, this->device_.stream(),
            this->num_points_, bin_size[0], bin_size[1], num_bins[0],
            num_bins[1], this->binstartpts, this->sortidx,
            this->points_[0], this->points_[1], this->idxnupts,
            this->spread_params_.pirange, this->grid_dims_[0],
            this->grid_dims_[1]));
        break;
      case 3:
        CalcInvertofGlobalSortIdx3DKernel<FloatType><<<num_blocks, threads_per_block>>>(
          this->num_points_,bin_size[0],
          bin_size[1],bin_size[2],num_bins[0],num_bins[1],num_bins[2],
          this->binstartpts,
          this->sortidx,this->points_[0],this->points_[1],this->points_[2],
          this->idxnupts, this->spread_params_.pirange, this->grid_dims_[0],
          this->grid_dims_[1], this->grid_dims_[2]);
        break;
      default:
        return errors::Unimplemented("Invalid rank: ", this->rank_);
    }
  } else {
    TF_CHECK_OK(GpuLaunchKernel(
        TrivialGlobalSortIdxKernel,
        num_blocks, threads_per_block, 0, this->device_.stream(),
        this->num_points_, this->idxnupts));
  }

  return Status::OK();
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::init_spreader_subproblem() {
  int num_blocks = (this->num_points_ + 1024 - 1) / 1024;
  int threads_per_block = 1024;

  int maxsubprobsize=this->options_.gpu_max_subproblem_size;

  int bin_size[3];
  bin_size[0] = this->options_.gpu_bin_size.x;
  bin_size[1] = this->options_.gpu_bin_size.y;
  bin_size[2] = this->options_.gpu_bin_size.z;
  if (bin_size[0] < 0 || bin_size[1] < 0 || bin_size[2] < 0) {
    return errors::InvalidArgument(
        "Invalid bin size: (", bin_size[0], ", ", bin_size[1], ", ",
        bin_size[2], ")");
  }

  int num_bins[3] = {1, 1, 1};
  int bin_count = 1;
  for (int i = 0; i < this->rank_; i++) {
    num_bins[i] = (this->grid_dims_[i] + bin_size[i] - 1) / bin_size[i];
    bin_count *= num_bins[i];
  }

  int *d_binsize = this->binsize;
  int *d_binstartpts = this->binstartpts;
  int *d_sortidx = this->sortidx;
  int *d_numsubprob = this->numsubprob;
  int *d_subprobstartpts = this->subprobstartpts;
  int *d_idxnupts = this->idxnupts;

  int *d_subprob_to_bin = NULL;

  int pirange=this->spread_params_.pirange;

  // This may not be necessary.
  this->device_.synchronize();

  // Calculate bin sizes.
  this->device_.memset(this->binsize, 0, bin_count * sizeof(int));
  switch (this->rank_) {
    case 2:
      TF_CHECK_OK(GpuLaunchKernel(
          CalcBinSizeNoGhost2DKernel<FloatType>,
          num_blocks, threads_per_block, 0, this->device_.stream(),
          this->num_points_, this->grid_dims_[0],
          this->grid_dims_[1], bin_size[0], bin_size[1],
          num_bins[0], num_bins[1], d_binsize, this->points_[0],
          this->points_[1], d_sortidx, pirange));
      break;
    case 3:
      TF_CHECK_OK(GpuLaunchKernel(
          CalcBinSizeNoGhost3DKernel<FloatType>,
          num_blocks, threads_per_block, 0, this->device_.stream(),
          this->num_points_, this->grid_dims_[0], this->grid_dims_[1],
          this->grid_dims_[2], bin_size[0],
          bin_size[1], bin_size[2], num_bins[0], num_bins[1], num_bins[2], d_binsize,
          this->points_[0], this->points_[1], this->points_[2], d_sortidx, pirange));
      break;
    default:
      return errors::Unimplemented("Invalid rank: ", this->rank_);
  }

  thrust::device_ptr<int> d_ptr(d_binsize);
  thrust::device_ptr<int> d_result(d_binstartpts);
  thrust::exclusive_scan(d_ptr, d_ptr + bin_count, d_result);

  switch (this->rank_) {
    case 2:
      TF_CHECK_OK(GpuLaunchKernel(
          CalcInvertofGlobalSortIdx2DKernel<FloatType>,
          num_blocks, threads_per_block, 0, this->device_.stream(),
          this->num_points_, bin_size[0], bin_size[1], num_bins[0],
          num_bins[1], d_binstartpts, d_sortidx, this->points_[0],
          this->points_[1], d_idxnupts, pirange, this->grid_dims_[0],
          this->grid_dims_[1]));
      break;
    case 3:
      TF_CHECK_OK(GpuLaunchKernel(
          CalcInvertofGlobalSortIdx3DKernel<FloatType>,
          num_blocks, threads_per_block, 0, this->device_.stream(),
          this->num_points_, bin_size[0],
          bin_size[1], bin_size[2], num_bins[0], num_bins[1], num_bins[2],
          d_binstartpts, d_sortidx, this->points_[0], this->points_[1],
          this->points_[2], d_idxnupts, pirange, this->grid_dims_[0],
          this->grid_dims_[1], this->grid_dims_[2]));
      break;
    default:
      return errors::Unimplemented("Invalid rank: ", this->rank_);
  }

  TF_CHECK_OK(GpuLaunchKernel(
      CalcSubproblemKernel, num_blocks, threads_per_block, 0,
      this->device_.stream(), d_binsize, d_numsubprob, maxsubprobsize,
      bin_count));

  d_ptr    = thrust::device_pointer_cast(d_numsubprob);
  d_result = thrust::device_pointer_cast(d_subprobstartpts+1);
  thrust::inclusive_scan(d_ptr, d_ptr + bin_count, d_result);
  checkCudaErrors(cudaMemset(d_subprobstartpts,0,sizeof(int)));

  int totalnumsubprob;
  checkCudaErrors(cudaMemcpy(&totalnumsubprob,&d_subprobstartpts[bin_count],
    sizeof(int),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMalloc(&d_subprob_to_bin,totalnumsubprob*sizeof(int)));

  num_blocks = (bin_count + 1024 - 1) / 1024;
  threads_per_block = 1024;

  TF_CHECK_OK(GpuLaunchKernel(
      MapBinToSubproblemKernel, num_blocks, threads_per_block, 0,
      this->device_.stream(), d_subprob_to_bin, d_subprobstartpts,
      d_numsubprob, bin_count));

  assert(d_subprob_to_bin != NULL);
  if (this->subprob_to_bin != NULL) cudaFree(this->subprob_to_bin);
  this->subprob_to_bin = d_subprob_to_bin;
  assert(this->subprob_to_bin != NULL);
  this->totalnumsubprob = totalnumsubprob;

  return Status::OK();
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::init_spreader_paul() {
	return errors::Unimplemented("init_spreader_paul");
}

template<typename FloatType>
Status Plan<GPUDevice, FloatType>::init_spreader_block_gather() {
  return errors::Unimplemented("init_spreader_block_gather");
}

namespace {

template<typename FloatType>
Status setup_spreader(int rank, FloatType eps, double upsampling_factor,
                      KernelEvaluationMethod kernel_evaluation_method,
                      SpreadParameters<FloatType>& spread_params)
// Initializes spreader kernel parameters given desired NUFFT tol eps,
// upsampling factor (=sigma in paper, or R in Dutt-Rokhlin), and ker eval meth
// (etiher 0:exp(sqrt()), 1: Horner ppval).
// Also sets all default options in SpreadParameters<FloatType>. See cnufftspread.h for spread_params.
// Must call before any kernel evals done.
// Returns: 0 success, 1, warning, >1 failure (see error codes in utils.h)
{
  if (upsampling_factor != 2.0) {
    if (kernel_evaluation_method == KernelEvaluationMethod::HORNER) {
      return errors::Internal(
          "Horner kernel evaluation only supports the standard "
          "upsampling factor of 2.0, but got ", upsampling_factor);
    }
    if (upsampling_factor <= 1.0) {
      return errors::Internal(
          "upsampling_factor must be > 1.0, but is ", upsampling_factor);
    }
  }
    
  // defaults... (user can change after this function called)
  spread_params.spread_direction = SpreadDirection::SPREAD;
  spread_params.pirange = 1;             // user also should always set this
  spread_params.upsampling_factor = upsampling_factor;

  // as in FINUFFT v2.0, allow too-small-eps by truncating to eps_mach...
  if (eps < kEpsilon<FloatType>) {
    eps = kEpsilon<FloatType>;
  }

  // Set kernel width w (aka ns) and ES kernel beta parameter, in spread_params...
  int ns = std::ceil(-log10(eps / (FloatType)10.0));   // 1 digit per power of ten
  if (upsampling_factor != 2.0)           // override ns for custom sigma
    ns = std::ceil(-log(eps) / (kPi<FloatType> * sqrt(1.0 - 1.0 / upsampling_factor)));  // formula, gamma=1
  ns = max(2, ns);               // we don't have ns=1 version yet
  if (ns > kMaxKernelWidth) {         // clip to match allocated arrays
    ns = kMaxKernelWidth;
  }
  spread_params.nspread = ns;

  spread_params.ES_halfwidth = (FloatType)ns / 2;   // constants to help ker eval (except Horner)
  spread_params.ES_c = 4.0 / (FloatType)(ns * ns);

  FloatType beta_over_ns = 2.30;         // gives decent betas for default sigma=2.0
  if (ns == 2) beta_over_ns = 2.20;  // some small-width tweaks...
  if (ns == 3) beta_over_ns = 2.26;
  if (ns == 4) beta_over_ns = 2.38;
  if (upsampling_factor != 2.0) {          // again, override beta for custom sigma
    FloatType gamma=0.97;              // must match devel/gen_all_horner_C_code.m
    beta_over_ns = gamma * kPi<FloatType> * (1-1/(2*upsampling_factor));  // formula based on cutoff
  }
  spread_params.ES_beta = beta_over_ns * (FloatType)ns;    // set the kernel beta parameter

  if (spread_params.spread_only)
    spread_params.ES_scale = calculate_scale_factor(rank, spread_params);

  return Status::OK();
}

template<typename FloatType>
Status setup_spreader_for_nufft(int rank, FloatType eps,
                                const Options& options,
                                SpreadParameters<FloatType>& spread_params)
// Set up the spreader parameters given eps, and pass across various nufft
// options. Report status of setup_spreader.  Barnett 10/30/17
{
  TF_RETURN_IF_ERROR(setup_spreader(
      rank, eps, options.upsampling_factor,
      options.kernel_evaluation_method, spread_params));

  spread_params.sort_points = options.sort_points;
  spread_params.spread_method = options.spread_method;
  spread_params.gpu_bin_size = options.gpu_bin_size;
  spread_params.gpu_obin_size = options.gpu_obin_size;
  spread_params.pirange = 1;
  spread_params.num_threads = options.num_threads;

  return Status::OK();
}

void set_bin_sizes(TransformType type, int rank, Options& options) {
  switch(rank) {
    case 2:
      options.gpu_bin_size.x = (options.gpu_bin_size.x == 0) ? 32 :
          options.gpu_bin_size.x;
      options.gpu_bin_size.y = (options.gpu_bin_size.y == 0) ? 32 :
          options.gpu_bin_size.y;
      options.gpu_bin_size.z = 1;
      break;
    case 3:
      switch(options.spread_method) {
        case SpreadMethod::NUPTS_DRIVEN:
        case SpreadMethod::SUBPROBLEM:
          options.gpu_bin_size.x = (options.gpu_bin_size.x == 0) ? 16 :
              options.gpu_bin_size.x;
          options.gpu_bin_size.y = (options.gpu_bin_size.y == 0) ? 16 :
              options.gpu_bin_size.y;
          options.gpu_bin_size.z = (options.gpu_bin_size.z == 0) ? 2 :
              options.gpu_bin_size.z;
          break;
        case SpreadMethod::BLOCK_GATHER:
          options.gpu_obin_size.x = (options.gpu_obin_size.x == 0) ? 8 :
              options.gpu_obin_size.x;
          options.gpu_obin_size.y = (options.gpu_obin_size.y == 0) ? 8 :
              options.gpu_obin_size.y;
          options.gpu_obin_size.z = (options.gpu_obin_size.z == 0) ? 8 :
              options.gpu_obin_size.z;
          options.gpu_bin_size.x = (options.gpu_bin_size.x == 0) ? 4 :
              options.gpu_bin_size.x;
          options.gpu_bin_size.y = (options.gpu_bin_size.y == 0) ? 4 :
              options.gpu_bin_size.y;
          options.gpu_bin_size.z = (options.gpu_bin_size.z == 0) ? 4 :
              options.gpu_bin_size.z;
          break;
      }
      break;
  }
}

template<typename FloatType>
Status set_grid_size(int ms,
                     int bin_size,
                     const Options& options,
                     const SpreadParameters<FloatType>& spread_params,
                     int* grid_size) {
  // for spread/interp only, we do not apply oversampling (Montalt 6/8/2021).
  if (options.spread_only) {
    *grid_size = ms;
  } else {
    *grid_size = static_cast<int>(options.upsampling_factor * ms);
  }

  // This is required to avoid errors.
  if (*grid_size < 2 * spread_params.nspread)
    *grid_size = 2 * spread_params.nspread;

  // Check if array size is too big.
  if (*grid_size > kMaxArraySize) {
    return errors::Internal(
        "Upsampled dim size too big: ", *grid_size, " > ", kMaxArraySize);
  }

  // Find the next smooth integer.
  if (options.spread_method == SpreadMethod::BLOCK_GATHER)
    *grid_size = next_smooth_int(*grid_size, bin_size);
  else
    *grid_size = next_smooth_int(*grid_size);

  // For spread/interp only mode, make sure that the grid size is valid.
  if (options.spread_only && *grid_size != ms) {
    return errors::Internal(
        "Invalid grid size: ", ms, ". Value should be even, "
        "larger than the kernel (", 2 * spread_params.nspread, ") and have no prime "
        "factors larger than 5.");
  }

  return Status::OK();
}

template<typename FloatType>
Status allocate_gpu_memory_2d(Plan<GPUDevice, FloatType>* d_plan) {

  // Mult-GPU support: set the CUDA Device ID:
  int orig_gpu_device_id;
  cudaGetDevice(& orig_gpu_device_id);
  cudaSetDevice(d_plan->options_.gpu_device_id);

  d_plan->bin_size_[0] = d_plan->options_.gpu_bin_size.x;
  d_plan->bin_size_[1] = d_plan->rank_ > 1 ? d_plan->options_.gpu_bin_size.y : 1;
  d_plan->bin_size_[2] = d_plan->rank_ > 2 ? d_plan->options_.gpu_bin_size.z : 1;

  d_plan->num_bins_[0] = 1;
  d_plan->num_bins_[1] = 1;
  d_plan->num_bins_[2] = 1;
  d_plan->bin_count_ = 1;
  for (int i = 0; i < d_plan->rank_; i++) {
    d_plan->num_bins_[i] = (d_plan->grid_dims_[i] + d_plan->bin_size_[i] - 1) / d_plan->bin_size_[i];
    d_plan->bin_count_ *= d_plan->num_bins_[i];
  }

  // No extra memory is needed in nuptsdriven method (case 1)
  size_t bin_bytes = sizeof(int) * d_plan->bin_count_;
  size_t grid_bytes = sizeof(int) * d_plan->grid_count_;
  switch (d_plan->options_.spread_method) {
    case SpreadMethod::NUPTS_DRIVEN:
      {
        if (d_plan->spread_params_.sort_points == SortPoints::YES) {
          checkCudaErrors(cudaMalloc(&d_plan->binsize, bin_bytes));
          checkCudaErrors(cudaMalloc(&d_plan->binstartpts, bin_bytes));
        }
      }
      break;
    case SpreadMethod::SUBPROBLEM:
      {
        checkCudaErrors(cudaMalloc(&d_plan->numsubprob, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->binsize, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->binstartpts, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->subprobstartpts,
            sizeof(int) * (d_plan->bin_count_ + 1)));
      }
      break;
    case SpreadMethod::PAUL:
      {
        checkCudaErrors(cudaMalloc(&d_plan->finegridsize, grid_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->fgstartpts, grid_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->numsubprob, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->binsize, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->binstartpts, bin_bytes));
        checkCudaErrors(cudaMalloc(&d_plan->subprobstartpts,
            sizeof(int) * (d_plan->bin_count_ + 1)));
      }
      break;
    default:
      return errors::Internal("Invalid GPU spread method");
  }

  // Multi-GPU support: reset the device ID
  cudaSetDevice(orig_gpu_device_id);
  
  return Status::OK();
}

template<typename FloatType>
Status allocate_gpu_memory_3d(Plan<GPUDevice, FloatType>* d_plan) {

        // Mult-GPU support: set the CUDA Device ID:
        int orig_gpu_device_id;
        cudaGetDevice(& orig_gpu_device_id);
        cudaSetDevice(d_plan->options_.gpu_device_id);

  int nf1 = d_plan->nf1;
  int nf2 = d_plan->nf2;
  int nf3 = d_plan->nf3;

  switch(d_plan->options_.spread_method)
  {
    case SpreadMethod::NUPTS_DRIVEN:
      {
        if (d_plan->spread_params_.sort_points == SortPoints::YES) {
          int numbins[3];
          numbins[0] = ceil((FloatType) nf1/d_plan->options_.gpu_bin_size.x);
          numbins[1] = ceil((FloatType) nf2/d_plan->options_.gpu_bin_size.y);
          numbins[2] = ceil((FloatType) nf3/d_plan->options_.gpu_bin_size.z);
          checkCudaErrors(cudaMalloc(&d_plan->binsize,numbins[0]*
            numbins[1]*numbins[2]*sizeof(int)));
          checkCudaErrors(cudaMalloc(&d_plan->binstartpts,numbins[0]*
            numbins[1]*numbins[2]*sizeof(int)));
        }
      }
      break;
    case SpreadMethod::SUBPROBLEM:
      {
        int numbins[3];
        numbins[0] = ceil((FloatType) nf1/d_plan->options_.gpu_bin_size.x);
        numbins[1] = ceil((FloatType) nf2/d_plan->options_.gpu_bin_size.y);
        numbins[2] = ceil((FloatType) nf3/d_plan->options_.gpu_bin_size.z);
        checkCudaErrors(cudaMalloc(&d_plan->numsubprob,numbins[0]*
          numbins[1]*numbins[2]*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->binsize,numbins[0]*
          numbins[1]*numbins[2]*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->binstartpts,numbins[0]*
          numbins[1]*numbins[2]*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->subprobstartpts,
          (numbins[0]*numbins[1]*numbins[2]+1)*sizeof(int)));
      }
      break;
    case SpreadMethod::BLOCK_GATHER:
      {
        int numobins[3], numbins[3];
        int binsperobins[3];
        numobins[0] = ceil((FloatType) nf1/d_plan->options_.gpu_obin_size.x);
        numobins[1] = ceil((FloatType) nf2/d_plan->options_.gpu_obin_size.y);
        numobins[2] = ceil((FloatType) nf3/d_plan->options_.gpu_obin_size.z);

        binsperobins[0] = d_plan->options_.gpu_obin_size.x/
          d_plan->options_.gpu_bin_size.x;
        binsperobins[1] = d_plan->options_.gpu_obin_size.y/
          d_plan->options_.gpu_bin_size.y;
        binsperobins[2] = d_plan->options_.gpu_obin_size.z/
          d_plan->options_.gpu_bin_size.z;

        numbins[0] = numobins[0]*(binsperobins[0]+2);
        numbins[1] = numobins[1]*(binsperobins[1]+2);
        numbins[2] = numobins[2]*(binsperobins[2]+2);

        checkCudaErrors(cudaMalloc(&d_plan->numsubprob,
          numobins[0]*numobins[1]*numobins[2]*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->binsize,
          numbins[0]*numbins[1]*numbins[2]*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->binstartpts,
          (numbins[0]*numbins[1]*numbins[2]+1)*sizeof(int)));
        checkCudaErrors(cudaMalloc(&d_plan->subprobstartpts,(numobins[0]
          *numobins[1]*numobins[2]+1)*sizeof(int)));
      }
      break;
    default:
      return errors::Internal("Invalid GPU spread method");
  }

  // Multi-GPU support: reset the device ID
  cudaSetDevice(orig_gpu_device_id);

  return Status::OK();
}

template<typename FloatType>
void free_gpu_memory(Plan<GPUDevice, FloatType>* d_plan) {

      // Mult-GPU support: set the CUDA Device ID:
      int orig_gpu_device_id;
      cudaGetDevice(& orig_gpu_device_id);
      cudaSetDevice(d_plan->options_.gpu_device_id);

  switch(d_plan->options_.spread_method)
  {
    case SpreadMethod::NUPTS_DRIVEN:
      {
        if (d_plan->spread_params_.sort_points == SortPoints::YES) {
          if (d_plan->idxnupts)
            checkCudaErrors(cudaFree(d_plan->idxnupts));
          if (d_plan->sortidx)
            checkCudaErrors(cudaFree(d_plan->sortidx));
          checkCudaErrors(cudaFree(d_plan->binsize));
          checkCudaErrors(cudaFree(d_plan->binstartpts));
        }else{
          if (d_plan->idxnupts)
            checkCudaErrors(cudaFree(d_plan->idxnupts));
        }
      }
      break;
    case SpreadMethod::SUBPROBLEM:
      {
        if (d_plan->idxnupts)
          checkCudaErrors(cudaFree(d_plan->idxnupts));
        if (d_plan->sortidx)
          checkCudaErrors(cudaFree(d_plan->sortidx));
        checkCudaErrors(cudaFree(d_plan->numsubprob));
        checkCudaErrors(cudaFree(d_plan->binsize));
        checkCudaErrors(cudaFree(d_plan->binstartpts));
        checkCudaErrors(cudaFree(d_plan->subprobstartpts));
        checkCudaErrors(cudaFree(d_plan->subprob_to_bin));
      }
      break;
    case SpreadMethod::PAUL:
      {
        if (d_plan->idxnupts)
          checkCudaErrors(cudaFree(d_plan->idxnupts));
        if (d_plan->sortidx)
          checkCudaErrors(cudaFree(d_plan->sortidx));
        checkCudaErrors(cudaFree(d_plan->numsubprob));
        checkCudaErrors(cudaFree(d_plan->binsize));
        checkCudaErrors(cudaFree(d_plan->finegridsize));
        checkCudaErrors(cudaFree(d_plan->binstartpts));
        checkCudaErrors(cudaFree(d_plan->subprobstartpts));
        checkCudaErrors(cudaFree(d_plan->subprob_to_bin));
      }
      break;
    case SpreadMethod::BLOCK_GATHER:
      {
        if (d_plan->idxnupts)
          checkCudaErrors(cudaFree(d_plan->idxnupts));
        if (d_plan->sortidx)
          checkCudaErrors(cudaFree(d_plan->sortidx));
        checkCudaErrors(cudaFree(d_plan->numsubprob));
        checkCudaErrors(cudaFree(d_plan->binsize));
        checkCudaErrors(cudaFree(d_plan->binstartpts));
        checkCudaErrors(cudaFree(d_plan->subprobstartpts));
        checkCudaErrors(cudaFree(d_plan->subprob_to_bin));
      }
      break;
  }

        // Multi-GPU support: reset the device ID
        cudaSetDevice(orig_gpu_device_id);
}

} // namespace

template class Plan<GPUDevice, float>;
template class Plan<GPUDevice, double>;

} // namespace nufft
} // namespace tensorflow

#endif // GOOGLE_CUDA
