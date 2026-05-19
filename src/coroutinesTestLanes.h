#ifndef COROUTINES_TEST_LANES_H
#define COROUTINES_TEST_LANES_H

#include <functional>

#include <boost/asio/io_service.hpp>

namespace coroutines::test {

boost::asio::io_service &testPuppeteerLaneIoService();

void runOnTestPuppeteerLane(const std::function<void()> &testBody);

void drainTestPuppeteerLaneIoService();

void resetTestPuppeteerLaneAfterFork();

} // namespace coroutines::test

#endif // COROUTINES_TEST_LANES_H
