#ifndef CTEST_HARNESS_H
#define CTEST_HARNESS_H

#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include <body/body.h>
#include <componentThread.h>
#include <harnessThread.h>
#include <leg/leg.h>
#include <marionette/marionette.h>
#include <spinscale/puppetApplication.h>
#include <world/world.h>

namespace ctest {
namespace harness {

class TestHarness
:	public sscl::PuppetApplication
{
public:
	TestHarness();
	~TestHarness() = default;

	std::shared_ptr<HarnessThread> getComponentThread(sscl::ThreadId id) const;
	std::shared_ptr<HarnessThread> getComponentThread(
		const std::string &name) const;

	mrntt::MrnttViralPostingInvoker<void> initializeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	mrntt::MrnttViralPostingInvoker<void> finalizeCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> callback);

	body::BodyComponent body;
	world::WorldComponent world;
	leg::LegComponent leg;
};

namespace global {
extern std::shared_ptr<TestHarness> harness;
} // namespace global

} // namespace harness
} // namespace ctest

#endif // CTEST_HARNESS_H
