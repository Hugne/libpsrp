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

# Live interop test. Labelled so it can be selected or excluded; it skips
# itself unless PSRP_INTEROP=1, so the default run needs no WinRM.
add_executable(test_live interop/test_live.c)
target_link_libraries(test_live PRIVATE psrp psrp_flags)
target_include_directories(test_live PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_live COMMAND test_live)
set_tests_properties(interop.test_live PROPERTIES LABELS "interop" TIMEOUT 180)
