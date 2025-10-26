cmake_minimum_required(VERSION 3.21)
project(vm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(SRC_DIR "src")
set(INCLUDE_DIR "include")
set(TEST_DIR "test")

# OPTION 1: Use GLOB_RECURSE (simpler but requires re-running cmake after adding files)
file(GLOB_RECURSE SRC_FILES "${SRC_DIR}/*.cpp")

# OPTION 2: Explicit listing (more reliable, recommended for production)
# Uncomment the following and comment out the GLOB_RECURSE line above:
# set(SRC_FILES
#     ${SRC_DIR}/main.cpp
#     ${SRC_DIR}/assembler/assembler.cpp
#     ${SRC_DIR}/vm/vm_base.cpp
#     ${SRC_DIR}/vm/rvss/rvss_vm.cpp
#     ${SRC_DIR}/vm/rvss/rvss_pipeline_vm.cpp
#     ${SRC_DIR}/vm/rvss/rvss_control_unit.cpp
#     ${SRC_DIR}/memory_controller.cpp
#     ${SRC_DIR}/registers.cpp
#     ${SRC_DIR}/alu.cpp
#     ${SRC_DIR}/utils.cpp
#     ${SRC_DIR}/globals.cpp
#     ${SRC_DIR}/config.cpp
#     ${SRC_DIR}/command_handler.cpp
#     # Add other source files here
# )

file(GLOB_RECURSE TEST_FILES "${TEST_DIR}/*.cpp")

# Print found source files for debugging
message(STATUS "Source files found:")
foreach(src_file ${SRC_FILES})
    message(STATUS "  ${src_file}")
endforeach()

add_executable(${PROJECT_NAME} ${SRC_FILES})
target_include_directories(${PROJECT_NAME} PRIVATE ${INCLUDE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE 
    -Wall 
    -Wextra 
    -pedantic 
    -frounding-math 
    -ffloat-store 
    -g 
    -O3
)
target_link_libraries(${PROJECT_NAME} PRIVATE m)

if(ENABLE_ASAN)
    message(STATUS "ASAN enabled: Adding AddressSanitizer flags to main target")
    target_compile_options(${PROJECT_NAME} PRIVATE 
        -fsanitize=address 
        -fsanitize=undefined 
        -fno-omit-frame-pointer
    )
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address)
    set(ASAN_ENV "ASAN_OPTIONS=detect_leaks=1:leak_check_at_exit=1")
    add_custom_target(run_asan
        COMMAND ${CMAKE_COMMAND} -E env ${ASAN_ENV} ./${PROJECT_NAME}
        DEPENDS ${PROJECT_NAME}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running ${PROJECT_NAME} with ASAN"
    )
endif()

# Tests
option(ENABLE_TESTS "Build tests" OFF)
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})
    
    # Remove main.cpp from test build
    list(REMOVE_ITEM SRC_FILES "${CMAKE_SOURCE_DIR}/src/main.cpp")
    
    add_executable(tests ${SRC_FILES} ${TEST_FILES})
    target_include_directories(tests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(tests GTest::GTest GTest::Main pthread m)
    
    add_custom_target(test_run
        COMMAND ./tests
        DEPENDS tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

add_custom_target(run
    COMMAND ${PROJECT_NAME}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(debug
    COMMAND gdb ${PROJECT_NAME}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(valgrind
    COMMAND valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./${PROJECT_NAME}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)



find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating documentation with Doxygen"
    )
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Done")
