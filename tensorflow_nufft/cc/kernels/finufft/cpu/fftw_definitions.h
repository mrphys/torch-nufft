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

#ifndef TENSORFLOW_NUFFT_KERNELS_FINUFFT_FFTW_DEFINITIONS_H
#define TENSORFLOW_NUFFT_KERNELS_FINUFFT_FFTW_DEFINITIONS_H

#include <fftw3.h>

// Here we define typedefs and macros to switch between single and double
// precision library compilation, which need different FFTW commands.


// prec-indep interfaces to FFTW and other math utilities...
#ifdef SINGLE
  typedef fftwf_complex FFTW_CPX;           //  single-prec has fftwf_*
  typedef fftwf_plan FFTW_PLAN;
  #define FFTW_INIT fftwf_init_threads
  #define FFTW_PLAN_TH fftwf_plan_with_nthreads
  #define FFTW_ALLOC_RE fftwf_alloc_real
  #define FFTW_ALLOC_CPX fftwf_alloc_complex
  #define FFTW_PLAN_MANY_DFT fftwf_plan_many_dft
  #define FFTW_EXECUTE fftwf_execute
  #define FFTW_DESTROY_PLAN fftwf_destroy_plan
  #define FFTW_FREE fftwf_free
  #define FFTW_FORGET_WISDOM fftwf_forget_wisdom
  #define FFTW_CLEANUP fftwf_cleanup
  #define FFTW_CLEANUP_THREADS fftwf_cleanup_threads
  #ifdef FFTW_PLAN_SAFE
    #define FFTW_PLAN_SF() fftwf_make_planner_thread_safe()
  #else
    #define FFTW_PLAN_SF()
  #endif
#else
  typedef fftw_complex FFTW_CPX;           // double-prec has fftw_*
  typedef fftw_plan FFTW_PLAN;
  #define FFTW_INIT fftw_init_threads
  #define FFTW_PLAN_TH fftw_plan_with_nthreads
  #define FFTW_ALLOC_RE fftw_alloc_real
  #define FFTW_ALLOC_CPX fftw_alloc_complex
  #define FFTW_PLAN_MANY_DFT fftw_plan_many_dft
  #define FFTW_EXECUTE fftw_execute
  #define FFTW_DESTROY_PLAN fftw_destroy_plan
  #define FFTW_FREE fftw_free
  #define FFTW_FORGET_WISDOM fftw_forget_wisdom
  #define FFTW_CLEANUP fftw_cleanup
  #define FFTW_CLEANUP_THREADS fftw_cleanup_threads
  #ifdef FFTW_PLAN_SAFE
    #define FFTW_PLAN_SF() fftw_make_planner_thread_safe()
  #else
    #define FFTW_PLAN_SF()
  #endif
#endif

#endif // TENSORFLOW_NUFFT_KERNELS_FINUFFT_FFTW_DEFINITIONS_H