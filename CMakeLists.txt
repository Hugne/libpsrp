cmake_minimum_required(VERSION 3.20)
project(libpsrp VERSION 0.1.0 LANGUAGES C)

# [MS-PSRP] client implementation. Client side only; server sections are out
# of scope by design (see PLAN.md).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PSRP_WERROR "Treat warnings as errors" ON)
option(PSRP_BUILD_TESTS "Build the test suite" ON)

# Static linking is offered per library rather than as one switch, because the
# two are not equally available: libxml2 ships a usable archive nearly
# everywhere, while a static libcrypto depends on the distribution having
# packaged archives for OpenSSL's own private dependencies, which Ubuntu does
# not. One combined flag would have made the easy half unavailable because of
# the hard half. Both are non-Windows only; on Windows the XML and crypto
# backends are OS components with nothing to link statically.
option(PSRP_STATIC_LIBXML2 "Link libxml2 from its static archive" OFF)
option(PSRP_STATIC_OPENSSL "Link libcrypto from its static archive" OFF)

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
  src/transport/psrp_over_winrm.c
  src/transport/winrm_shell_info.c
  src/crypto/crypto_shared.c
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
    src/transport/winrm_wsman.c
    src/transport/winrm_enumerate.c
    src/client/client.c
  )
  target_link_libraries(psrp PUBLIC xmllite bcrypt WsmSvc ole32 oleaut32)
  set(PSRP_HAVE_TRANSPORT ON CACHE INTERNAL "")
else()
  # Non-Windows is a partial port: the protocol core, XML, RNG and crypto all
  # build and are tested here. What is missing is the transport, so
  # psrp_client and the WSMan transport are absent rather than stubbed -- a
  # build that omits them is honest, where stubs returning UNSUPPORTED would
  # later be mistaken for support. See TODO PSRP-03.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
  pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET libcrypto)
  pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
  pkg_check_modules(GSSAPI REQUIRED IMPORTED_TARGET krb5-gssapi)
  target_sources(psrp PRIVATE
    src/xml/xml_libxml2.c
    src/platform/random_posix.c
    src/crypto/crypto_openssl.c
    src/transport/winrm_curl.c
    src/client/client.c
  )

  # The archives are linked by absolute path rather than by asking pkg-config
  # for a static link line. Naming a library with -l leaves the choice to the
  # linker, which prefers the .so sitting next to the .a, so a build asking
  # for static would quietly get dynamic. A path cannot be misread.
  #
  # An archive also records none of its own dependencies, so what the shared
  # build picks up transitively has to be named here: libm for libxml2's
  # schema types (floor, trunc), and zlib and zstd for the compression both
  # libraries can use.
  if(PSRP_STATIC_LIBXML2)
    find_library(LIBXML2_ARCHIVE NAMES libxml2.a REQUIRED)
    target_link_libraries(psrp PUBLIC ${LIBXML2_ARCHIVE} m z)
    target_include_directories(psrp SYSTEM PRIVATE ${LIBXML2_INCLUDE_DIRS})
  else()
    target_link_libraries(psrp PUBLIC PkgConfig::LIBXML2)
  endif()

  if(PSRP_STATIC_OPENSSL)
    find_library(OPENSSL_ARCHIVE NAMES libcrypto.a REQUIRED)
    set(PSRP_OPENSSL_STATIC_LIBS ${OPENSSL_ARCHIVE} z zstd dl pthread)

    # Whether that archive can actually be linked depends on the packaging of
    # OpenSSL's private dependencies, and the usual failure is a wall of
    # undefined jent_* symbols at link time because the distribution built
    # OpenSSL against jitterentropy without shipping libjitterentropy.a.
    # Finding that out at configure time, with a name attached, beats finding
    # it out in the middle of a build.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES ${PSRP_OPENSSL_STATIC_LIBS})
    check_c_source_compiles(
      "#include <openssl/evp.h>
int main(void){return EVP_aes_256_cbc()?0:1;}"
      PSRP_STATIC_OPENSSL_LINKS)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(NOT PSRP_STATIC_OPENSSL_LINKS)
      message(FATAL_ERROR
        "PSRP_STATIC_OPENSSL=ON but ${OPENSSL_ARCHIVE} will not link. "
        "OpenSSL's own private dependencies are probably missing their static "
        "archives -- on Debian and Ubuntu libjitterentropy.a is not packaged "
        "at all, which makes a static libcrypto unbuildable there. Use "
        "PSRP_STATIC_LIBXML2 alone, or build against an OpenSSL whose "
        "dependencies ship archives.")
    endif()
    target_link_libraries(psrp PUBLIC ${PSRP_OPENSSL_STATIC_LIBS})
    target_include_directories(psrp SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIRS})
  else()
    target_link_libraries(psrp PUBLIC PkgConfig::OPENSSL)
  endif()

  # No static options for these two yet: a static libcurl needs the same
  # jitterentropy archive Debian does not package, and Kerberos ships no
  # archives at all there. See TODO PSRP-03.
  target_link_libraries(psrp PUBLIC PkgConfig::CURL PkgConfig::GSSAPI)
  set(PSRP_HAVE_TRANSPORT ON CACHE INTERNAL "")
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
