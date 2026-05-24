#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/error.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/system/error_code.hpp>

#include <marionette/marionette.h>
#include <test/marionetteTestRunner.h>
#include <spinscale/co/group.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>

namespace sscl::co::group_timer_test {

namespace {

constexpr const char *testLogPrefix = "group_timer_test";
constexpr int timerDelayShortMs = 50;
constexpr int timerDelayMediumMs = 200;
constexpr int timerDelayLongMs = 500;
constexpr int awaitAllTimingSlackMs = 25;
constexpr int awaitAllLongCancelTimingMarginMs = 50;

boost::asio::io_service &groupTimerTestIoService()
{
	return ctest::mrntt::MrnttThreadTag::io_service();
}

struct GroupTimerTestThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return groupTimerTestIoService(); }
};

template <typename T>
using GroupTimerTestPostingPromise = TaggedPostingPromise<T, GroupTimerTestThreadTag>;

template <typename T>
using GroupTimerTestViralInvoker = ViralPostingInvoker<
	GroupTimerTestPostingPromise, T>;

using GroupTimerTestIntGroup = Group<GroupTimerTestViralInvoker<int>>;

using CancelableDeadlineTimer = std::shared_ptr<boost::asio::deadline_timer>;

std::unordered_map<int, std::weak_ptr<boost::asio::deadline_timer>>
	cancelableDeadlineTimersByLabel;

void registerCancelableDeadlineTimer(
	int labelMilliseconds,
	const CancelableDeadlineTimer &timer)
{
	cancelableDeadlineTimersByLabel[labelMilliseconds] = timer;
}

void cancelDeadlineTimer(int labelMilliseconds)
{
	const auto iterator = cancelableDeadlineTimersByLabel.find(labelMilliseconds);

	if (iterator == cancelableDeadlineTimersByLabel.end()) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": no cancelable deadline_timer registered for label "
			+ std::to_string(labelMilliseconds));
	}

	const CancelableDeadlineTimer timer = iterator->second.lock();

	if (!timer) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": cancelable deadline_timer expired before cancel for label "
			+ std::to_string(labelMilliseconds));
	}

	timer->cancel();
}

struct TimerAsyncWaitAwaiter
{
	TimerAsyncWaitAwaiter(
		int delayMilliseconds,
		const CancelableDeadlineTimer &sharedTimer)
	: timer(sharedTimer)
	{
		timer->expires_from_now(
			boost::posix_time::milliseconds(delayMilliseconds));
		timer->async_wait([this](const boost::system::error_code &errorCode) {
			completionErrorCode = errorCode;
			waitCompleted = true;
			if (resumeHandle) {
				resumeHandle.resume();
			}
		});
	}

	bool await_ready() const noexcept
		{ return waitCompleted; }

	bool await_suspend(std::coroutine_handle<> handle) noexcept
	{
		resumeHandle = handle;
		return !waitCompleted;
	}

	boost::system::error_code await_resume() const noexcept
		{ return completionErrorCode; }

	CancelableDeadlineTimer timer;
	boost::system::error_code completionErrorCode;
	bool waitCompleted = false;
	std::coroutine_handle<> resumeHandle;
};

struct RegisteredTimerAsyncWaitAwaiter
{
	RegisteredTimerAsyncWaitAwaiter(
		boost::asio::io_context &ioContext,
		int delayMilliseconds,
		int registrationLabelMilliseconds)
	: timer(std::make_shared<boost::asio::deadline_timer>(ioContext))
	{
		registerCancelableDeadlineTimer(registrationLabelMilliseconds, timer);
		waiter.emplace(delayMilliseconds, timer);
	}

	bool await_ready() const noexcept
		{ return waiter->await_ready(); }

	bool await_suspend(std::coroutine_handle<> handle) noexcept
		{ return waiter->await_suspend(handle); }

	boost::system::error_code await_resume() const noexcept
		{ return waiter->await_resume(); }

	CancelableDeadlineTimer timer;
	std::optional<TimerAsyncWaitAwaiter> waiter;
};

GroupTimerTestViralInvoker<int> waitDeadlineTimer(int timerLabelMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		timerLabelMilliseconds,
		std::make_shared<boost::asio::deadline_timer>(groupTimerTestIoService())};

	if (waitError) {
		throw std::runtime_error(
			"deadline_timer wait failed: " + waitError.message());
	}

	co_return timerLabelMilliseconds;
}

