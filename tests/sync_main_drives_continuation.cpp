#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>

#include <body/body.h>
#include <componentThread.h>
#include <harness/harness.h>
#include <leg/leg.h>
#include <marionette/marionette.h>
#include <spinscale/co/coQutex.h>
#include <test/marionetteTestRunner.h>
#include <world/world.h>

namespace ctest {
namespace sync_main {

sscl::co::CoQutex initializeCReqLock;

leg::LegViralPostingInvoker<int> print2Ints(int arg1, int arg2)
{
	sscl::co::CoQutex print2IntsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	(void)co_await print2IntsLock.getAcquireInvocationAndSuspensionPolicy();
	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " !!! print2Ints returning: " << arg1 + arg2 << "\n";
	co_return arg1 + arg2;
}

world::WorldViralPostingInvoker<std::string> print2Strings(
	std::string arg1, std::string arg2)
{
	sscl::co::CoQutex print2StringsLock;
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	(void)co_await print2StringsLock.getAcquireInvocationAndSuspensionPolicy();
	(void)co_await print2Ints(1, 2);
	co_return arg1 + " " + arg2;
}

body::BodyNonViralPostingInvoker initializeDemoCReq(
	std::exception_ptr &, std::function<void()>,
	int arg3, std::string arg4)
{
	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " Executing: " << arg3 << " " << arg4 << ".\n";
	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " About to co_await print2Strings.\n";
	(void)co_await initializeCReqLock.getAcquireInvocationAndSuspensionPolicy();
	auto deferredPrint2StringsResult = print2Strings("Hello", "World");
	std::this_thread::sleep_for(std::chrono::milliseconds(750));
	std::string returnedString = co_await deferredPrint2StringsResult;
	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " print2Strings returned: " << returnedString << "\n";
	co_return;
}

void pollAllHarnessComponentThreadsOnce()
{
	(void)mrntt::thread->getIoService().poll_one();
	if (!harness::global::harness) {
		return;
	}

	(void)harness::global::harness->getComponentThread(CtestThreadId::BODY)
		->getIoService().poll_one();
	(void)harness::global::harness->getComponentThread(CtestThreadId::WORLD)
		->getIoService().poll_one();
	(void)harness::global::harness->getComponentThread(CtestThreadId::LEG)
		->getIoService().poll_one();
}

void waitForAsyncCompletion(std::atomic<bool> &completed)
{
	constexpr int maxPollSteps = 1000000;

	for (int step = 0; step < maxPollSteps && !completed.load(); ++step) {
		pollAllHarnessComponentThreadsOnce();
	}
}

int runSyncMainTest()
{
	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " About to call initializeDemoCReq.\n";

	std::atomic<bool> initializeCompleted{false};
	std::exception_ptr initializeExceptionPtr = nullptr;
	body::BodyNonViralPostingInvoker initializeDemoCReqInvoker = initializeDemoCReq(
		initializeExceptionPtr,
		[&initializeCompleted]()
		{
			std::cout << "initializeDemoCReq caller completion: "
				<< std::this_thread::get_id() << "\n";
			initializeCompleted.store(true);
		},
		4, "KEKW");

	waitForAsyncCompletion(initializeCompleted);
	(void)initializeDemoCReqInvoker;

	std::cout << __func__ << ": " << std::this_thread::get_id()
		<< " initializeDemoCReq completed.\n";

	return EXIT_SUCCESS;
}

struct SyncMainTestRegistrar
{
	SyncMainTestRegistrar()
	{
		test::registerMarionetteTestBody(runSyncMainTest);
	}
};

SyncMainTestRegistrar syncMainTestRegistrar;

} // namespace sync_main
} // namespace ctest
