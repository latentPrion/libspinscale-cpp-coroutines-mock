cmake_minimum_required(VERSION 3.16)

project(coroutines VERSION 0.1.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")

option(ENABLE_LTO "Enable link-time optimization (-flto)" OFF)
if(ENABLE_LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
	if(ipo_supported)
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	else()
		message(WARNING "LTO not supported: ${ipo_error}")
	endif()
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
	"#include <coroutine>
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
	Task test_coroutine() { co_await SuspendNever{}; }
	int main() { test_coroutine(); return 0; }"
	COROUTINES_HAVE_CXX23_COROUTINES
)
if(NOT COROUTINES_HAVE_CXX23_COROUTINES)
	message(FATAL_ERROR "A compiler with C++23 coroutine support is required.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libspinscale/CMakeLists.txt")
	message(FATAL_ERROR
		"libspinscale submodule is missing. Run:\n"
		"  git submodule update --init --recursive")
endif()

include(GNUInstallDirs)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
		"Install prefix for libspinscale install rules" FORCE)
endif()

add_subdirectory(libspinscale)
