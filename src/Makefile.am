bin_PROGRAMS = \
	group-edge-test \
	group-timer-test \
	sync-main-drives-continuation
#	awaitable-returnable-to-caller
#	get-ret-obj-must-return-awaitable

AM_CXXFLAGS = -Wall -Wextra -pedantic
AM_CPPFLAGS = @BOOST_CPPFLAGS@
AM_LDFLAGS = @BOOST_LDFLAGS@

# awaitable_returnable_to_caller_SOURCES = awaitable-returnable-to-caller.cpp
# get_ret_obj_must_return_awaitable_SOURCES = get_ret_obj_must_return_awaitable.cpp
group_edge_test_SOURCES = \
	group.h \
	group_edge_test.cpp \
	invokers.h \
	postingInvoker.h \
	promises.h \
	sharedResourceGroup.h
group_timer_test_SOURCES = \
	group.h \
	group_timer_test.cpp \
	invokers.h \
	postingInvoker.h \
	promises.h \
	sharedResourceGroup.h
sync_main_drives_continuation_SOURCES = \
	childThreads.cpp \
	childThreads.h \
	coConditionVariable.h \
	group.h \
	group_smoke.cpp \
	invokers.h \
	postingInvoker.h \
	promises.h \
	sharedResourceGroup.h \
	sync_main_drives_continuation.cpp
