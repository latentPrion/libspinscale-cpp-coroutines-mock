#ifndef CHILD_THREADS_H
#define CHILD_THREADS_H

#include <memory>

#include <boost/asio/io_service.hpp>
#include <spinscale/componentThread.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>

extern std::shared_ptr<sscl::PuppeteerThread> mainPuppeteerLane;
extern std::shared_ptr<sscl::PuppetThread> bodyPuppetLane;
extern std::shared_ptr<sscl::PuppetThread> worldPuppetLane;
extern std::shared_ptr<sscl::PuppetThread> legPuppetLane;

struct MainPuppeteerThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return mainPuppeteerLane->getIoService(); }
};
template <typename T>
using MainPuppeteerJoltCReqPostingPromise =
	sscl::PuppetThreadCReqPostingPromise<T, true, MainPuppeteerThreadTag>;
using MainPuppeteerJoltThreadCReqInvokerFor =
	sscl::JoltThreadCReqInvokerFor<MainPuppeteerThreadTag>;

struct BodyThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return bodyPuppetLane->getIoService(); }
};
template <typename T>
using BodyPostingPromise = sscl::co::TaggedPostingPromise<T, BodyThreadTag>;
using BodyNonViralNonSuspendingInvoker =
	sscl::co::NonViralNonSuspendingInvoker<BodyPostingPromise>;
template <typename T>
using BodyViralInvoker = sscl::co::ViralSuspendingInvoker<BodyPostingPromise, T>;
template <typename T>
using BodyPuppetThreadCReqPostingPromise =
	sscl::PuppetThreadCReqPostingPromise<T, false, BodyThreadTag>;
using BodyStartThreadCReqInvokerFor =
	sscl::StartThreadCReqInvokerFor<BodyThreadTag>;
using BodyPauseThreadCReqInvokerFor =
	sscl::PauseThreadCReqInvokerFor<BodyThreadTag>;

struct WorldThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return worldPuppetLane->getIoService(); }
};
template <typename T>
using WorldPostingPromise = sscl::co::TaggedPostingPromise<T, WorldThreadTag>;
using WorldNonViralNonSuspendingInvoker =
	sscl::co::NonViralNonSuspendingInvoker<WorldPostingPromise>;
template <typename T>
using WorldViralInvoker = sscl::co::ViralSuspendingInvoker<WorldPostingPromise, T>;
template <typename T>
using WorldPuppetThreadCReqPostingPromise =
	sscl::PuppetThreadCReqPostingPromise<T, false, WorldThreadTag>;
using WorldStartThreadCReqInvokerFor =
	sscl::StartThreadCReqInvokerFor<WorldThreadTag>;
using WorldPauseThreadCReqInvokerFor =
	sscl::PauseThreadCReqInvokerFor<WorldThreadTag>;

struct LegThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return legPuppetLane->getIoService(); }
};
template <typename T>
using LegPostingPromise = sscl::co::TaggedPostingPromise<T, LegThreadTag>;
using LegNonViralNonSuspendingInvoker =
	sscl::co::NonViralNonSuspendingInvoker<LegPostingPromise>;
template <typename T>
using LegViralInvoker = sscl::co::ViralSuspendingInvoker<LegPostingPromise, T>;
template <typename T>
using LegPuppetThreadCReqPostingPromise =
	sscl::PuppetThreadCReqPostingPromise<T, false, LegThreadTag>;
using LegStartThreadCReqInvokerFor =
	sscl::StartThreadCReqInvokerFor<LegThreadTag>;
using LegPauseThreadCReqInvokerFor =
	sscl::PauseThreadCReqInvokerFor<LegThreadTag>;

void startSyncMainPuppetLanes();

void joinSyncMainPuppetLanes();

#endif // CHILD_THREADS_H
