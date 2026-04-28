#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <functional>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "current_io_context.h"
#include "coQutex.h"
#include "promises.h"
#include "postingInvoker.h"

thread_local boost::asio::io_context *tls_current_io_context = nullptr;

boost::asio::io_context &current_io_context() {
	if (!tls_current_io_context) {
		throw std::runtime_error("TLS io_context is not set");
	}
	return *tls_current_io_context;
}

boost::asio::io_context mainIoContext, bodyIoContext;
boost::asio::executor_work_guard<
	boost::asio::io_context::executor_type> bodyIoContextIdleDoesNotEndRun(
		bodyIoContext.get_executor());	

struct NonViralNonSuspendingInvoker
:	public PostingInvoker<BodyPostingPromise<void>, void>
{
	struct promise_type
	:	public BodyPostingPromise<void>
	{
		using BodyPostingPromise<void>::BodyPostingPromise;
		NonViralNonSuspendingInvoker get_return_object()
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning NonViralNonSuspendingInvoker.\n";
			if (!this->callerLambda) {
				throw std::runtime_error("Missing completion lambda: non-viral coroutine would leak frame because no destroy() owner was provided.");
			}
			return NonViralNonSuspendingInvoker(*this);
		}
	};

	using PostingInvoker<BodyPostingPromise<void>, void>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
		return true;
	}
	void await_suspend(std::coroutine_handle<NonViralNonSuspendingInvoker>) noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}
	void await_resume() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}
};

template <typename T>
struct ViralSuspendingInvoker
:	public PostingInvoker<BodyPostingPromise<T>, T>
{
	struct promise_type
	:	public BodyPostingPromise<T>
	{
		using BodyPostingPromise<T>::BodyPostingPromise;
		ViralSuspendingInvoker<T> get_return_object() noexcept
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning ViralSuspendingInvoker.\n";
			return ViralSuspendingInvoker<T>(*this);
		}
	};

	using PostingInvoker<BodyPostingPromise<T>, T>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning false.\n";
		return false;
	}
	void await_suspend(std::coroutine_handle<> callerSchedHandle) noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Setting callerSchedHandle and 'suspending'.\n";
		this->setCallerSchedHandle(callerSchedHandle);
	}
	T await_resume() const
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Resumed on caller thread, hopefully.\n";
		return PostingInvoker<BodyPostingPromise<T>, T>::await_resume();
	}
};

ViralSuspendingInvoker<std::string> print2Strings(std::string arg1, std::string arg2)
{
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
//	throw "KEK!";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	co_return arg1 + " " + arg2;
}

NonViralNonSuspendingInvoker initializeCReq(
	std::exception_ptr &, std::function<void(CalleeCoroutineHandleDestroyer)>)
{
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	// throw std::runtime_error("initializeCReq exception");
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to co_await print2Strings.\n";
	std::string returnedString = co_await print2Strings("Hello", "World");
	std::cout << __func__ << ": " << std::this_thread::get_id() << " print2Strings returned: " << returnedString << "\n";
	co_return;
}

void bodyThreadEntry(bool &body_keep_looping)
{
	tls_current_io_context = &bodyIoContext;

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";

	for (body_keep_looping = true; body_keep_looping;) {
		bool send_exception_ind = false;

		try {
			bodyIoContext.restart();
			bodyIoContext.run();
		} catch (const std::exception &exception) {
			send_exception_ind = true;
			std::cerr << "body: Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			send_exception_ind = true;
			std::cerr << "body: Unknown exception occurred\n";
		}

		if (send_exception_ind) {
			std::cerr << "body: loop exception hook stand-in\n";
		}
	}
	tls_current_io_context = nullptr;
}

int main() {
	tls_current_io_context = &mainIoContext;
	bool keep_looping = true;
	bool body_keep_looping = true;

	boost::asio::signal_set shutdownSignals(mainIoContext, SIGINT, SIGTERM);
	shutdownSignals.async_wait([&keep_looping, &body_keep_looping](
		const boost::system::error_code& errorCode, int)
	{
		if (errorCode) {
			return;
		}
		boost::asio::post(bodyIoContext, [&body_keep_looping] {
			bodyIoContext.stop();
			body_keep_looping = false;
		});
		boost::asio::post(mainIoContext, [&keep_looping] {
			mainIoContext.stop();
			keep_looping = false;
		});
	});

	std::thread body_thread(bodyThreadEntry, std::ref(body_keep_looping));

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";
	/* Give the body thread time to start up and wait on its io context.
	 */
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to call initializeCReq.\n";
	/*NonViralNonSuspendingInvoker invoker =*/
	std::exception_ptr initializeCReqExceptionPtr = nullptr;
	initializeCReq(
		initializeCReqExceptionPtr,
		[&initializeCReqExceptionPtr, &keep_looping] (CalleeCoroutineHandleDestroyer)
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to check if initializeCReq threw an exception.\n";
		if (initializeCReqExceptionPtr)
		{
			keep_looping = false;
			mainIoContext.stop();
			std::rethrow_exception(initializeCReqExceptionPtr);
		}
	});
	std::cout << __func__ << ": " << std::this_thread::get_id() << " initializeCReq returned.\n";

	for (keep_looping = true; keep_looping;) {
		bool send_exception_ind = false;

		try {
			mainIoContext.restart();
			mainIoContext.run();
		} catch (const std::exception &exception) {
			send_exception_ind = true;
			std::cerr << "main: Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			send_exception_ind = true;
			std::cerr << "main: Unknown exception occurred\n";
		}

		if (send_exception_ind) {
			std::cerr << "main: loop exception hook stand-in\n";
		}
	}

	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to join body thread.\n";
	body_thread.join();
	tls_current_io_context = nullptr;
	return EXIT_SUCCESS;
}
