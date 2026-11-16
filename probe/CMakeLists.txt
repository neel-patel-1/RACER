# Superbuild for probe-point accelerator preemption.
#
#   pass/       RacerProbePass, an LLVM 17 pass plugin that puts probe points
#               at loop headers
#   runtime/    weak default definitions of the probe symbols
#   third-party protobuf v28.3 (with its abseil) and ipp-crypto, compiled
#               through the plugin
#   workloads/  RACER kernels, the router proto, and preempt_bench
#
# One build directory holds one configuration:
#   -DRACER_PROBE_INTERVAL=200   probe about every 200 IR instructions
#   -DRACER_PROBE_INTERVAL=0     probe every loop iteration (no unrolling)
#   -DRACER_PROBE_INTERVAL=off   same libraries without probes (baseline)
#
# The third-party builds do not track the plugin. After a change to the pass,
# build into a new build directory.
cmake_minimum_required(VERSION 3.20)
project(RacerProbe LANGUAGES C CXX)

include(ExternalProject)

find_package(LLVM 17 REQUIRED CONFIG)
foreach(language C CXX)
  string(REGEX MATCH "^[0-9]+" compiler_major_version "${CMAKE_${language}_COMPILER_VERSION}")
  if(NOT CMAKE_${language}_COMPILER_ID STREQUAL "Clang"
     OR NOT compiler_major_version EQUAL LLVM_VERSION_MAJOR)
    message(FATAL_ERROR
      "The ${language} compiler must be clang ${LLVM_VERSION_MAJOR}, the LLVM "
      "the plugin is built against. Configure with "
      "-DCMAKE_C_COMPILER=clang-${LLVM_VERSION_MAJOR} "
      "-DCMAKE_CXX_COMPILER=clang++-${LLVM_VERSION_MAJOR}.")
  endif()
endforeach()

set(RACER_PROBE_INTERVAL "200" CACHE STRING
    "IR instructions between probe points (0 = every loop iteration), or off for a build without probes")
if(RACER_PROBE_INTERVAL STREQUAL "off")
  set(RACER_PROBES_ENABLED OFF)
elseif(RACER_PROBE_INTERVAL MATCHES "^[0-9]+$")
  set(RACER_PROBES_ENABLED ON)
else()
  message(FATAL_ERROR "RACER_PROBE_INTERVAL must be a number or off, not '${RACER_PROBE_INTERVAL}'")
endif()
message(STATUS "RACER_PROBE_INTERVAL: ${RACER_PROBE_INTERVAL}")

get_filename_component(RACER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RACER_INSTALL_DIR "${CMAKE_BINARY_DIR}/install")

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
add_subdirectory(pass)
add_subdirectory(runtime)
set(RACER_PROBE_PLUGIN "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/RacerProbePass${CMAKE_SHARED_MODULE_SUFFIX}")
set(RACER_PROBE_RUNTIME "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/${CMAKE_STATIC_LIBRARY_PREFIX}racer_probe_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")

# CMAKE_C_FLAGS and CMAKE_CXX_FLAGS pass host flags (scripts/build.sh gives
# --gcc-install-dir) to every inner build.
set(RACER_COMMON_CMAKE_ARGS
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
  -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_STANDARD=17
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_INSTALL_PREFIX=${RACER_INSTALL_DIR}
  -DCMAKE_PREFIX_PATH=${RACER_INSTALL_DIR})

# Probed third-party builds compile through the launcher. The launcher runs no
# link step, so programs in those builds (protoc) get the probe symbols from
# the runtime library, given as a standard library of every link.
if(RACER_PROBES_ENABLED)
  set(RACER_PROBE_LAUNCHER "${CMAKE_BINARY_DIR}/racer-probe-compiler-launcher")
  configure_file(cmake/racer-probe-compiler-launcher.in "${RACER_PROBE_LAUNCHER}"
                 @ONLY FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                        GROUP_READ GROUP_EXECUTE
                                        WORLD_READ WORLD_EXECUTE)
  set(RACER_PROBED_CMAKE_ARGS
    -DCMAKE_C_COMPILER_LAUNCHER=${RACER_PROBE_LAUNCHER}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${RACER_PROBE_LAUNCHER}
    -DCMAKE_C_STANDARD_LIBRARIES=${RACER_PROBE_RUNTIME}
    -DCMAKE_CXX_STANDARD_LIBRARIES=${RACER_PROBE_RUNTIME})
  set(RACER_PROBE_DEPENDENCIES RacerProbePass racer_probe_runtime)
