#ifndef CTEST_MARIONETTE_TEST_RUNNER_H
#define CTEST_MARIONETTE_TEST_RUNNER_H

#include <chrono>
#include <functional>

#include <boost/asio/io_service.hpp>

namespace ctest {
namespace test {

using MarionetteTestBodyFn = std::function<int()>;

void registerMarionetteTestBody(MarionetteTestBodyFn testBody);

void runRegisteredMarionetteTestBody();

/**	Pump handlers on a puppeteer io_service without blocking run(): the
 *	permanent io_service::work guard would prevent run() from ever returning.
 */
void pumpMarionetteIoContext(
	boost::asio::io_service &io,
	std::chrono::milliseconds maxIdleTime = std::chrono::milliseconds(800),
	std::chrono::milliseconds maxTotalTime = std::chrono::milliseconds(10000));

int runMarionetteHarnessAndExit(MarionetteTestBodyFn testBody);

} // namespace test
} // namespace ctest

#endif // CTEST_MARIONETTE_TEST_RUNNER_H
