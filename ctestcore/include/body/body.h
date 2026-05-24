#ifndef CTEST_BODY_H
#define CTEST_BODY_H

#include <exception>
#include <functional>

#include <harnessComponent.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>

namespace ctest {
namespace body {

struct BodyThreadTag
{
	static boost::asio::io_service &io_service();
};

template <typename T>
using BodyPostingPromise =
	sscl::co::TaggedPostingPromise<T, BodyThreadTag>;
using BodyNonViralPostingInvoker =
	sscl::co::NonViralPostingInvoker<BodyPostingPromise>;
template <typename T>
using BodyViralPostingInvoker =
	sscl::co::ViralPostingInvoker<BodyPostingPromise, T>;

class BodyComponent
:	public HarnessComponent
{
public:
	BodyComponent(
		sscl::PuppetApplication &parent,
		const std::shared_ptr<sscl::PuppetThread> &thread)
	:	HarnessComponent(parent, thread)
	{}

	BodyViralPostingInvoker<void> initializeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	BodyViralPostingInvoker<void> finalizeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);
};

} // namespace body
} // namespace ctest

#endif // CTEST_BODY_H
