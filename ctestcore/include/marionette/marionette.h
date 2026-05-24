#ifndef CTEST_MARIONETTE_H
#define CTEST_MARIONETTE_H

#include <boost/asio/signal_set.hpp>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

#include <componentThread.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>
#include <spinscale/component.h>

namespace sscl {
class PuppeteerThread;
} // namespace sscl

namespace ctest {
namespace mrntt {

struct MrnttThreadTag
{
	static boost::asio::io_service &io_service() noexcept;
};

template <typename T>
using MrnttPostingPromise =
	sscl::co::TaggedPostingPromise<T, MrnttThreadTag>;
using MrnttNonViralPostingInvoker =
	sscl::co::NonViralPostingInvoker<MrnttPostingPromise>;
template <typename T>
using MrnttViralPostingInvoker =
	sscl::co::ViralPostingInvoker<MrnttPostingPromise, T>;

class MarionetteComponent
:	public sscl::pptr::PuppeteerComponent
{
public:
	MarionetteComponent(const std::shared_ptr<sscl::PuppeteerThread> &thread);
	~MarionetteComponent() = default;

	/**	Hold a non-viral initialize coroutine until it completes. Call from MRNTT
	 *	hooks or from work posted to the MRNTT io_service.
	 */
	void holdInitializeCReq(std::function<void()> completion);

	/**	Hold a non-viral finalize coroutine until it completes. */
	void holdFinalizeCReq(std::function<void()> completion);

	MrnttNonViralPostingInvoker initializeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	MrnttNonViralPostingInvoker finalizeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	void exceptionInd();

	void handleLoopExceptionHook() override;

	static void preJoltHook(sscl::PuppeteerThread &thread);

protected:
	void postJoltHook() override;
	void tryBlock1Hook() override;
	void preLoopHook() override;
	void postLoopHook() override;
	void handleTryBlock1TypedException(const std::exception &exception) override;
	void handleTryBlock1UnknownException() override;

private:
	std::unique_ptr<boost::asio::signal_set> signals;
	std::optional<MrnttNonViralPostingInvoker> initializeCReqInvoker;
	std::optional<MrnttNonViralPostingInvoker> finalizeCReqInvoker;

public:
	std::exception_ptr initializeLifetimeExceptionPtr;
	std::exception_ptr finalizeLifetimeExceptionPtr;
};

extern std::shared_ptr<sscl::PuppeteerThread> thread;
extern MarionetteComponent mrntt;

void marionetteInitializeReqCb(bool success);
void marionetteFinalizeReqCb(bool success);

} // namespace mrntt
} // namespace ctest

#endif // CTEST_MARIONETTE_H
