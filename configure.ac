AC_PREREQ([2.69])
AC_INIT([coroutines], [0.1.0], [noreply@example.com])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_SRCDIR([README.md])
AC_CONFIG_HEADERS([config.h])

AC_PROG_CXX

AC_LANG([C++])
CXXFLAGS="$CXXFLAGS -std=c++23"

AC_MSG_CHECKING([whether the compiler supports C++23 coroutines])
AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM(
		[[
			#include <coroutine>

			struct SuspendNever {
				bool await_ready() noexcept { return true; }
				void await_suspend(std::coroutine_handle<>) noexcept {}
				void await_resume() noexcept {}
			};

			struct Task {
				struct promise_type {
					Task get_return_object() { return {}; }
					std::suspend_never initial_suspend() noexcept { return {}; }
					std::suspend_never final_suspend() noexcept { return {}; }
					void return_void() noexcept {}
					void unhandled_exception() noexcept {}
				};
			};

			Task test_coroutine() {
				co_await SuspendNever{};
			}
		]],
		[[
			test_coroutine();
			return 0;
		]]
	)],
	[
		AC_MSG_RESULT([yes])
	],
	[
		AC_MSG_RESULT([no])
		AC_MSG_ERROR([A compiler with C++23 coroutine support is required.])
	]
)

AC_CHECK_HEADER([boost/asio/io_context.hpp], [], [
	AC_MSG_ERROR([Boost.Asio headers are required (boost/asio/io_context.hpp).])
])

BOOST_CPPFLAGS=""
BOOST_LDFLAGS="-lboost_system -pthread"
AC_SUBST([BOOST_CPPFLAGS])
AC_SUBST([BOOST_LDFLAGS])

AC_MSG_CHECKING([whether Boost.Asio links with -lboost_system -pthread])
saved_LIBS="$LIBS"
LIBS="$LIBS $BOOST_LDFLAGS"
AC_LINK_IFELSE(
	[AC_LANG_PROGRAM(
		[[
			#include <boost/asio/io_context.hpp>
		]],
		[[
			boost::asio::io_context io;
			(void)io;
			return 0;
		]]
	)],
	[
		AC_MSG_RESULT([yes])
	],
	[
		AC_MSG_RESULT([no])
		AC_MSG_ERROR([Failed to link a minimal Boost.Asio program.])
	]
)
LIBS="$saved_LIBS"

AC_CONFIG_FILES([
	Makefile
	src/Makefile
])
AC_OUTPUT
