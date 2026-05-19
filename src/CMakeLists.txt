find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

function(add_coroutines_executable target)
	add_executable(${target} ${ARGN})
	target_link_libraries(${target} PRIVATE spinscale Boost::system Threads::Threads)
	target_compile_features(${target} PRIVATE cxx_std_23)
endfunction()

add_coroutines_executable(group-edge-test
	group_edge_test.cpp
	coroutinesTestLanes.cpp
)
add_coroutines_executable(group-timer-test
	group_timer_test.cpp
	coroutinesTestLanes.cpp
)

add_coroutines_executable(sync-main-drives-continuation
	sync_main_drives_continuation.cpp
	childThreads.cpp
	group_smoke.cpp
)

# add_coroutines_executable(awaitable-returnable-to-caller awaitable-returnable-to-caller.cpp)
# add_coroutines_executable(get-ret-obj-must-return-awaitable get_ret_obj_must_return_awaitable.cpp)
