#include "coroutinesTestLanes.h"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <boost/asio/post.hpp>
#include <spinscale/component.h>
#include <spinscale/componentThread.h>

namespace coroutines::test {

namespace {

constexpr sscl::ThreadId testPuppeteerThreadId = 0;

class TestPuppeteerComponent final : public sscl::pptr::PuppeteerComponent
{
public:
	explicit TestPuppeteerComponent(
		const std::shared_ptr<sscl::PuppeteerThread> &thread)
	:	sscl::pptr::PuppeteerComponent(thread)
	{}

	void handleLoopExceptionHook() override
	{}
};

std::mutex g_laneStartMutex;
std::condition_variable g_laneStartCondition;
bool g_puppeteerLaneSharedPtrReady = false;
bool g_laneEntryTlsReady = false;

const std::function<void()> *g_pendingTestBody = nullptr;

std::unique_ptr<TestPuppeteerComponent> g_testPuppeteerComponent;
std::shared_ptr<sscl::PuppeteerThread> g_testPuppeteerLane;

void testPuppeteerLaneEntry(
	const sscl::PuppeteerThread::EntryFnArguments &entryArguments)
{
	sscl::PuppeteerThread &lane = entryArguments.usableBeforeJolt;

	{
		std::unique_lock<std::mutex> lock(g_laneStartMutex);
		g_laneStartCondition.wait(lock, [] {
			return g_puppeteerLaneSharedPtrReady;
		});
	}

	lane.initializeTls();

	{
		std::lock_guard<std::mutex> lock(g_laneStartMutex);
		g_laneEntryTlsReady = true;
	}
	g_laneStartCondition.notify_one();

	const std::function<void()> *testBody = nullptr;

	{
		std::unique_lock<std::mutex> lock(g_laneStartMutex);
		g_laneStartCondition.wait(lock, [] {
			return g_pendingTestBody != nullptr;
		});
		testBody = g_pendingTestBody;
	}

	(*testBody)();
	lane.exitLoop();
}

void ensureTestPuppeteerLaneStarted()
{
	if (g_testPuppeteerLane) {
		return;
	}

	sscl::ComponentThread::setPuppeteerThreadId(testPuppeteerThreadId);

	g_testPuppeteerComponent = std::make_unique<TestPuppeteerComponent>(
		std::shared_ptr<sscl::PuppeteerThread>());
	g_testPuppeteerLane = std::make_shared<sscl::PuppeteerThread>(
		testPuppeteerThreadId,
		"coroutines_test_puppeteer",
		testPuppeteerLaneEntry,
		*g_testPuppeteerComponent,
		nullptr);
	g_testPuppeteerComponent->thread = g_testPuppeteerLane;
	sscl::ComponentThread::setPuppeteerThread(g_testPuppeteerLane);

	{
		std::lock_guard<std::mutex> lock(g_laneStartMutex);
		g_puppeteerLaneSharedPtrReady = true;
	}
	g_laneStartCondition.notify_all();

	{
		std::unique_lock<std::mutex> lock(g_laneStartMutex);
		g_laneStartCondition.wait(lock, [] {
			return g_laneEntryTlsReady;
		});
	}
}

} // namespace

boost::asio::io_service &testPuppeteerLaneIoService()
{
	ensureTestPuppeteerLaneStarted();
	return g_testPuppeteerLane->getIoService();
}

void drainTestPuppeteerLaneIoService()
{
	sscl::PuppeteerThread &lane = *g_testPuppeteerLane;
	boost::asio::io_service &ioService = lane.getIoService();

	lane.work.~work();
	ioService.reset();

	constexpr int maxRunOneSteps = 4096;

	for (int step = 0; step < maxRunOneSteps; ++step) {
		boost::system::error_code drainError;

		if (ioService.run_one(drainError) == 0) {
			break;
		}
	}

	new (&lane.work) boost::asio::io_service::work(ioService);
}

void runOnTestPuppeteerLane(const std::function<void()> &testBody)
{
	ensureTestPuppeteerLaneStarted();

	{
		std::lock_guard<std::mutex> lock(g_laneStartMutex);
		g_pendingTestBody = &testBody;
	}
	g_laneStartCondition.notify_one();

	g_testPuppeteerLane->thread.join();

	g_testPuppeteerLane.reset();
	g_testPuppeteerComponent.reset();
	g_pendingTestBody = nullptr;
	g_puppeteerLaneSharedPtrReady = false;
	g_laneEntryTlsReady = false;
}

void resetTestPuppeteerLaneAfterFork()
{
	g_testPuppeteerLane.reset();
	g_testPuppeteerComponent.reset();
	g_pendingTestBody = nullptr;
	g_puppeteerLaneSharedPtrReady = false;
	g_laneEntryTlsReady = false;
}

} // namespace coroutines::test
