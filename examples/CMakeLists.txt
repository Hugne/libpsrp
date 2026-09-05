# Examples are built as part of the normal build so they cannot silently rot,
# but they are not tests: none of them runs without a server.
add_executable(example_run_command run_command.c)
target_link_libraries(example_run_command PRIVATE psrp psrp_flags)

# The same job through the convenience layer, for contrast.
add_executable(example_quick_run quick_run.c)
target_link_libraries(example_quick_run PRIVATE psrp psrp_flags)
