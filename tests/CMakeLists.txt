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
psrp_add_unit_test(test_hostdata unit/test_hostdata.c)
psrp_add_unit_test(test_metadata unit/test_metadata.c)
psrp_add_unit_test(test_hostvalues unit/test_hostvalues.c)
psrp_add_unit_test(test_hostparams unit/test_hostparams.c)
psrp_add_unit_test(test_timezone unit/test_timezone.c)
psrp_add_unit_test(test_invocation unit/test_invocation.c)

# The crypto tests run on both backends: CNG on Windows, OpenSSL elsewhere.
# Running them against both is the point -- they pin the wire formats where a
# reasonable-looking choice would fail to interoperate silently.
psrp_add_unit_test(test_crypto unit/test_crypto.c)

# Shell enumeration goes through the WSMan COM automation interface and has no
# POSIX counterpart; WS-Enumerate over curl would be a separate piece of work.
if(WIN32)
  psrp_add_unit_test(test_enumerate unit/test_enumerate.c)
endif()
psrp_add_unit_test(test_differential unit/test_differential.c)
target_include_directories(test_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

# The interop suites drive a full session, which the curl transport cannot do
# until run_command and send land (TODO PSRP-35). They are Windows-only for
# now rather than registered and failing.
if(WIN32)

# Live interop test. Labelled so it can be selected or excluded; it skips
# itself unless PSRP_INTEROP=1, so the default run needs no WinRM.
add_executable(test_live interop/test_live.c)
target_link_libraries(test_live PRIVATE psrp psrp_flags)
target_include_directories(test_live PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_live COMMAND test_live)
set_tests_properties(interop.test_live PROPERTIES LABELS "interop" TIMEOUT 180)

# Pipeline input against a live server. The input direction was implemented
# and unit-tested but never proven end to end until this existed.
add_executable(test_input interop/test_input.c)
target_link_libraries(test_input PRIVATE psrp psrp_flags)
target_include_directories(test_input PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_input COMMAND test_input)
set_tests_properties(interop.test_input PROPERTIES LABELS "interop" TIMEOUT 600)

# Feature coverage against a live server: the streams, runspace controls, host
# calls and key exchange, none of which had ever been spoken to a real server.
add_executable(test_features interop/test_features.c)
target_link_libraries(test_features PRIVATE psrp psrp_flags)
target_include_directories(test_features PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_features COMMAND test_features)
set_tests_properties(interop.test_features PROPERTIES LABELS "interop" TIMEOUT 600)

# Lifecycle stress test. Also interop-labelled and also opt-in: it needs a
# WinRM service, and it is the only test that can see defects which only appear
# across repeated cycles.
add_executable(test_stress interop/test_stress.c)
target_link_libraries(test_stress PRIVATE psrp psrp_flags)
target_link_libraries(test_stress PRIVATE psapi)
target_include_directories(test_stress PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_stress COMMAND test_stress)
set_tests_properties(interop.test_stress PROPERTIES LABELS "interop" TIMEOUT 600)

# The convenience layer against a live server. Written the way a caller
# writes it -- no pump loop -- so it also fails if that promise stops holding.
add_executable(test_client interop/test_client.c)
target_link_libraries(test_client PRIVATE psrp psrp_flags)
target_include_directories(test_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_client COMMAND test_client)
set_tests_properties(interop.test_client PROPERTIES LABELS "interop" TIMEOUT 600)

# The original stdin lab's binary sweep, ported onto PSRP pipeline input.
# Proves arbitrary bytes survive CLIXML <BA>, fragmentation and reassembly.
add_executable(test_binary_roundtrip interop/test_binary_roundtrip.c)
target_link_libraries(test_binary_roundtrip PRIVATE psrp psrp_flags bcrypt)
target_include_directories(test_binary_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME interop.test_binary_roundtrip COMMAND test_binary_roundtrip)
set_tests_properties(interop.test_binary_roundtrip PROPERTIES LABELS "interop" TIMEOUT 600)

endif()  # WIN32: interop suites

# The curl transport's own live test. What it can do -- authenticate, encrypt,
# create a shell, negotiate -- is exactly what was in doubt, so it is worth a
# test of its own rather than waiting for the full suites to become portable.
if(NOT WIN32)
  # Kerberos against a real KDC. Skips itself unless PSRP_CONNECTION names an
  # endpoint by FQDN, since a service ticket is issued for HTTP/<fqdn> and
  # connecting by address asks the KDC for a principal it has never heard of.
  add_executable(test_kerberos interop/test_kerberos.c)
  target_link_libraries(test_kerberos PRIVATE psrp psrp_flags)
  target_include_directories(test_kerberos PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME interop.test_kerberos COMMAND test_kerberos)
  set_tests_properties(interop.test_kerberos PROPERTIES LABELS "interop" TIMEOUT 300)

  # Adopting a disconnected pool: the second half of MS-WSMV's disconnected
  # shells, and the only exercise winrm_connect gets on this platform.
  add_executable(test_curl_connect interop/test_curl_connect.c)
  target_link_libraries(test_curl_connect PRIVATE psrp psrp_flags)
  target_include_directories(test_curl_connect PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME interop.test_curl_connect COMMAND test_curl_connect)
  set_tests_properties(interop.test_curl_connect
                       PROPERTIES LABELS "interop" TIMEOUT 300)

  # Stopping a running pipeline: the signal path had no live coverage on this
  # platform at all, which is what TODO PSRP-38 was waiting for.
  add_executable(test_curl_stop interop/test_curl_stop.c)
  target_link_libraries(test_curl_stop PRIVATE psrp psrp_flags)
  target_include_directories(test_curl_stop PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME interop.test_curl_stop COMMAND test_curl_stop)
  set_tests_properties(interop.test_curl_stop
                       PROPERTIES LABELS "interop" TIMEOUT 300)

  add_executable(test_curl_open interop/test_curl_open.c)
  target_link_libraries(test_curl_open PRIVATE psrp psrp_flags)
  target_include_directories(test_curl_open PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME interop.test_curl_open COMMAND test_curl_open)
  set_tests_properties(interop.test_curl_open PROPERTIES LABELS "interop" TIMEOUT 300)
endif()

# Public header self-containment.
#
# Every header must compile on its own, without another one having been
# included first. That property breaks the moment somebody uses a type without
# including where it comes from, and it breaks for consumers rather than for
# us, because our own translation units always include several headers
# together and mask it.
#
# One translation unit is generated per header, including only that header.
# They are compiled, not linked: a successful compile is the whole assertion,
# so this is a build-time check rather than a ctest.
file(GLOB PSRP_PUBLIC_HEADERS
     RELATIVE ${CMAKE_SOURCE_DIR}/include/psrp
     ${CMAKE_SOURCE_DIR}/include/psrp/*.h)

set(PSRP_HEADER_TUS "")
foreach(hdr ${PSRP_PUBLIC_HEADERS})
  string(REPLACE ".h" "" base ${hdr})
  set(tu ${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_${base}.c)
  file(GENERATE OUTPUT ${tu}
       CONTENT "/* Generated. Asserts psrp/${hdr} is self-contained. */\n#include \"psrp/${hdr}\"\n")
  list(APPEND PSRP_HEADER_TUS ${tu})
endforeach()

add_library(test_headers OBJECT ${PSRP_HEADER_TUS})
target_link_libraries(test_headers PRIVATE psrp_flags)
target_include_directories(test_headers PRIVATE ${CMAKE_SOURCE_DIR}/include)

# The same check from C++, which is what the extern "C" guards are for. Those
# guards are in every header and nothing else exercises them, so without this
# they are decoration that might not work. Skipped when no C++ compiler is
# configured, since requiring one to build a C library would be rude.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 11)
  set(PSRP_HEADER_TUS_CXX "")
  foreach(hdr ${PSRP_PUBLIC_HEADERS})
    string(REPLACE ".h" "" base ${hdr})
    set(tu ${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_${base}.cpp)
    file(GENERATE OUTPUT ${tu}
         CONTENT "/* Generated. Asserts psrp/${hdr} is usable from C++. */\n#include \"psrp/${hdr}\"\n")
    list(APPEND PSRP_HEADER_TUS_CXX ${tu})
  endforeach()
  add_library(test_headers_cxx OBJECT ${PSRP_HEADER_TUS_CXX})
  target_include_directories(test_headers_cxx PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

# Fuzz target. Labelled separately so a soak run can select it:
#   ctest -L fuzz
# PSRP_FUZZ_ITERATIONS and PSRP_FUZZ_SEED steer it; the defaults keep the
# normal run to about a second while still covering every parser.
add_executable(fuzz_parsers fuzz/fuzz_parsers.c)
target_link_libraries(fuzz_parsers PRIVATE psrp psrp_testharness psrp_flags)
target_include_directories(fuzz_parsers PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fuzz.parsers COMMAND fuzz_parsers)
set_tests_properties(fuzz.parsers PROPERTIES LABELS "fuzz" TIMEOUT 300)
