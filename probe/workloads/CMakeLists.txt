# RACER kernels, the router proto, and preempt_bench.
#
# racer_kernels and racer_router_proto are compiled through the probe plugin,
# as protobuf and ipp-crypto are in the superbuild. preempt_bench is the
# application: it is not probed, and it gives the strong definitions of
# preempt_signal and do_yield.
cmake_minimum_required(VERSION 3.20)
project(RacerProbeWorkloads LANGUAGES C CXX ASM)

find_package(Threads REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# -Xclang -load loads the plugin before clang parses -mllvm options, so that
# -racer-probe-interval is a known option; -fpass-plugin adds the pass to the
# pipeline.
if(RACER_PROBE_INTERVAL STREQUAL "off")
  set(RACER_PROBE_COMPILE_OPTIONS "")
else()
  set(RACER_PROBE_COMPILE_OPTIONS
    "-fpass-plugin=${RACER_PROBE_PLUGIN}"
    "SHELL:-Xclang -load -Xclang ${RACER_PROBE_PLUGIN}"
    "SHELL:-mllvm -racer-probe-interval=${RACER_PROBE_INTERVAL}")
endif()

# Compiles every source of the target through the plugin, and again when the
# plugin changes.
function(racer_probe_all_sources target)
  if(RACER_PROBE_INTERVAL STREQUAL "off")
    return()
  endif()
  target_compile_options(${target} PRIVATE ${RACER_PROBE_COMPILE_OPTIONS})
  get_target_property(target_sources ${target} SOURCES)
  set_source_files_properties(${target_sources} TARGET_DIRECTORY ${target}
                              PROPERTIES OBJECT_DEPENDS "${RACER_PROBE_PLUGIN}")
endfunction()

add_library(ippcp STATIC IMPORTED)
set_target_properties(ippcp PROPERTIES
  IMPORTED_LOCATION "${IPPCP_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${IPPCP_INCLUDE_DIR}")

# The RACER kernels with the flags of the RACER Makefile (-O3 -march=native),
# and PERF, which compiles the logging out. Without PERF, a printf call (not
# taken at run time) stays in the loops of gather and dotproduct, and the pass
# does not unroll a loop with a call.
set(RACER_KERNEL_NAMES ch3_hash dotproduct gather matmul_histogram sequential_writer)
list(TRANSFORM RACER_KERNEL_NAMES PREPEND "${RACER_ROOT}/src/kernels/"
     OUTPUT_VARIABLE RACER_KERNEL_SOURCES)
list(TRANSFORM RACER_KERNEL_SOURCES APPEND ".cpp")
add_library(racer_kernels STATIC ${RACER_KERNEL_SOURCES} aes_gcm_chunked.cpp)
target_include_directories(racer_kernels PUBLIC "${RACER_ROOT}/include"
                                                "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(racer_kernels PRIVATE PERF)
target_compile_options(racer_kernels PRIVATE -march=native -Wno-unused-value)
target_link_libraries(racer_kernels PUBLIC ippcp)
racer_probe_all_sources(racer_kernels)

# RACER-Old probed the generated parser of the router message too.
add_library(racer_router_proto STATIC)
protobuf_generate(TARGET racer_router_proto LANGUAGE cpp
  PROTOS "${RACER_ROOT}/src/protobuf/router.proto"
  IMPORT_DIRS "${RACER_ROOT}/src/protobuf"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(racer_router_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(racer_router_proto PUBLIC protobuf::libprotobuf)
racer_probe_all_sources(racer_router_proto)

add_executable(preempt_bench
  preempt_bench.cpp
  "${RACER_ROOT}/src/jump_x86_64_sysv_elf_gas.S"
  "${RACER_ROOT}/src/make_x86_64_sysv_elf_gas.S"
  "${RACER_ROOT}/src/ontop_x86_64_sysv_elf_gas.S")
target_include_directories(preempt_bench PRIVATE "${RACER_PROBE_RUNTIME_INCLUDE_DIR}")
target_compile_definitions(preempt_bench PRIVATE
  RACER_PROBE_INTERVAL="${RACER_PROBE_INTERVAL}")
target_compile_options(preempt_bench PRIVATE -march=native)
target_link_libraries(preempt_bench PRIVATE racer_kernels racer_router_proto Threads::Threads)

install(TARGETS preempt_bench racer_kernels racer_router_proto)
