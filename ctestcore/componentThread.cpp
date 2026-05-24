#include <stdexcept>

#include <componentThread.h>

namespace ctest {

namespace {

constexpr const char *mrnttThreadName = "ctest:mrntt";
constexpr const char *bodyThreadName = "ctest:body";
constexpr const char *worldThreadName = "ctest:world";
constexpr const char *legThreadName = "ctest:leg";

}

std::string getThreadName(sscl::ThreadId id)
{
	switch (id)
	{
	case CtestThreadId::MRNTT:
		return mrnttThreadName;
	case CtestThreadId::BODY:
		return bodyThreadName;
	case CtestThreadId::WORLD:
		return worldThreadName;
	case CtestThreadId::LEG:
		return legThreadName;
	default:
		throw std::runtime_error(
			std::string(__func__) + ": Unknown thread id "
			+ std::to_string(static_cast<int>(id)));
	}
}

} // namespace ctest
