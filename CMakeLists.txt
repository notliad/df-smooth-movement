set(STRATUM_SOURCES
	stratum.cpp
	movement_feature.cpp
	camera_feature.cpp
	sprite_flip_feature.cpp)

dfhack_plugin(stratum ${STRATUM_SOURCES} LINK_LIBRARIES lua)
target_include_directories(stratum PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(stratum PRIVATE cxx_std_17)

add_executable(
	stratum-animation-test
	EXCLUDE_FROM_ALL
	test_visual_animation_manager.cpp)
target_compile_features(stratum-animation-test PRIVATE cxx_std_17)

add_executable(
	stratum-camera-test
	EXCLUDE_FROM_ALL
	test_camera_feature.cpp
	camera_feature.cpp)
target_compile_features(stratum-camera-test PRIVATE cxx_std_17)

add_executable(
	stratum-sprite-flip-test
	EXCLUDE_FROM_ALL
	test_sprite_flip_feature.cpp
	sprite_flip_feature.cpp)
target_compile_features(stratum-sprite-flip-test PRIVATE cxx_std_17)