GroupTimerTestViralInvoker<int> waitCancelableDeadlineTimer(
	int timerLabelMilliseconds)
{
	const boost::system::error_code waitError = co_await RegisteredTimerAsyncWaitAwaiter{
		groupTimerTestIoService(),
		timerLabelMilliseconds,
		timerLabelMilliseconds};

	if (waitError == boost::asio::error::operation_aborted) {
		co_return timerLabelMilliseconds;
	}

	if (waitError) {
		throw std::runtime_error(
			"deadline_timer wait failed: " + waitError.message());
	}

	co_return timerLabelMilliseconds;
}

int settlementReturnLabel(GroupTimerTestViralInvoker<int> &invoker)
	{ return invoker.completedReturnValues().myReturnValue; }

void assertSettlementCompleted(
	const GroupTimerTestIntGroup::SettlementDescriptor &descriptor,
	int expectedLabelMilliseconds)
{
	if (descriptor.type
		!= GroupTimerTestIntGroup::SettlementDescriptor::TypeE::COMPLETED) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": expected COMPLETED settlement for timer "
			+ std::to_string(expectedLabelMilliseconds) + "ms");
	}

	const int label = settlementReturnLabel(descriptor.invoker.get());

	if (label != expectedLabelMilliseconds) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": timer label mismatch: expected "
			+ std::to_string(expectedLabelMilliseconds) + ", got "
			+ std::to_string(label));
	}
}

void finishDriverCoroutine(GroupTimerTestViralInvoker<int> &driver)
{
	if (driver.completedReturnValues().myExceptionPtr) {
		std::rethrow_exception(driver.completedReturnValues().myExceptionPtr);
	}
}

void runDriverCoroutine(GroupTimerTestViralInvoker<int> &driver)
{
	ctest::test::pumpMarionetteIoContext(groupTimerTestIoService());
	finishDriverCoroutine(driver);
}

void logPass(const char *testName)
{
	std::cout << testLogPrefix << ": PASS " << testName << "\n";
}

GroupTimerTestViralInvoker<int> runGroupTimerRaceTest()
{
	using Clock = std::chrono::steady_clock;
	using Ms = std::chrono::milliseconds;

	GroupTimerTestIntGroup group;

	GroupTimerTestViralInvoker<int> invokerShort = waitDeadlineTimer(
		timerDelayShortMs);
	GroupTimerTestViralInvoker<int> invokerMedium = waitDeadlineTimer(
		timerDelayMediumMs);
	GroupTimerTestViralInvoker<int> invokerLong = waitDeadlineTimer(
		timerDelayLongMs);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	std::cout << testLogPrefix << ": race: awaiting first settlement (expect "
		<< timerDelayShortMs << "ms timer)\n";

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;

	const auto firstDone = Clock::now();
	const auto firstElapsedMs = std::chrono::duration_cast<Ms>(firstDone - testStart);

	if (firstElapsedMs > Ms(timerDelayMediumMs - awaitAllTimingSlackMs)) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": await-first took too long: "
			+ std::to_string(firstElapsedMs.count()) + "ms");
	}

	assertSettlementCompleted(firstSettlement, timerDelayShortMs);

	if (&firstSettlement.invoker.get() != &invokerShort) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": first settlement was not the shortest timer invoker");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": await-first returned but all invokers are already settled");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allSettlements = co_await awaitAll;

	const auto allDone = Clock::now();
	const auto allElapsedMs = std::chrono::duration_cast<Ms>(allDone - testStart);

	if (allElapsedMs < Ms(timerDelayLongMs - awaitAllLongCancelTimingMarginMs)) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": await-all finished too soon: "
			+ std::to_string(allElapsedMs.count()) + "ms");
	}

	if (allSettlements.size() != 3) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": expected 3 settlements, got "
			+ std::to_string(allSettlements.size()));
	}

	for (auto &descriptor : allSettlements) {
		assertSettlementCompleted(
			descriptor,
			settlementReturnLabel(descriptor.invoker.get()));
	}

	assertSettlementCompleted(allSettlementsAfterFirst[0], timerDelayShortMs);
	assertSettlementCompleted(allSettlementsAfterFirst[1], timerDelayMediumMs);
	assertSettlementCompleted(allSettlementsAfterFirst[2], timerDelayLongMs);

	co_return 0;
}

