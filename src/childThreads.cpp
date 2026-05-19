#include "childThreads.h"

#include <array>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include <spinscale/component.h>
#include <spinscale/puppetApplication.h>

namespace {

constexpr sscl::ThreadId mainPuppeteerThreadId = 0;
constexpr sscl::ThreadId bodyPuppetThreadId = 1;
constexpr sscl::ThreadId worldPuppetThreadId = 2;
constexpr sscl::ThreadId legPuppetThreadId = 3;
constexpr std::size_t syncMainLaneCount = 4;

class SyncMainPuppeteerComponent final : public sscl::pptr::PuppeteerComponent
{
public:
	explicit SyncMainPuppeteerComponent(
		const std::shared_ptr<sscl::PuppeteerThread> &thread)
	:	sscl::pptr::PuppeteerComponent(thread)
	{}

	void handleLoopExceptionHook() override
	{}
};

class SyncMainPuppetComponent final : public sscl::PuppetComponent
{
public:
	SyncMainPuppetComponent(
		sscl::PuppetApplication &parent,
		const std::shared_ptr<sscl::PuppetThread> &thread)
	:	sscl::PuppetComponent(parent, thread)
	{}

	void handleLoopExceptionHook() override
	{}
};

struct SyncMainPuppetLaneBundle
{
	std::unique_ptr<SyncMainPuppetComponent> component;
	std::shared_ptr<sscl::PuppetThread> lane;
};

std::mutex g_laneStartMutex;
std::condition_variable g_laneStartCondition;
std::size_t g_puppetLanesConstructed = 0;
std::size_t g_puppetLanesWithTlsReady = 0;

std::shared_ptr<sscl::PuppetApplication> g_syncMainPuppetApplication;
std::unique_ptr<SyncMainPuppeteerComponent> g_mainPuppeteerComponent;
std::array<SyncMainPuppetLaneBundle, 3> g_childPuppetLaneBundles;

void notifyPuppetLaneTlsReady()
{
	{
		std::lock_guard<std::mutex> lock(g_laneStartMutex);
		++g_puppetLanesWithTlsReady;
	}
	g_laneStartCondition.notify_one();
}

void puppetLaneEntry(const sscl::PuppetThread::EntryFnArguments &entryArguments)
{
	sscl::PuppetThread &lane = entryArguments.usableBeforeJolt;

	{
		std::unique_lock<std::mutex> lock(g_laneStartMutex);
		g_laneStartCondition.wait(lock, [] {
			return g_puppetLanesConstructed >= syncMainLaneCount;
		});
	}

	lane.initializeTls();
	notifyPuppetLaneTlsReady();

	for (lane.keepLooping = true; lane.keepLooping;) {
		lane.getIoService().reset();
		lane.getIoService().run();
	}
}

void mainPuppeteerLaneEntry(
	const sscl::PuppeteerThread::EntryFnArguments &entryArguments)
{
	sscl::PuppeteerThread &lane = entryArguments.usableBeforeJolt;

	{
		std::unique_lock<std::mutex> lock(g_laneStartMutex);
		g_laneStartCondition.wait(lock, [] {
			return g_puppetLanesConstructed >= syncMainLaneCount;
		});
	}

	lane.initializeTls();
	notifyPuppetLaneTlsReady();

	for (lane.keepLooping = true; lane.keepLooping;) {
		lane.getIoService().reset();
		lane.getIoService().run();
	}
}

void waitForPuppetLaneTlsReady()
{
	std::unique_lock<std::mutex> lock(g_laneStartMutex);
	g_laneStartCondition.wait(lock, [] {
		return g_puppetLanesWithTlsReady >= syncMainLaneCount;
	});
}

std::shared_ptr<sscl::PuppetThread> makeChildPuppetLane(
	sscl::ThreadId threadId,
	const char *laneName,
	SyncMainPuppetLaneBundle &bundle)
{
	bundle.component = std::make_unique<SyncMainPuppetComponent>(
		*g_syncMainPuppetApplication,
		std::shared_ptr<sscl::PuppetThread>());
	bundle.lane = std::make_shared<sscl::PuppetThread>(
		threadId,
		laneName,
		puppetLaneEntry,
		*bundle.component,
		nullptr);
	bundle.component->thread = bundle.lane;
	return bundle.lane;
}

} // namespace

std::shared_ptr<sscl::PuppeteerThread> mainPuppeteerLane;
std::shared_ptr<sscl::PuppetThread> bodyPuppetLane;
std::shared_ptr<sscl::PuppetThread> worldPuppetLane;
std::shared_ptr<sscl::PuppetThread> legPuppetLane;

void startSyncMainPuppetLanes()
{
	g_syncMainPuppetApplication = std::make_shared<sscl::PuppetApplication>(
		std::vector<std::shared_ptr<sscl::PuppetThread>>{});

	bodyPuppetLane = makeChildPuppetLane(
		bodyPuppetThreadId, "sync_main_body", g_childPuppetLaneBundles[0]);
	worldPuppetLane = makeChildPuppetLane(
		worldPuppetThreadId, "sync_main_world", g_childPuppetLaneBundles[1]);
	legPuppetLane = makeChildPuppetLane(
		legPuppetThreadId, "sync_main_leg", g_childPuppetLaneBundles[2]);

	g_mainPuppeteerComponent = std::make_unique<SyncMainPuppeteerComponent>(
		std::shared_ptr<sscl::PuppeteerThread>());
	sscl::ComponentThread::setPuppeteerThreadId(mainPuppeteerThreadId);
	mainPuppeteerLane = std::make_shared<sscl::PuppeteerThread>(
		mainPuppeteerThreadId,
		"sync_main_puppeteer",
		mainPuppeteerLaneEntry,
		*g_mainPuppeteerComponent,
		nullptr);
	g_mainPuppeteerComponent->thread = mainPuppeteerLane;
	sscl::ComponentThread::setPuppeteerThread(mainPuppeteerLane);

	{
		std::lock_guard<std::mutex> lock(g_laneStartMutex);
		g_puppetLanesConstructed = syncMainLaneCount;
	}
	g_laneStartCondition.notify_all();

	waitForPuppetLaneTlsReady();
}

void joinSyncMainPuppetLanes()
{
	if (mainPuppeteerLane) {
		mainPuppeteerLane->thread.join();
	}
	if (bodyPuppetLane) {
		bodyPuppetLane->thread.join();
	}
	if (worldPuppetLane) {
		worldPuppetLane->thread.join();
	}
	if (legPuppetLane) {
		legPuppetLane->thread.join();
	}
}
