# Copyright 2025 KVCache.AI
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# SHM Allocation Benchmark
add_executable(shm_allocation_bench shm_allocation_bench.cpp)
target_link_libraries(shm_allocation_bench
    PRIVATE
    gflags
    glog::glog
    rt  # Required for shm_open
)

# SHM Address Lookup Benchmark
add_executable(shm_address_lookup_bench shm_address_lookup_bench.cpp)
target_link_libraries(shm_address_lookup_bench
    PRIVATE
    gflags
    glog::glog
)

# SHM Transfer Benchmark
add_executable(shm_transfer_bench shm_transfer_bench.cpp)
target_link_libraries(shm_transfer_bench
    PRIVATE
    gflags
    glog::glog
    rt  # Required for shm_open
)

# Install benchmarks
install(TARGETS
    shm_allocation_bench
    shm_address_lookup_bench
    shm_transfer_bench
    RUNTIME DESTINATION bin
)
