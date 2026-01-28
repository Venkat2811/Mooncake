# TENT Tests and Examples

# TENT Metrics Example
add_executable(tent_metrics_example tent_metrics_example.cpp)
target_link_libraries(tent_metrics_example PRIVATE tent_metrics tent_common glog ibverbs)
target_include_directories(tent_metrics_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# TENT Metrics Config Loader Unit Test
add_executable(metrics_config_loader_test metrics_config_loader_test.cpp)
target_link_libraries(metrics_config_loader_test PRIVATE tent_metrics tent_common gtest gtest_main glog)
target_include_directories(metrics_config_loader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME metrics_config_loader_test COMMAND metrics_config_loader_test)

# SHM Arena Test
add_executable(shm_arena_test shm_arena_test.cpp)
target_link_libraries(shm_arena_test PRIVATE tent gtest gtest_main glog rt)
target_include_directories(shm_arena_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME shm_arena_test COMMAND shm_arena_test)

