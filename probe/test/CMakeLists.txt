# Pass tests: each pass/*.ll file holds "; RUN:" lines (opt + FileCheck), run
# by run-pass-test.sh. Build the plugin before running ctest.
file(GLOB RACER_PASS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pass/*.ll")
foreach(test_file ${RACER_PASS_TESTS})
  get_filename_component(test_name "${test_file}" NAME_WE)
  add_test(NAME pass.${test_name}
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-pass-test.sh"
                   "${test_file}" "${LLVM_TOOLS_BINARY_DIR}" "${RACER_PROBE_PLUGIN}")
endforeach()
