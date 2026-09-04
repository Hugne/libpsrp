# Examples are built as part of the normal build so they cannot silently rot,
# but they are not tests: none of them runs without a server.
add_executable(example_run_command run_command.c)
target_link_libraries(example_run_command PRIVATE psrp psrp_flags)
