cmake_minimum_required(VERSION 3.20)
project(Moneta VERSION 0.1.0 LANGUAGES CXX)

add_library(moneta
    src/core/money.cpp
    src/expenses/expense_tree.cpp
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
endif()
