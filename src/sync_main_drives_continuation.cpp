#include <coroutine>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <boost/asio/io_service.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include "childThreads.h"
#include <spinscale/co/coQutex.h>

using sscl::co::CoQutex;

CoQutex initializeCReqLock;

LegViralInvoker<int> print2Ints(int arg1, int arg2)
{
	CoQutex print2SIntsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	auto r = co_await print2SIntsLock.getAcquireInvocationAndSuspensionPolicy();
	std::cout << __func__ << ": " << std::this_thread::get_id() << " !!! print2Ints returning: " << arg1 + arg2 << "\n";
	co_return arg1 + arg2;
}

WorldViralInvoker<std::string> print2Strings(std::string arg1, std::string arg2)
{
	CoQutex print2StringsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
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
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to co_await print2Strings.\n";
	auto r2 = co_await initializeCReqLock.getAcquireInvocationAndSuspensionPolicy();
	auto deferredPrint2StringsResult = print2Strings("Hello", "World");
	std::this_thread::sleep_for(std::chrono::milliseconds(750));
	std::string returnedString = co_await deferredPrint2StringsResult;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " print2Strings returned: " << returnedString << "\n";
	co_return;
}

void finalizeAllThreads()
{
	auto stopPuppetLane = [](const std::shared_ptr<sscl::PuppetThread> &lane) {
		lane->keepLooping = false;
		lane->getIoService().stop();
	};

	boost::asio::post(bodyPuppetLane->getIoService(), [stopPuppetLane] {
		stopPuppetLane(bodyPuppetLane);
	});
	boost::asio::post(mainPuppeteerLane->getIoService(), [] {
		mainPuppeteerLane->exitLoop();
	});
	boost::asio::post(worldPuppetLane->getIoService(), [stopPuppetLane] {
		stopPuppetLane(worldPuppetLane);
	});
	boost::asio::post(legPuppetLane->getIoService(), [stopPuppetLane] {
		stopPuppetLane(legPuppetLane);
	});
}

void runSyncMainBody()
{
	boost::asio::signal_set shutdownSignals(
		mainPuppeteerLane->getIoService(), SIGINT, SIGTERM);
	shutdownSignals.async_wait([](const boost::system::error_code &errorCode, int) {
		if (errorCode) {
			return;
		}
		finalizeAllThreads();
	});

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to call initializeCReq.\n";
	std::exception_ptr initializeCReqExceptionPtr = nullptr;
	BodyNonViralNonSuspendingInvoker initializeCReqInvoker = initializeCReq(
		initializeCReqExceptionPtr,
		[]() {
			std::cout << "initializeCReq caller completion: "
				<< std::this_thread::get_id()
				<< " (callee frame is destroyed when main() exits and "
				<< "~initializeCReqInvoker runs).\n";
			finalizeAllThreads();
		},
		4, "KEKW");
	(void)initializeCReqInvoker;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " initializeCReq returned.\n";

	for (mainPuppeteerLane->keepLooping = true;
		mainPuppeteerLane->keepLooping;) {
		bool sendExceptionInd = false;

		try {
			mainPuppeteerLane->getIoService().reset();
			mainPuppeteerLane->getIoService().run();
		} catch (const std::exception &exception) {
			sendExceptionInd = true;
			std::cerr << "main: Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			sendExceptionInd = true;
			std::cerr << "main: Unknown exception occurred\n";
		}

		if (sendExceptionInd) {
			std::cerr << "main: loop exception hook stand-in\n";
			finalizeAllThreads();
		}
	}
}

int main()
{
	startSyncMainPuppetLanes();

	boost::asio::post(mainPuppeteerLane->getIoService(), [] {
		runSyncMainBody();
	});

	joinSyncMainPuppetLanes();
	return EXIT_SUCCESS;
}
