#ifndef CTEST_WORLD_H
#define CTEST_WORLD_H

#include <exception>
#include <functional>

#include <harnessComponent.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>

namespace ctest {
namespace world {

struct WorldThreadTag
{
	static boost::asio::io_service &io_service();
};

template <typename T>
using WorldPostingPromise =
	sscl::co::TaggedPostingPromise<T, WorldThreadTag>;
using WorldNonViralPostingInvoker =
	sscl::co::NonViralPostingInvoker<WorldPostingPromise>;
template <typename T>
using WorldViralPostingInvoker =
	sscl::co::ViralPostingInvoker<WorldPostingPromise, T>;

class WorldComponent
:	public HarnessComponent
{
public:
	WorldComponent(
		sscl::PuppetApplication &parent,
		const std::shared_ptr<sscl::PuppetThread> &thread)
	:	HarnessComponent(parent, thread)
	{}

	WorldViralPostingInvoker<void> initializeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	WorldViralPostingInvoker<void> finalizeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);
};

} // namespace world
} // namespace ctest

#endif // CTEST_WORLD_H
