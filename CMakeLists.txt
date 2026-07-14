cmake_minimum_required(VERSION 3.16)
project(Flux
    VERSION 0.1.0
    DESCRIPTION "Flux Programming Language"
    LANGUAGES C
)

# -------------------------------------------------------------------------
# Standards and options
# -------------------------------------------------------------------------
# Use explicit flags for C17 instead of cmake standard variables
# (cmake 3.19 does not know the C17 flag for GCC/Clang)
add_compile_options(-std=gnu17)

option(FLUX_DEBUG_BYTECODE "Print bytecode disassembly after compilation" OFF)
option(FLUX_DEBUG_GC       "Run GC on every allocation (stress test)"     OFF)
option(FLUX_DEBUG_TRACE    "Trace VM instruction execution"                OFF)
option(FLUX_ENABLE_ASAN    "Enable AddressSanitizer"                       OFF)

# -------------------------------------------------------------------------
# Compiler flags
# -------------------------------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
else()
    add_compile_options(-O2)
endif()

if(FLUX_ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# -------------------------------------------------------------------------
# Debug defines
# -------------------------------------------------------------------------
if(FLUX_DEBUG_BYTECODE)
    add_compile_definitions(FLUX_DEBUG_BYTECODE)
endif()
if(FLUX_DEBUG_GC)
    add_compile_definitions(FLUX_DEBUG_GC)
endif()
if(FLUX_DEBUG_TRACE)
    add_compile_definitions(FLUX_DEBUG_TRACE)
endif()

# -------------------------------------------------------------------------
# Include directories
# -------------------------------------------------------------------------
include_directories(include)

# -------------------------------------------------------------------------
# Source files
# -------------------------------------------------------------------------
set(FLUX_LIB_SOURCES
    src/util/util.c
    src/vm/chunk.c
    src/object/object.c
    src/gc/gc.c
    src/lexer/lexer.c
    src/ast/ast.c
    src/parser/parser.c
    src/compiler/compiler.c
    src/vm/vm.c
    src/runtime/runtime.c
    src/stdlib/stdlib.c
    src/api/api.c
)

# -------------------------------------------------------------------------
# Shared library  (libflux.so)
# -------------------------------------------------------------------------
add_library(flux_shared SHARED ${FLUX_LIB_SOURCES})
target_include_directories(flux_shared PUBLIC include)
set_target_properties(flux_shared PROPERTIES
    OUTPUT_NAME flux
    VERSION     ${PROJECT_VERSION}
    SOVERSION   0
)
target_link_libraries(flux_shared m)

# -------------------------------------------------------------------------
# Static library  (libflux.a)
# -------------------------------------------------------------------------
add_library(flux_static STATIC ${FLUX_LIB_SOURCES})
target_include_directories(flux_static PUBLIC include)
set_target_properties(flux_static PROPERTIES
    OUTPUT_NAME flux
)
target_link_libraries(flux_static m)

# -------------------------------------------------------------------------
# CLI executable  (flux)
# -------------------------------------------------------------------------
add_executable(flux src/main.c)
target_include_directories(flux PRIVATE include)
target_link_libraries(flux flux_static m)

# -------------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------------
enable_testing()

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_lexer.c")
    add_executable(test_lexer tests/test_lexer.c)
    target_include_directories(test_lexer PRIVATE include)
    target_link_libraries(test_lexer flux_static m)
    add_test(NAME lexer COMMAND test_lexer)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_vm.c")
    add_executable(test_vm tests/test_vm.c)
    target_include_directories(test_vm PRIVATE include)
    target_link_libraries(test_vm flux_static m)
    add_test(NAME vm COMMAND test_vm)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_gc.c")
    add_executable(test_gc tests/test_gc.c)
    target_include_directories(test_gc PRIVATE include)
    target_link_libraries(test_gc flux_static m)
    add_test(NAME gc COMMAND test_gc)
endif()

# -------------------------------------------------------------------------
# Install
# -------------------------------------------------------------------------
install(TARGETS flux         DESTINATION bin)
install(TARGETS flux_shared  DESTINATION lib)
install(TARGETS flux_static  DESTINATION lib)
install(DIRECTORY include/flux DESTINATION include)

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Flux ${PROJECT_VERSION} build configuration:")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C compiler     : ${CMAKE_C_COMPILER}")
message(STATUS "  Debug bytecode : ${FLUX_DEBUG_BYTECODE}")
message(STATUS "  Debug GC       : ${FLUX_DEBUG_GC}")
message(STATUS "  Debug trace    : ${FLUX_DEBUG_TRACE}")
message(STATUS "  ASan           : ${FLUX_ENABLE_ASAN}")
message(STATUS "")
