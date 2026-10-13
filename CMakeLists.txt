cmake_minimum_required(VERSION 3.20)
project(Moneta VERSION 0.1.0 LANGUAGES CXX)

add_library(moneta
    src/core/money.cpp
    src/expenses/expense_tree.cpp
    src/expenses/expense_queries.cpp
    src/expenses/tag_registry.cpp
    src/expenses/expense_history.cpp
    src/storage/expense_store.cpp
)
target_include_directories(moneta PUBLIC include)
target_compile_features(moneta PUBLIC cxx_std_20)
set_target_properties(moneta PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(moneta PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(expense_tree_tests tests/unit/expense_tree_tests.cpp)
    target_link_libraries(expense_tree_tests PRIVATE moneta)
    add_test(NAME expense_tree_tests COMMAND expense_tree_tests)
    add_executable(tag_registry_tests tests/unit/tag_registry_tests.cpp)
    target_link_libraries(tag_registry_tests PRIVATE moneta)
    add_test(NAME tag_registry_tests COMMAND tag_registry_tests)
    add_executable(expense_store_tests tests/unit/expense_store_tests.cpp)
    target_link_libraries(expense_store_tests PRIVATE moneta)
    add_test(NAME expense_store_tests COMMAND expense_store_tests)
endif()

option(MONETA_BUILD_BENCHMARKS "Build the optional expense-tree benchmark" OFF)
if(MONETA_BUILD_BENCHMARKS)
    add_executable(expense_tree_benchmark benchmarks/expense_tree_benchmark.cpp)
    target_link_libraries(expense_tree_benchmark PRIVATE moneta)
endif()
