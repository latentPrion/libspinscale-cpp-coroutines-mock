#include <stdexcept>

#include <componentThread.h>
#include <harness/harness.h>
#include <leg/leg.h>
#include <spinscale/componentThread.h>

namespace ctest {
namespace leg {

boost::asio::io_service &LegThreadTag::io_service()
{
	if (!harness::global::harness) {
		throw std::runtime_error(
			"LegThreadTag: harness not initialized");
	}

	return harness::global::harness->getComponentThread(
		CtestThreadId::LEG)->getIoService();
}

LegViralPostingInvoker<void> LegComponent::initializeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::LEG)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on Leg thread");
	}

	co_return;
}

LegViralPostingInvoker<void> LegComponent::finalizeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::LEG)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on Leg thread");
	}

	co_return;
}

} // namespace leg
} // namespace ctest
