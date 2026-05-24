#ifndef CTEST_HARNESS_COMPONENT_H
#define CTEST_HARNESS_COMPONENT_H

#include <spinscale/component.h>

namespace ctest {

class HarnessComponent
:	public sscl::PuppetComponent
{
public:
	using sscl::PuppetComponent::PuppetComponent;

	static void preJoltHook(sscl::PuppetThread &thread);

	void handleLoopExceptionHook() override;
	void preLoopHook() override;
	void postLoopHook() override;
};

} // namespace ctest

#endif // CTEST_HARNESS_COMPONENT_H