GroupTimerTestViralInvoker<int> runGroupTimerCancelLongAfterAwaitFirst()
{
	using Clock = std::chrono::steady_clock;
	using Ms = std::chrono::milliseconds;

	GroupTimerTestIntGroup group;

	GroupTimerTestViralInvoker<int> invokerShort = waitCancelableDeadlineTimer(
		timerDelayShortMs);
	GroupTimerTestViralInvoker<int> invokerMedium = waitCancelableDeadlineTimer(
		timerDelayMediumMs);
	GroupTimerTestViralInvoker<int> invokerLong = waitCancelableDeadlineTimer(
		timerDelayLongMs);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;

	assertSettlementCompleted(firstSettlement, timerDelayShortMs);

	if (&firstSettlement.invoker.get() != &invokerShort) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": cancel test: first settlement was not the shortest timer");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": cancel test: await-first returned with all invokers settled");
	}

	cancelDeadlineTimer(timerDelayLongMs);

	std::cout << testLogPrefix
		<< ": cancel: canceled long deadline_timer; awaiting all settlements "
		<< "(expect near " << timerDelayMediumMs << "ms, not "
		<< timerDelayLongMs << "ms)\n";

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allSettlements = co_await awaitAll;

	const auto allDone = Clock::now();
	const auto allElapsedMs = std::chrono::duration_cast<Ms>(allDone - testStart);

	if (allElapsedMs >= Ms(timerDelayLongMs - awaitAllLongCancelTimingMarginMs)) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": await-all waited for uncanceled long timer: "
			+ std::to_string(allElapsedMs.count()) + "ms");
	}

	if (allElapsedMs < Ms(timerDelayMediumMs - awaitAllTimingSlackMs)) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": await-all finished before medium timer: "
			+ std::to_string(allElapsedMs.count()) + "ms");
	}

	if (allSettlements.size() != 3) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": cancel test: expected 3 settlements");
	}

	assertSettlementCompleted(allSettlements[0], timerDelayShortMs);
	assertSettlementCompleted(allSettlements[1], timerDelayMediumMs);
	assertSettlementCompleted(allSettlements[2], timerDelayLongMs);

	if (&allSettlements[2].invoker.get() != &invokerLong) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": cancel test: long settlement invoker mismatch");
	}

	(void)allSettlementsAfterFirst;
	(void)invokerMedium;

	co_return 0;
}

struct NamedTimerTest
{
	const char *name;
	GroupTimerTestViralInvoker<int> (*body)();
};

void runNamedTimerTest(const NamedTimerTest &namedTimerTest)
{
	std::cout << testLogPrefix << ": RUN " << namedTimerTest.name << "\n";
	cancelableDeadlineTimersByLabel.clear();

	GroupTimerTestViralInvoker<int> driver = namedTimerTest.body();
	runDriverCoroutine(driver);

	logPass(namedTimerTest.name);
	groupTimerTestIoService().reset();
}

void runAllTimerTests()
{
	static const NamedTimerTest namedTimerTests[] = {
		{"race", runGroupTimerRaceTest},
		{"cancelLongAfterAwaitFirst", runGroupTimerCancelLongAfterAwaitFirst},
	};

	for (const NamedTimerTest &namedTimerTest : namedTimerTests) {
		runNamedTimerTest(namedTimerTest);
	}
}

} // namespace

} // namespace sscl::co::group_timer_test

int main()
{
	using namespace sscl::co::group_timer_test;

	return ctest::test::runMarionetteHarnessAndExit(
		[]
		{
			try {
				runAllTimerTests();
				std::cout << "group_timer_test: all tests passed\n";
				return EXIT_SUCCESS;
			}
			catch (const std::exception &exception) {
				std::cerr << "group_timer_test: FAIL: "
					<< exception.what() << "\n";
				return EXIT_FAILURE;
			}
			catch (...) {
				std::cerr << "group_timer_test: FAIL: unknown exception\n";
				return EXIT_FAILURE;
			}
		});
}