else()
  set(RACER_PROBED_CMAKE_ARGS "")
  set(RACER_PROBE_DEPENDENCIES "")
endif()

# abseil is not probed: protobuf parses with its own code, and abseil code it
# inlines from headers is compiled (and probed) inside protobuf.
ExternalProject_Add(abseil
  SOURCE_DIR "${RACER_ROOT}/third-party/protobuf/third_party/abseil-cpp"
  BINARY_DIR "${CMAKE_BINARY_DIR}/abseil"
  CMAKE_ARGS ${RACER_COMMON_CMAKE_ARGS}
             -DABSL_PROPAGATE_CXX_STD=ON
             -DABSL_BUILD_TESTING=OFF
             -DBUILD_TESTING=OFF)

ExternalProject_Add(protobuf
  SOURCE_DIR "${RACER_ROOT}/third-party/protobuf"
  BINARY_DIR "${CMAKE_BINARY_DIR}/protobuf"
  DEPENDS abseil ${RACER_PROBE_DEPENDENCIES}
  CMAKE_ARGS ${RACER_COMMON_CMAKE_ARGS} ${RACER_PROBED_CMAKE_ARGS}
             -Dprotobuf_ABSL_PROVIDER=package
             -Dprotobuf_BUILD_SHARED_LIBS=OFF
             -Dprotobuf_BUILD_TESTS=OFF
             -Dprotobuf_BUILD_LIBUPB=OFF
             -Dprotobuf_WITH_ZLIB=OFF)

# The merged build (one libippcp.a that dispatches to a per-CPU copy of each
# function) is the build RACER links. The launcher reaches every C file of
# every per-CPU copy; the assembly kernels (nasm) get no probes. ipp-crypto
# needs nasm 2.16.02 or newer; without a path it can take yasm, which fails.
set(IPPCP_BUILD_DIR "${CMAKE_BINARY_DIR}/ipp-crypto")
find_program(NASM_EXECUTABLE nasm REQUIRED)
ExternalProject_Add(ippcrypto
  SOURCE_DIR "${RACER_ROOT}/third-party/ipp-crypto"
  BINARY_DIR "${IPPCP_BUILD_DIR}"
  DEPENDS ${RACER_PROBE_DEPENDENCIES}
  CMAKE_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
             -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
             -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_ASM_NASM_COMPILER=${NASM_EXECUTABLE}
             -DARCH=intel64
             -DMERGED_BLD=ON
             ${RACER_PROBED_CMAKE_ARGS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ippcp_s
  INSTALL_COMMAND "")

ExternalProject_Add(workloads
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/workloads"
  BINARY_DIR "${CMAKE_BINARY_DIR}/workloads"
  DEPENDS protobuf ippcrypto RacerProbePass racer_probe_runtime
  BUILD_ALWAYS ON
  CMAKE_ARGS ${RACER_COMMON_CMAKE_ARGS}
             -DRACER_ROOT=${RACER_ROOT}
             -DRACER_PROBE_INTERVAL=${RACER_PROBE_INTERVAL}
             -DRACER_PROBE_PLUGIN=${RACER_PROBE_PLUGIN}
             -DRACER_PROBE_RUNTIME_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/runtime
             -DIPPCP_LIBRARY=${IPPCP_BUILD_DIR}/.build/RELEASE/lib/libippcp.a
             -DIPPCP_INCLUDE_DIR=${RACER_ROOT}/third-party/ipp-crypto/include)

enable_testing()
add_subdirectory(test)
