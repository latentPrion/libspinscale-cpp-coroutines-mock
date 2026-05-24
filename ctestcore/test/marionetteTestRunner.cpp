#include <iostream>
#include <pthread.h>
#include <thread>

#include <boost/asio/post.hpp>
#include <componentThread.h>
#include <marionette/marionette.h>
#include <spinscale/componentThread.h>
#include <spinscale/runtime.h>
#include <test/marionetteTestRunner.h>

namespace ctest {
namespace test {

namespace {

MarionetteTestBodyFn registeredMarionetteTestBody;

}

void registerMarionetteTestBody(MarionetteTestBodyFn testBody)
{
	registeredMarionetteTestBody = std::move(testBody);
}

void pumpMarionetteIoContext(
	boost::asio::io_service &io,
	std::chrono::milliseconds maxIdleTime,
	std::chrono::milliseconds maxTotalTime)
{
	const auto totalDeadline =
		std::chrono::steady_clock::now() + maxTotalTime;
	auto lastProgress = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() < totalDeadline)
	{
		if (io.poll_one() > 0)
		{
			lastProgress = std::chrono::steady_clock::now();
			continue;
		}

		if (std::chrono::steady_clock::now() - lastProgress >= maxIdleTime) {
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void runRegisteredMarionetteTestBody()
{
	if (!registeredMarionetteTestBody)
	{
		std::cout << __func__ << ": No test body registered.\n";
		mrntt::mrntt.holdFinalizeCReq(
			[]() { mrntt::marionetteFinalizeReqCb(true); });
		return;
	}

	boost::asio::post(mrntt::thread->getIoService(), []
	{
		try
		{
			const int exitCode = registeredMarionetteTestBody();
			sscl::pptr::exitCode = exitCode;
		}
		catch (const std::exception &exception)
		{
			std::cerr << "runRegisteredMarionetteTestBody: Test body exception: "
				<< exception.what() << "\n";
			sscl::pptr::exitCode = EXIT_FAILURE;
		}
		catch (...)
		{
			std::cerr << "runRegisteredMarionetteTestBody: Test body unknown exception\n";
			sscl::pptr::exitCode = EXIT_FAILURE;
		}

		mrntt::mrntt.holdFinalizeCReq(
			[]() { mrntt::marionetteFinalizeReqCb(true); });
	});
}

int runMarionetteHarnessAndExit(MarionetteTestBodyFn testBody)
{
	registerMarionetteTestBody(std::move(testBody));

	sscl::ComponentThread::setPuppeteerThreadId(CtestThreadId::MRNTT);
	sscl::ComponentThread::setPuppeteerThread(mrntt::thread);
	pthread_setname_np(pthread_self(), "ctest:CRT:main");

	std::cout << "CRT:" << __func__ << ": about to JOLT Mrntt with cmdline args\n";

	mrntt::thread->getIoService().post(
		[]
		{
			std::cout << "Mrntt:" << __func__ << ":JOLTED: setting cmdline args\n";
			sscl::CrtCommandLineArgs::set(0, nullptr, nullptr);
			mrntt::thread->getIoService().stop();
		});

	mrntt::thread->thread.join();

	std::cout << "CRT:" << __func__ << ": Mrntt exited with code '"
		<< sscl::pptr::exitCode << "'\n";

	return sscl::pptr::exitCode;
}

} // namespace test
} // namespace ctest
