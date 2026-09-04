# In-repo test harness (no external dependencies).
add_library(psrp_testharness STATIC harness/psrp_test.c)
target_include_directories(psrp_testharness PUBLIC harness)
target_link_libraries(psrp_testharness PRIVATE psrp_flags)

# psrp_add_unit_test(<name> <sources...>)
#
# Builds one executable per test file and registers it with CTest. Tests link
# the library's private include path too, so they can exercise internal
# headers (e.g. src/internal/psrp_codec.h) as well as the public API.
function(psrp_add_unit_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE psrp psrp_testharness psrp_flags)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME unit.${name} COMMAND ${name})
  set_tests_properties(unit.${name} PROPERTIES LABELS "unit" TIMEOUT 60)
endfunction()

psrp_add_unit_test(test_error  unit/test_error.c)
psrp_add_unit_test(test_buffer unit/test_buffer.c)
psrp_add_unit_test(test_guid   unit/test_guid.c)
psrp_add_unit_test(test_codec  unit/test_codec.c)
psrp_add_unit_test(test_fragment unit/test_fragment.c)
psrp_add_unit_test(test_message  unit/test_message.c)
psrp_add_unit_test(test_clixml_str unit/test_clixml_str.c)
psrp_add_unit_test(test_clixml_write unit/test_clixml_write.c)
psrp_add_unit_test(test_clixml_read unit/test_clixml_read.c)
psrp_add_unit_test(test_messages unit/test_messages.c)
psrp_add_unit_test(test_messages_pipeline unit/test_messages_pipeline.c)
psrp_add_unit_test(test_records unit/test_records.c)
psrp_add_unit_test(test_session unit/test_session.c)
psrp_add_unit_test(test_messages_pool unit/test_messages_pool.c)
psrp_add_unit_test(test_host unit/test_host.c)
psrp_add_unit_test(test_crypto unit/test_crypto.c)
psrp_add_unit_test(test_hostdata unit/test_hostdata.c)
psrp_add_unit_test(test_metadata unit/test_metadata.c)
psrp_add_unit_test(test_hostvalues unit/test_hostvalues.c)
psrp_add_unit_test(test_hostparams unit/test_hostparams.c)
psrp_add_unit_test(test_timezone unit/test_timezone.c)
psrp_add_unit_test(test_invocation unit/test_invocation.c)
psrp_add_unit_test(test_enumerate unit/test_enumerate.c)
psrp_add_unit_test(test_differential unit/test_differential.c)
target_include_directories(test_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

# Live interop test. Labelled so it can be selected or excluded; it skips
# itself unless PSRP_INTEROP=1, so the default run needs no WinRM.
add_executable(test_live interop/test_live.c)
target_link_libraries(test_live PRIVATE psrp psrp_flags)
target_include_directories(test_live PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_live COMMAND test_live)
set_tests_properties(interop.test_live PROPERTIES LABELS "interop" TIMEOUT 180)

# Fuzz target. Labelled separately so a soak run can select it:
#   ctest -L fuzz
# PSRP_FUZZ_ITERATIONS and PSRP_FUZZ_SEED steer it; the defaults keep the
# normal run to about a second while still covering every parser.
add_executable(fuzz_parsers fuzz/fuzz_parsers.c)
target_link_libraries(fuzz_parsers PRIVATE psrp psrp_testharness psrp_flags)
target_include_directories(fuzz_parsers PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fuzz.parsers COMMAND fuzz_parsers)
set_tests_properties(fuzz.parsers PROPERTIES LABELS "fuzz" TIMEOUT 300)
