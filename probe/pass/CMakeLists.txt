# The plugin is loaded into clang-17 and opt-17, which already hold LLVM, so it
# links no LLVM library.
add_library(RacerProbePass MODULE RacerProbePass.cpp)
target_include_directories(RacerProbePass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(RacerProbePass PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_features(RacerProbePass PRIVATE cxx_std_17)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(RacerProbePass PRIVATE -fno-rtti)
endif()
set_target_properties(RacerProbePass PROPERTIES PREFIX "" OUTPUT_NAME RacerProbePass)
