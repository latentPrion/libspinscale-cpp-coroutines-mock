#include <coroutine>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include "childThreads.h"
#include "coQutex.h"
#include "current_io_context.h"

CoQutex initializeCReqLock;

LegViralInvoker<int> print2Ints(int arg1, int arg2)
{
	CoQutex print2SIntsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	auto r = co_await print2SIntsLock.getAcquireInvocationAndSuspensionPolicy();
//	auto r2 = co_await initializeCReqLock.getAcquireInvocationAndSuspensionPolicy();
	std::cout << __func__ << ": " << std::this_thread::get_id() << " !!! print2Ints returning: " << arg1 + arg2 << "\n";
	co_return arg1 + arg2;
}

WorldViralInvoker<std::string> print2Strings(std::string arg1, std::string arg2)
{
	CoQutex print2StringsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
//	throw "KEK!";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	auto r = co_await print2StringsLock.getAcquireInvocationAndSuspensionPolicy();
	co_await print2Ints(1, 2);
	co_return arg1 + " " + arg2;
}

BodyNonViralNonSuspendingInvoker initializeCReq(
	std::exception_ptr &, std::function<void()>,
	int arg3, std::string arg4)
{
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing: " << arg3 << " " << arg4 << ".\n";
	// throw std::runtime_error("initializeCReq exception");
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to co_await print2Strings.\n";
	auto r2 = co_await initializeCReqLock.getAcquireInvocationAndSuspensionPolicy();
	auto deferredPrint2StringsResult = print2Strings("Hello", "World");
	std::this_thread::sleep_for(std::chrono::milliseconds(750));
	std::string returnedString = co_await deferredPrint2StringsResult;
//	auto r3 = co_await initializeCReqLock.getAcquireInvocationAndSuspensionPolicy();
	std::cout << __func__ << ": " << std::this_thread::get_id() << " print2Strings returned: " << returnedString << "\n";
//	throw std::runtime_error("initializeCReq exception test!");
	co_return;
}

void finalizeAllThreads(
	bool &body_keep_looping,
	bool &world_keep_looping,
	bool &leg_keep_looping,
	bool &keep_looping)
{
	boost::asio::post(bodyIoContext, [&body_keep_looping] {
		body_keep_looping = false;
		bodyIoContext.stop();
	});
	boost::asio::post(mainIoContext, [&keep_looping] {
		keep_looping = false;
		mainIoContext.stop();
	});
	boost::asio::post(worldIoContext, [&world_keep_looping] {
		world_keep_looping = false;
		worldIoContext.stop();
	});
	boost::asio::post(legIoContext, [&leg_keep_looping] {
		leg_keep_looping = false;
		legIoContext.stop();
	});
}

int main()
{
	tls_current_io_context = &mainIoContext;
	bool keep_looping = true;
	bool body_keep_looping = true;
	bool world_keep_looping = true;
	bool leg_keep_looping = true;

	boost::asio::signal_set shutdownSignals(mainIoContext, SIGINT, SIGTERM);
	shutdownSignals.async_wait([&keep_looping, &body_keep_looping, &world_keep_looping, &leg_keep_looping](
		const boost::system::error_code& errorCode, int)
	{
		if (errorCode) {
			return;
		}
		finalizeAllThreads(
			body_keep_looping, world_keep_looping,
			leg_keep_looping, keep_looping);
	});

	std::thread body_thread(bodyThreadEntry, std::ref(body_keep_looping));
	std::thread world_thread(worldThreadEntry, std::ref(world_keep_looping));
	std::thread leg_thread(legThreadEntry, std::ref(leg_keep_looping));

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";
	/* Give the body thread time to start up and wait on its io context.
	 */
//	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to call initializeCReq.\n";
	std::exception_ptr initializeCReqExceptionPtr = nullptr;
	BodyNonViralNonSuspendingInvoker initializeCReqInvoker = initializeCReq(
		initializeCReqExceptionPtr,
		[&body_keep_looping, &world_keep_looping, &leg_keep_looping, &keep_looping]()
	{
		std::cout << "initializeCReq caller completion: " << std::this_thread::get_id()
			<< " (callee frame is destroyed when main() exits and ~initializeCReqInvoker runs).\n";
		finalizeAllThreads(
			body_keep_looping, world_keep_looping,
			leg_keep_looping, keep_looping);
	},
		4, "KEKW");
	(void)initializeCReqInvoker;
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
			finalizeAllThreads(
				body_keep_looping, world_keep_looping,
				leg_keep_looping, keep_looping);
		}
	}

	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to join body thread.\n";
	body_thread.join();
	world_thread.join();
	leg_thread.join();
	tls_current_io_context = nullptr;
	return EXIT_SUCCESS;
}
