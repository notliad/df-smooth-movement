set(XPREDUX_SOURCES
	xpredux.cpp
	movement_feature.cpp
	camera_feature.cpp
	sprite_flip_feature.cpp)

dfhack_plugin(xpredux ${XPREDUX_SOURCES} LINK_LIBRARIES lua)
target_include_directories(xpredux PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(xpredux PRIVATE cxx_std_17)

add_executable(
	xpredux-animation-test
	EXCLUDE_FROM_ALL
	test_visual_animation_manager.cpp)
target_compile_features(xpredux-animation-test PRIVATE cxx_std_17)

add_executable(
	xpredux-camera-test
	EXCLUDE_FROM_ALL
	test_camera_feature.cpp
	camera_feature.cpp)
target_compile_features(xpredux-camera-test PRIVATE cxx_std_17)

add_executable(
	xpredux-sprite-flip-test
	EXCLUDE_FROM_ALL
	test_sprite_flip_feature.cpp
	sprite_flip_feature.cpp)
target_compile_features(xpredux-sprite-flip-test PRIVATE cxx_std_17)
