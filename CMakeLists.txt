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
  src/xml/xml_xmllite.c
  src/proto/clixml_read.c
  src/proto/messages.c
  src/proto/messages_pipeline.c
  src/proto/records.c
)
add_library(psrp::psrp ALIAS psrp)

target_include_directories(psrp
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(psrp PRIVATE psrp_flags)

# XmlLite is the XML backend (Microsoft standard; ships with Windows and is
# present in both the Windows SDK and llvm-mingw, so nothing is vendored).
target_link_libraries(psrp PUBLIC xmllite)

if(PSRP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
