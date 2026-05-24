#ifndef CTEST_LEG_H
#define CTEST_LEG_H

#include <exception>
#include <functional>

#include <harnessComponent.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/postingPromise.h>

namespace ctest {
namespace leg {

struct LegThreadTag
{
	static boost::asio::io_service &io_service();
};

template <typename T>
using LegPostingPromise =
	sscl::co::TaggedPostingPromise<T, LegThreadTag>;
using LegNonViralPostingInvoker =
	sscl::co::NonViralPostingInvoker<LegPostingPromise>;
template <typename T>
using LegViralPostingInvoker =
	sscl::co::ViralPostingInvoker<LegPostingPromise, T>;

class LegComponent
:	public HarnessComponent
{
public:
	LegComponent(
		sscl::PuppetApplication &parent,
		const std::shared_ptr<sscl::PuppetThread> &thread)
	:	HarnessComponent(parent, thread)
	{}

	LegViralPostingInvoker<void> initializeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	LegViralPostingInvoker<void> finalizeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);
};

} // namespace leg
} // namespace ctest

#endif // CTEST_LEG_H
