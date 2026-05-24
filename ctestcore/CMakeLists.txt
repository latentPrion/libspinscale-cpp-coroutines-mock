add_library(ctestcore STATIC
	componentThread.cpp
	harnessComponent.cpp
	body/body.cpp
	world/world.cpp
	leg/leg.cpp
	harness/harness.cpp
	marionette/main.cpp
	marionette/lifetime.cpp
	test/marionetteTestRunner.cpp
)

target_include_directories(ctestcore
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ctestcore PUBLIC spinscale)

target_compile_features(ctestcore PUBLIC cxx_std_23)

function(add_ctest_executable target)
	add_executable(${target} ${ARGN})
	target_link_libraries(${target} PRIVATE ctestcore spinscale Boost::system Threads::Threads)
	target_compile_features(${target} PRIVATE cxx_std_23)
endfunction()

add_ctest_executable(ctest-sync-main
	${CMAKE_SOURCE_DIR}/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../tests/sync_main_drives_continuation.cpp
)

add_ctest_executable(ctest-group-edge-test
	${CMAKE_CURRENT_SOURCE_DIR}/../tests/group_edge_test.cpp
)

add_ctest_executable(ctest-group-timer-test
	${CMAKE_CURRENT_SOURCE_DIR}/../tests/group_timer_test.cpp
)

add_ctest_executable(ctest-group-smoke
	${CMAKE_CURRENT_SOURCE_DIR}/../tests/group_smoke.cpp
)

add_ctest_executable(ctest-viral-non-posting-test
	${CMAKE_CURRENT_SOURCE_DIR}/../tests/viral_non_posting_test.cpp
)
