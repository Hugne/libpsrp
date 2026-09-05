cmake_minimum_required(VERSION 3.20)
project(libpsrp VERSION 0.1.0 LANGUAGES C)

# [MS-PSRP] client implementation. Client side only; server sections are out
# of scope by design (see PLAN.md).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PSRP_WERROR "Treat warnings as errors" ON)
option(PSRP_BUILD_TESTS "Build the test suite" ON)

# Warnings are part of the contract: both toolchains stay green.
add_library(psrp_flags INTERFACE)
if(MSVC)
  target_compile_options(psrp_flags INTERFACE /W4 /permissive-)
  if(PSRP_WERROR)
    target_compile_options(psrp_flags INTERFACE /WX)
  endif()
  # We use the standard C library deliberately; silence the MS-only nags.
  target_compile_definitions(psrp_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(psrp_flags INTERFACE
    -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith)
  if(PSRP_WERROR)
    target_compile_options(psrp_flags INTERFACE -Werror)
  endif()
endif()

# The protocol itself is platform-free: state machine, CLIXML, fragmentation,
# messages, the object model. Everything that touches an OS sits in the
# per-platform block below, behind psrp_xml.h, psrp_crypto.h and
# psrp_transport.h. See TODO PSRP-03 / PSRP-05.
add_library(psrp STATIC
  src/core/error.c
  src/core/buffer.c
  src/core/guid.c
  src/core/codec.c
  src/proto/fragment.c
  src/proto/defrag.c
  src/proto/message.c
  src/proto/clixml_str.c
  src/proto/clixml_write.c
  src/proto/object.c
  src/proto/object_complex.c
  src/proto/clixml_read.c
  src/proto/messages.c
  src/proto/messages_pipeline.c
  src/proto/records.c
  src/state/session.c
  src/proto/messages_pool.c
  src/state/host.c
  src/state/hostdata.c
  src/proto/metadata.c
  src/proto/primdict.c
  src/proto/timezone.c
  src/proto/invocation.c
  src/proto/keyexchange_msg.c
  src/state/hostvalues.c
  src/state/hostparams.c
)
add_library(psrp::psrp ALIAS psrp)

target_include_directories(psrp
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(psrp PRIVATE psrp_flags)

if(WIN32)
  # XmlLite is the XML backend (Microsoft standard; ships with Windows and is
  # present in both the Windows SDK and llvm-mingw, so nothing is vendored).
  # ole32/oleaut32 are for the WSMan automation interface, which is the only
  # way to enumerate shells (3.1.4.10.1); the flat WSMan C API has none.
  target_sources(psrp PRIVATE
    src/xml/xml_xmllite.c
    src/platform/random_win.c
    src/crypto/crypto_cng.c
    src/transport/transport_wsman.c
    src/transport/enumerate_wsman.c
    src/client/client.c
  )
  target_link_libraries(psrp PUBLIC xmllite bcrypt WsmSvc ole32 oleaut32)
  set(PSRP_HAVE_TRANSPORT ON CACHE INTERNAL "")
else()
  # Non-Windows is a partial port, deliberately: the protocol core and the XML
  # and RNG backends build and are tested, and that is what proves the seams
  # are real rather than aspirational. Crypto and transport are not ported
  # yet, so psrp_client and the WSMan transport are absent here -- a build
  # that omits them is honest, where stubs returning UNSUPPORTED would later
  # be mistaken for support. See TODO PSRP-03.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
  target_sources(psrp PRIVATE
    src/xml/xml_libxml2.c
    src/platform/random_posix.c
    src/crypto/crypto_none.c
  )
  target_link_libraries(psrp PUBLIC PkgConfig::LIBXML2)
  set(PSRP_HAVE_TRANSPORT OFF CACHE INTERNAL "")
endif()

if(PSRP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  # Built alongside the tests so they cannot silently rot, but they are not
  # tests: none of them runs without a server. They need the transport.
  if(PSRP_HAVE_TRANSPORT)
    add_subdirectory(examples)
  endif()
endif()
