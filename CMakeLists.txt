# ======================================================================== #
# Copyright 2019-2020 The Collaborators                                    #
#                                                                          #
# Licensed under the Apache License, Version 2.0 (the "License");          #
# you may not use this file except in compliance with the License.         #
# You may obtain a copy of the License at                                  #
#                                                                          #
#     http://www.apache.org/licenses/LICENSE-2.0                           #
#                                                                          #
# Unless required by applicable law or agreed to in writing, software      #
# distributed under the License is distributed on an "AS IS" BASIS,        #
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. #
# See the License for the specific language governing permissions and      #
# limitations under the License.                                           #
# ======================================================================== #

cmake_minimum_required(VERSION 3.12)
cmake_policy(SET CMP0104 NEW)

project(cudaParticleAdvection
	VERSION 1.0
	DESCRIPTION "cuda Particle Advection with hardware accelerated tet query"
	LANGUAGES C CXX CUDA
)

set(OptiX_INSTALL_DIR "C:/ProgramData/NVIDIA Corporation/OptiX SDK 7.0.0")
set(OptiX_INCLUDE  "${OptiX_INSTALL_DIR}/include")
set(TBB_ROOT_PATH "E:/libs/oneapi-tbb-2021.5.0")

# set CUDA
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CUDA_LINK_LIBRARIES_KEYWORD PUBLIC)
set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
set(CMAKE_CUDA_ARCHITECTURES "86")

if (MSVC)
	list(APPEND CUDA_NVCC_FLAGS "-Xcompiler=-bigobj")
else()
	list(APPEND CUDA_NVCC_FLAGS "-Xcompiler=-mf16c")
	list(APPEND CUDA_NVCC_FLAGS "-Xcompiler=-Wno-float-conversion")
	list(APPEND CUDA_NVCC_FLAGS "-Xcompiler=-fno-strict-aliasing")
	list(APPEND CUDA_NVCC_FLAGS "-Xcompiler=-fPIC")
endif()
list(APPEND CUDA_NVCC_FLAGS "--extended-lambda")
list(APPEND CUDA_NVCC_FLAGS "--expt-relaxed-constexpr")

# owl
set(owl_dir ${PROJECT_SOURCE_DIR}/submodules/owl)
add_subdirectory(${owl_dir} EXCLUDE_FROM_ALL)
include_directories("${owl_dir}/owl/include")
								
# tet query lib
add_subdirectory(${PROJECT_SOURCE_DIR}/tetMeshQueryLib)

# test query with particle advection
add_executable(cudaParticleAdvection 
    cudaHelpers.cuh
    DeviceTetMesh.cuh
    HostTetMesh.cuh
    cudaThreadedParticleAdvection.cu
    cudaParticleAdvection.cu
    )
 
target_link_libraries(cudaParticleAdvection PUBLIC tetMeshQueryLib)



