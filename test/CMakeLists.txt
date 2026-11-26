add_executable(testbeast testbeast.cpp)
target_link_libraries(testbeast $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeast COMMAND testbeast)

# Covers the config write that restarts the server answering it, which needs a
# host that marshals the write to the fcitx thread, as fcitx5-macos does.
add_executable(testbeastmt testbeastmt.cpp)
target_link_libraries(testbeastmt $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeastmt COMMAND testbeastmt)

# Each test owns its config directory, socket and ports, so only the timing is
# shared; they run one at a time to keep it out of each other's way.
foreach(test testbeast testbeastmt)
    set_tests_properties(${test} PROPERTIES RESOURCE_LOCK beast_listener)
endforeach()
