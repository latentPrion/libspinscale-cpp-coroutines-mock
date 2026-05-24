#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include <boost/asio/signal_set.hpp>
#include <componentThread.h>
#include <marionette/marionette.h>
#include <spinscale/componentThread.h>
#include <test/marionetteTestRunner.h>

namespace ctest {
namespace mrntt {

std::shared_ptr<sscl::PuppeteerThread> thread = std::make_shared<
	sscl::PuppeteerThread>(
		CtestThreadId::MRNTT, getThreadName(CtestThreadId::MRNTT),
		sscl::pptr::PuppeteerComponent::defaultPuppeteerMain,
		mrntt, &MarionetteComponent::preJoltHook);

MarionetteComponent mrntt(thread);

void marionetteInitializeReqCb(bool success)
{
	if (success)
	{
		std::cout << __func__ << ": Marionette initialized.\n";
		test::runRegisteredMarionetteTestBody();
		return;
	}

	std::cerr << __func__ << ": Failed to initialize Marionette. "
		<< "Shutting down.\n";
	::ctest::mrntt::mrntt.holdFinalizeCReq(
		[]() { marionetteFinalizeReqCb(true); });
}

void marionetteFinalizeReqCb(bool success)
{
	if (!success)
	{
		std::cerr << __func__ << ": Failed to finalize Marionette.\n";
	}

	std::cout << __func__ << ": Marionette finalized.\n";
	thread->exitLoop();
}

MarionetteComponent::MarionetteComponent(
	const std::shared_ptr<sscl::PuppeteerThread> &puppeteerThread)
:	sscl::pptr::PuppeteerComponent(puppeteerThread)
{
}

void MarionetteComponent::preJoltHook(sscl::PuppeteerThread &self)
{
	sscl::pptr::exitCode = EXIT_SUCCESS;
	pthread_setname_np(pthread_self(), self.name.c_str());
	std::cout << __func__ << ": Waiting for command line JOLT\n";
}

void MarionetteComponent::postJoltHook()
{
	signals = std::make_unique<boost::asio::signal_set>(
		thread->getIoService(), SIGINT, SIGTERM);

	signals->async_wait(
		[](const boost::system::error_code &errorCode, int)
		{
			if (errorCode) {
				return;
			}

			std::cerr << "SIGINT/SIGTERM received. Initiating shutdown...\n";
			::ctest::mrntt::mrntt.holdFinalizeCReq(
				[]() { marionetteFinalizeReqCb(true); });
		});
}

void MarionetteComponent::tryBlock1Hook()
{
	std::cout << "PuppeteerThread::main: coroutines ctest\n";
}

void MarionetteComponent::preLoopHook()
{
	holdInitializeCReq(
		[]
		{
			marionetteInitializeReqCb(
				!::ctest::mrntt::mrntt.initializeLifetimeExceptionPtr);
		});

	std::cout << "PuppeteerThread::main: Entering event loop\n";
}

void MarionetteComponent::postLoopHook()
{
	std::cout << "PuppeteerThread::main: Exited event loop\n";
}

void MarionetteComponent::handleTryBlock1TypedException(
	const std::exception &exception)
{
	std::cerr << "main: Exception occurred: " << exception.what() << "\n";
	sscl::pptr::exitCode = EXIT_FAILURE;
}

void MarionetteComponent::handleTryBlock1UnknownException()
{
	std::cerr << "main: Unknown exception occurred\n";
	sscl::pptr::exitCode = EXIT_FAILURE;
}

void MarionetteComponent::handleLoopExceptionHook()
{
	sscl::pptr::exitCode = EXIT_FAILURE;
	exceptionInd();
}

void MarionetteComponent::exceptionInd()
{
	auto puppeteer = sscl::ComponentThread::getPptr();

	boost::asio::post(
		puppeteer->getIoService(),
		[]
		{
			::ctest::mrntt::mrntt.holdFinalizeCReq(
				[]() { marionetteFinalizeReqCb(true); });
		});
}

} // namespace mrntt
} // namespace ctest
