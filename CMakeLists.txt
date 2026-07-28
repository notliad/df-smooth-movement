dfhack_plugin(smooth-movement smooth-movement.cpp LINK_LIBRARIES lua)
target_include_directories(smooth-movement PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(smooth-movement PRIVATE cxx_std_17)

add_executable(
    smooth-movement-test
    EXCLUDE_FROM_ALL
    test_visual_animation_manager.cpp)
target_compile_features(smooth-movement-test PRIVATE cxx_std_17)
