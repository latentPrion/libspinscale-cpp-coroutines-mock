#ifndef CTEST_HARNESS_THREAD_H
#define CTEST_HARNESS_THREAD_H

#include <memory>
#include <string>

#include <componentThread.h>
#include <spinscale/componentThread.h>

namespace ctest {

class HarnessThread
:	public sscl::PuppetThread
{
public:
	HarnessThread(
		sscl::ThreadId id, std::string name,
		sscl::PuppetThread::entryPointFn entryPoint,
		sscl::PuppetComponent &component,
		sscl::PuppetThread::preJoltHookFn preJoltFn = nullptr)
	:	sscl::PuppetThread(
			id, std::move(name), std::move(entryPoint),
			component, preJoltFn)
	{}
};

} // namespace ctest

#endif // CTEST_HARNESS_THREAD_H
