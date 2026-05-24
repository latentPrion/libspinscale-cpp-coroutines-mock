#include <iostream>
#include <stdexcept>

#include <body/body.h>
#include <harness/harness.h>
#include <harnessComponent.h>
#include <leg/leg.h>
#include <world/world.h>

namespace ctest {
namespace harness {

namespace global {
std::shared_ptr<TestHarness> harness;
} // namespace global

TestHarness::TestHarness()
:	sscl::PuppetApplication(
		std::vector<std::shared_ptr<sscl::PuppetThread>>{
			std::make_shared<HarnessThread>(
				CtestThreadId::BODY, getThreadName(CtestThreadId::BODY),
				sscl::PuppetComponent::defaultPuppetMain, body,
				&HarnessComponent::preJoltHook),
			std::make_shared<HarnessThread>(
				CtestThreadId::WORLD, getThreadName(CtestThreadId::WORLD),
				sscl::PuppetComponent::defaultPuppetMain, world,
				&HarnessComponent::preJoltHook),
			std::make_shared<HarnessThread>(
				CtestThreadId::LEG, getThreadName(CtestThreadId::LEG),
				sscl::PuppetComponent::defaultPuppetMain, leg,
				&HarnessComponent::preJoltHook)
		}),
	body(*this, componentThreads[CtestThreadId::BODY - 1]),
	world(*this, componentThreads[CtestThreadId::WORLD - 1]),
	leg(*this, componentThreads[CtestThreadId::LEG - 1])
{
}

std::shared_ptr<HarnessThread> TestHarness::getComponentThread(
	sscl::ThreadId id) const
{
	if (id == CtestThreadId::MRNTT)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": MRNTT is not a HarnessThread");
	}

	for (const auto &thread : componentThreads)
	{
		if (thread->id == id) {
			return std::static_pointer_cast<HarnessThread>(thread);
		}
	}

	throw std::runtime_error(
		std::string(__func__) + ": No HarnessThread found with ID "
		+ std::to_string(static_cast<int>(id)));
}

std::shared_ptr<HarnessThread> TestHarness::getComponentThread(
	const std::string &name) const
{
	if (name == getThreadName(CtestThreadId::MRNTT))
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": MRNTT is not a HarnessThread");
	}

	for (const auto &thread : componentThreads)
	{
		if (thread->name == name) {
			return std::static_pointer_cast<HarnessThread>(thread);
		}
	}

	throw std::runtime_error(
		std::string(__func__) + ": No HarnessThread found with name '"
		+ name + "'");
}

mrntt::MrnttViralPostingInvoker<void> TestHarness::initializeCReq(
	std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	try
	{
		distributeAndPinThreadsAcrossCpus();
	}
	catch (const std::exception &exception)
	{
		std::cerr << "TestHarness: CPU pinning failed: "
			<< exception.what() << "\n";
	}

	std::function<void()> noopCallback;

	co_await joltAllPuppetThreadsCReq(exceptionPtr, noopCallback);
	co_await startAllPuppetThreadsCReq(exceptionPtr, noopCallback);

	co_await body.initializeCReq(exceptionPtr, noopCallback);
	co_await world.initializeCReq(exceptionPtr, noopCallback);
	co_await leg.initializeCReq(exceptionPtr, noopCallback);

	co_return;
}

mrntt::MrnttViralPostingInvoker<void> TestHarness::finalizeCReq(
	std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	std::function<void()> noopCallback;

	co_await body.finalizeCReq(exceptionPtr, noopCallback);
	co_await world.finalizeCReq(exceptionPtr, noopCallback);
	co_await leg.finalizeCReq(exceptionPtr, noopCallback);

	co_await exitAllPuppetThreadsCReq(exceptionPtr, noopCallback);

	co_return;
}

} // namespace harness
} // namespace ctest
