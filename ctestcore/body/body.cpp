#include <stdexcept>

#include <body/body.h>
#include <componentThread.h>
#include <harness/harness.h>
#include <spinscale/componentThread.h>

namespace ctest {
namespace body {

boost::asio::io_service &BodyThreadTag::io_service()
{
	if (!harness::global::harness) {
		throw std::runtime_error(
			"BodyThreadTag: harness not initialized");
	}

	return harness::global::harness->getComponentThread(
		CtestThreadId::BODY)->getIoService();
}

BodyViralPostingInvoker<void> BodyComponent::initializeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::BODY)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on Body thread");
	}

	co_return;
}

BodyViralPostingInvoker<void> BodyComponent::finalizeCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	auto self = sscl::ComponentThread::getSelf();
	if (self->id != CtestThreadId::BODY)
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": Must be executed on Body thread");
	}

	co_return;
}

} // namespace body
} // namespace ctest
