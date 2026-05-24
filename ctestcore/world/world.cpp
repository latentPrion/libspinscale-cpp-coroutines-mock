#include <stdexcept>

#include <componentThread.h>
#include <harness/harness.h>
#include <spinscale/componentThread.h>
#include <world/world.h>

namespace ctest {
namespace world {

boost::asio::io_service &WorldThreadTag::io_service()
{
	if (!harness::global::harness) {
		throw std::runtime_error(
			"WorldThreadTag: harness not initialized");
	}

	return harness::global::harness->getComponentThread(
		CtestThreadId::WORLD)->getIoService();
}

WorldViralPostingInvoker<void> WorldComponent::initializeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::WORLD)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on World thread");
	}

	co_return;
}

WorldViralPostingInvoker<void> WorldComponent::finalizeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::WORLD)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on World thread");
	}

	co_return;
}

} // namespace world
} // namespace ctest
