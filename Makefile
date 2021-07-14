CXX := g++
NVCC := nvcc
PYTHON_BIN_PATH = python

# Root directory.
ROOT_DIR := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

# Dependencies.
FINUFFT_DIR_CPU := third_party/finufft
FINUFFT_LIB_CPU = $(FINUFFT_DIR_CPU)/lib-static/libfinufft.a
FINUFFT_DIR_GPU := third_party/cufinufft
FINUFFT_LIB_GPU = $(FINUFFT_DIR_GPU)/lib-static/libcufinufft.a

NUFFT_SRCS_GPU = $(wildcard tensorflow_nufft/cc/kernels/*.cu.cc)
NUFFT_OBJS_GPU = $(patsubst %.cu.cc, %.cu.o, $(NUFFT_SRCS_GPU))
NUFFT_SRCS = $(filter-out $(NUFFT_SRCS_GPU), $(wildcard tensorflow_nufft/cc/kernels/*.cc)) $(wildcard tensorflow_nufft/cc/ops/*.cc)

TF_CFLAGS := $(shell $(PYTHON_BIN_PATH) -c 'import tensorflow as tf; print(" ".join(tf.sysconfig.get_compile_flags()))')
TF_LFLAGS := $(shell $(PYTHON_BIN_PATH) -c 'import tensorflow as tf; print(" ".join(tf.sysconfig.get_link_flags()))')

# GCC-specific compilation flags.
CCFLAGS = ${TF_CFLAGS} -fPIC -O2 -std=c++11
CCFLAGS += -I$(ROOT_DIR)/$(FINUFFT_DIR_CPU)/include
CCFLAGS += -I$(ROOT_DIR)/$(FINUFFT_DIR_GPU)/include
CCFLAGS += -DGOOGLE_CUDA=1
CCFLAGS += -I/usr/local/cuda/targets/x86_64-linux/include
CCFLAGS += -L/usr/local/cuda/targets/x86_64-linux/lib

# NVCC-specific compilation flags.
CUFLAGS = $(TF_CFLAGS) -std=c++11 -DGOOGLE_CUDA=1 -x cu -Xcompiler "-fPIC" -DNDEBUG --expt-relaxed-constexpr
CUFLAGS += -I$(ROOT_DIR)/$(FINUFFT_DIR_GPU)/include

# Include this Makefile's directory.
CCFLAGS += -I$(ROOT_DIR)
CUFLAGS += -I$(ROOT_DIR)

# Linker flags.
LDFLAGS = -shared ${TF_LFLAGS}

# Additional dynamic linking.
LDFLAGS += -lfftw3 -lfftw3_omp -lfftw3f -lfftw3f_omp
LDFLAGS += -lcudadevrt -lcudart -lnvToolsExt

# Additional static linking.
# LDFLAGS += $(FINUFFT_LIB_CPU)
# LDFLAGS += $(FINUFFT_LIB_GPU)

TARGET_LIB = tensorflow_nufft/python/ops/_nufft_ops.so
TARGET_LIB_GPU = tensorflow_nufft/python/ops/_nufft_ops.cu.o
# TARGET_OBJ_GPU = tensorflow_nufft/python/ops/_nufft_kernels.cu.o


# nufft op for CPU
op: $(TARGET_LIB)

$(TARGET_LIB): $(NUFFT_SRCS) $(TARGET_LIB_GPU) $(FINUFFT_LIB_CPU) $(FINUFFT_LIB_GPU)
	$(CXX) $(CCFLAGS) -o $@ $^ $(NUFFT_OBJS_GPU) ${LDFLAGS}

$(TARGET_LIB_GPU): $(NUFFT_SRCS_GPU)
# $(NVCC) -std=c++11 -c -o $@ $^  $(CUFLAGS)
	mkdir -p $(ROOT_DIR)/third_party/gpus/cuda
	cp -r /usr/local/cuda/include/ $(ROOT_DIR)/third_party/gpus/cuda/
	$(NVCC) -dc $^ $(CUFLAGS) -odir tensorflow_nufft/cc/kernels -Xcompiler "-fPIC" -lcudadevrt -lcudart
	$(NVCC) -dlink $(NUFFT_OBJS_GPU) $(FINUFFT_LIB_GPU) -o $(TARGET_LIB_GPU) -Xcompiler "-fPIC" -lcudadevrt -lcudart

# $(TARGET_LIB_GPU_LINK): $(TARGET_LIB_GPU)
# # $(NVCC) -std=c++11 -c -o $@ $^  $(CUFLAGS)
	
$(FINUFFT_LIB_CPU):
	$(MAKE) lib -C $(FINUFFT_DIR_CPU)

$(FINUFFT_LIB_GPU):
	$(MAKE) lib -C $(FINUFFT_DIR_GPU)

test: $(wildcard tensorflow_nufft/python/ops/*.py) $(TARGET_LIB)
	$(PYTHON_BIN_PATH) tensorflow_nufft/python/ops/nufft_ops_test.py

pip_pkg: $(TARGET_LIB)
	./build_pip_pkg.sh make artifacts

clean: mostlyclean
	$(MAKE) clean -C $(FINUFFT_DIR_CPU)
	$(MAKE) clean -C $(FINUFFT_DIR_GPU)

mostlyclean:
	rm -f $(TARGET_LIB)
	rm -f $(TARGET_LIB_GPU)
	rm -f $(NUFFT_OBJS_GPU)
# rm -f $(TARGET_LIB_GPU_LINK)
# rm -f $(TARGET_OBJ_GPU)

.PHONY: clean mostlyclean
