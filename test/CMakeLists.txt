add_executable(testbeast testbeast.cpp)
target_link_libraries(testbeast $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeast COMMAND testbeast)

# A host that marshals config calls to the fcitx thread, like fcitx5-macos. It
# covers the config write that restarts the server the request arrived on.
add_executable(testbeastmt testbeastmt.cpp)
target_link_libraries(testbeastmt $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeastmt COMMAND testbeastmt)

# Each test moves the server between endpoints on the default socket, so they
# must not run at the same time. Each gives itself a config directory, so a run
# never reads or writes the configuration of whoever started it.
foreach(test testbeast testbeastmt)
    set_tests_properties(${test} PROPERTIES RESOURCE_LOCK beast_listener)
endforeach()
