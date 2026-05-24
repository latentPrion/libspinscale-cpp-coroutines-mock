#ifndef CTEST_COMPONENT_THREAD_H
#define CTEST_COMPONENT_THREAD_H

#include <string>

#include <spinscale/componentThread.h>

namespace ctest {

enum CtestThreadId : sscl::ThreadId
{
	MRNTT = 0,
	BODY,
	WORLD,
	LEG,
	N_ITEMS
};

std::string getThreadName(sscl::ThreadId id);

} // namespace ctest

#endif // CTEST_COMPONENT_THREAD_H
