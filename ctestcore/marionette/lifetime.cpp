#include <iostream>
#include <stdexcept>

#include <harness/harness.h>
#include <marionette/marionette.h>
#include <spinscale/componentThread.h>

namespace ctest {
namespace mrntt {

namespace {

void assertMarionetteThread()
{
	auto self = sscl::ComponentThread::getSelf();

	if (self->id != CtestThreadId::MRNTT)
	{
		throw std::runtime_error(
			"Marionette lifetime coroutine: must run on Marionette thread");
	}
}

} // namespace

boost::asio::io_service &MrnttThreadTag::io_service() noexcept
{
	return thread->getIoService();
}

void MarionetteComponent::holdInitializeCReq(
	std::function<void()> completion)
{
	initializeCReqInvoker.emplace(initializeCReq(
		initializeLifetimeExceptionPtr, std::move(completion)));
}

void MarionetteComponent::holdFinalizeCReq(
	std::function<void()> completion)
{
	finalizeCReqInvoker.emplace(finalizeCReq(
		finalizeLifetimeExceptionPtr, std::move(completion)));
}

MrnttNonViralPostingInvoker MarionetteComponent::initializeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	assertMarionetteThread();

	harness::global::harness = std::make_shared<harness::TestHarness>();

	co_await harness::global::harness->initializeCReq(
		exceptionPtr, std::function<void()>{});

	co_return;
}

MrnttNonViralPostingInvoker MarionetteComponent::finalizeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	assertMarionetteThread();

	if (harness::global::harness)
	{
		co_await harness::global::harness->finalizeCReq(
			exceptionPtr, std::function<void()>{});

		harness::global::harness.reset();
	}

	co_return;
}

} // namespace mrntt
} // namespace ctest
