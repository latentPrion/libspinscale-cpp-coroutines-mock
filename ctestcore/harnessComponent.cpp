#include <iostream>
#include <pthread.h>

#include <harnessComponent.h>
#include <marionette/marionette.h>

namespace ctest {

void HarnessComponent::preJoltHook(sscl::PuppetThread &self)
{
	pthread_setname_np(pthread_self(), self.name.c_str());
}

void HarnessComponent::handleLoopExceptionHook()
{
	mrntt::mrntt.exceptionInd();
}

void HarnessComponent::preLoopHook()
{
	std::cout << thread->name << ":defaultPuppetMain"
		<< ": Entering event loop\n";
}

void HarnessComponent::postLoopHook()
{
	std::cout << thread->name << ":defaultPuppetMain"
		<< ": Exited event loop\n";
}

} // namespace ctest
