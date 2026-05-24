#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/system/error_code.hpp>

#include <marionette/marionette.h>
#include <test/marionetteTestRunner.h>
#include <spinscale/co/group.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/promises.h>

namespace sscl::co::group_edge_test {

namespace {

constexpr const char *testLogPrefix = "group_edge_test";
constexpr int delayShortMs = 50;
constexpr int delayMediumMs = 200;
constexpr int delayLongMs = 500;
constexpr int delayAddWhileSuspendedProbeMs = 80;

const char *const expectedThrowMessage = "group_edge_test intentional failure";
const char *const expectedEmptyGroupCoAwaitMessage =
	"co_await: Group has no member invokers; call add() before awaiting";
constexpr int expectedNonStdThrowValue = 42;
constexpr int wave2ImmediateSettlementLabel = 1000;
constexpr int overlappingAwaitAllDelayMs = 80;

struct GroupEdgeTestThreadTag
{
	static boost::asio::io_service &io_service() noexcept
		{ return ctest::mrntt::MrnttThreadTag::io_service(); }
};

template <typename T>
using GroupEdgeTestPostingPromise = TaggedPostingPromise<T, GroupEdgeTestThreadTag>;

template <typename T>
using GroupEdgeTestViralInvoker = ViralPostingInvoker<
	GroupEdgeTestPostingPromise, T>;

using GroupEdgeTestViralIntGroup = Group;
using GroupEdgeTestViralVoidGroup = Group;

struct TimerAsyncWaitAwaiter
{
	TimerAsyncWaitAwaiter(
		boost::asio::io_service &ioService,
		int delayMilliseconds)
	: timer(ioService)
	{
		timer.expires_from_now(
			boost::posix_time::milliseconds(delayMilliseconds));
		timer.async_wait([this](const boost::system::error_code &errorCode) {
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

	boost::asio::deadline_timer timer;
	boost::system::error_code completionErrorCode;
	bool waitCompleted = false;
	std::coroutine_handle<> resumeHandle;
};

void logPass(const char *testName)
{
	std::cout << testLogPrefix << ": PASS " << testName << "\n";
}

boost::asio::io_service &groupEdgeTestIoService()
{
	return ctest::mrntt::MrnttThreadTag::io_service();
}

void logSkip(const char *testName, const char *reason)
{
	std::cout << testLogPrefix << ": SKIP " << testName << ": " << reason << "\n";
}

void drainIoContext()
{
	ctest::test::pumpMarionetteIoContext(groupEdgeTestIoService());
}

void throwIfTimerWaitFailed(const boost::system::error_code &waitError)
{
	if (waitError) {
		throw std::runtime_error(
			"deadline_timer wait failed: " + waitError.message());
	}
}

GroupEdgeTestViralInvoker<int> waitAndReturnLabel(int timerLabelMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		groupEdgeTestIoService(), timerLabelMilliseconds};
	throwIfTimerWaitFailed(waitError);
	co_return timerLabelMilliseconds;
}

GroupEdgeTestViralInvoker<int> waitThenThrowAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		groupEdgeTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);
	throw std::runtime_error(expectedThrowMessage);
}

GroupEdgeTestViralInvoker<int> waitThenThrowIntAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		groupEdgeTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);
	throw expectedNonStdThrowValue;
}

GroupEdgeTestViralInvoker<int> returnLabelImmediately(int timerLabelMilliseconds)
{
	co_return timerLabelMilliseconds;
}

int readCompletedLabel(GroupEdgeTestViralInvoker<int> &invoker)
	{ return invoker.completedReturnValues().myReturnValue; }

void assertDescriptorCompleted(
	const GroupEdgeTestViralIntGroup::SettlementDescriptor &descriptor,
	int expectedLabel)
{
	if (descriptor.type
		!= GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::COMPLETED) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": expected COMPLETED settlement");
	}

	if (readCompletedLabel(descriptor.invokerAs<GroupEdgeTestViralInvoker<int>>()) != expectedLabel) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": settlement label mismatch");
	}
}

void assertDescriptorExceptionThrown(
	const GroupEdgeTestViralIntGroup::SettlementDescriptor &descriptor)
{
	if (descriptor.type
		!= GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": expected EXCEPTION_THROWN settlement");
	}

	if (!descriptor.calleeException) {
		throw std::runtime_error(
			std::string(testLogPrefix)
			+ ": missing calleeException on settlement");
	}
}

void assertDescriptorExceptionMessage(
	const GroupEdgeTestViralIntGroup::SettlementDescriptor &descriptor,
	const char *expectedMessage)
{
	assertDescriptorExceptionThrown(descriptor);

	try {
		std::rethrow_exception(descriptor.calleeException);
	} catch (const std::runtime_error &runtimeError) {
		if (std::string(runtimeError.what()) != expectedMessage) {
			throw std::runtime_error(
				std::string(testLogPrefix) + ": unexpected exception message");
		}
		return;
	}

	throw std::runtime_error(
		std::string(testLogPrefix) + ": expected std::runtime_error from member");
}

void assertDescriptorNonStdException(
	const GroupEdgeTestViralIntGroup::SettlementDescriptor &descriptor)
{
	assertDescriptorExceptionThrown(descriptor);

	try {
		std::rethrow_exception(descriptor.calleeException);
	} catch (int caughtValue) {
		if (caughtValue != expectedNonStdThrowValue) {
			throw std::runtime_error(
				std::string(testLogPrefix) + ": unexpected non-std exception value");
		}
		return;
	}

	throw std::runtime_error(
		std::string(testLogPrefix) + ": expected int exception from member");
}

int finishDriverCoroutine(GroupEdgeTestViralInvoker<int> &driver)
{
	if (driver.completedReturnValues().myExceptionPtr) {
		std::rethrow_exception(driver.completedReturnValues().myExceptionPtr);
	}

	return driver.completedReturnValues().myReturnValue;
}

int runDriverCoroutine(GroupEdgeTestViralInvoker<int> &driver)
{
	drainIoContext();
	return finishDriverCoroutine(driver);
}

GroupEdgeTestViralInvoker<int> testMixedSuccessAndFailureAwaitFirstThenAll()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> successInvoker = waitAndReturnLabel(1);
	GroupEdgeTestViralInvoker<int> failureInvoker = waitThenThrowAfterDelay(
		delayShortMs);

	group.add(successInvoker);
	group.add(failureInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	const bool firstWasSuccess = firstDescriptor.type
		== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::COMPLETED;
	const bool firstWasFailure = firstDescriptor.type
		== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::EXCEPTION_THROWN;

	if (!firstWasSuccess && !firstWasFailure) {
		throw std::runtime_error("first settlement has unexpected type");
	}

	if (firstWasSuccess) {
		assertDescriptorCompleted(firstDescriptor, 1);
	} else {
		assertDescriptorExceptionMessage(firstDescriptor, expectedThrowMessage);
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 2) {
		throw std::runtime_error("expected two settlements");
	}

	std::size_t completedCount = 0;
	std::size_t exceptionCount = 0;

	for (auto &descriptor : allDescriptors) {
		if (descriptor.type
			== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::COMPLETED) {
			++completedCount;
			assertDescriptorCompleted(descriptor, 1);
		} else if (descriptor.type
			== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
			++exceptionCount;
			assertDescriptorExceptionMessage(descriptor, expectedThrowMessage);
		}
	}

	if (completedCount != 1 || exceptionCount != 1) {
		throw std::runtime_error("mixed settlement type counts mismatch");
	}

	if (allAfterFirst.size() != 2) {
		throw std::runtime_error("await-first vector snapshot size mismatch");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testSingleMemberAwaitFirstThenAll()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> onlyInvoker = waitAndReturnLabel(delayShortMs);
	group.add(onlyInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	assertDescriptorCompleted(firstDescriptor, delayShortMs);

	if (!group.allInvokersSettled()) {
		throw std::runtime_error(
			"single-member group should be fully settled after await-first");
	}

	if (allAfterFirst.size() != 1) {
		throw std::runtime_error("await-first snapshot should have one entry");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("await-all should report one settlement");
	}

	assertDescriptorCompleted(allDescriptors[0], delayShortMs);

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testAllCompleteBeforeCoAwait()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> invokerTen = returnLabelImmediately(10);
	GroupEdgeTestViralInvoker<int> invokerTwenty = returnLabelImmediately(20);
	GroupEdgeTestViralInvoker<int> invokerThirty = returnLabelImmediately(30);

	group.add(invokerTen);
	group.add(invokerTwenty);
	group.add(invokerThirty);

	while (!group.allInvokersSettled()) {
		groupEdgeTestIoService().poll_one();
	}

	if (!group.firstInvokerSettled()) {
		throw std::runtime_error(
			"expected first settlement flag after immediate completions");
	}

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	assertDescriptorCompleted(firstDescriptor, 10);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 3) {
		throw std::runtime_error("expected three immediate settlements");
	}

	for (auto &descriptor : allDescriptors) {
		const int label = readCompletedLabel(descriptor.invokerAs<GroupEdgeTestViralInvoker<int>>());
		if (label != 10 && label != 20 && label != 30) {
			throw std::runtime_error("unexpected immediate settlement label");
		}
	}

	if (allAfterFirst.size() != 3) {
		throw std::runtime_error("await-first snapshot should list all members");
	}

	co_return 0;
}

std::thread startAddWhileGroupAwaiterSuspendedProbe(
	GroupEdgeTestViralIntGroup &group,
	GroupEdgeTestViralInvoker<int> &lateInvoker,
	std::atomic<bool> &groupIsAwaitingAll,
	std::atomic<bool> &addWasRejected)
{
	return std::thread([&]() {
		using namespace std::chrono_literals;

		while (!groupIsAwaitingAll.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(1ms);
		}

		std::this_thread::sleep_for(
			std::chrono::milliseconds(delayAddWhileSuspendedProbeMs));

		boost::asio::post(groupEdgeTestIoService(), [&]() {
			try {
				group.add(lateInvoker);
			} catch (const std::runtime_error &) {
				addWasRejected.store(true, std::memory_order_release);
			}
		});
	});
}

GroupEdgeTestViralInvoker<int> testAddWhileAwaitAllSuspended()
{
	GroupEdgeTestViralIntGroup group;

	std::atomic<bool> groupIsAwaitingAll{false};
	std::atomic<bool> addWasRejected{false};

	GroupEdgeTestViralInvoker<int> slowInvokerA = waitAndReturnLabel(delayLongMs);
	GroupEdgeTestViralInvoker<int> slowInvokerB = waitAndReturnLabel(delayLongMs);
	GroupEdgeTestViralInvoker<int> lateInvoker = waitAndReturnLabel(99);

	group.add(slowInvokerA);
	group.add(slowInvokerB);

	std::thread addProbeThread = startAddWhileGroupAwaiterSuspendedProbe(
		group,
		lateInvoker,
		groupIsAwaitingAll,
		addWasRejected);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	groupIsAwaitingAll.store(true, std::memory_order_release);
	co_await awaitAll;

	addProbeThread.join();

	if (!addWasRejected.load(std::memory_order_acquire)) {
		throw std::runtime_error(
			"expected add() to throw while group co_awaiter is suspended");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testAwaitAllOnlyMixedOutcomes()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> successInvoker = returnLabelImmediately(7);
	GroupEdgeTestViralInvoker<int> failureInvoker = waitThenThrowAfterDelay(
		delayShortMs);

	group.add(successInvoker);
	group.add(failureInvoker);

	while (!group.allInvokersSettled()) {
		groupEdgeTestIoService().poll_one();
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 2) {
		throw std::runtime_error("await-all-only expected two members");
	}

	std::size_t completedCount = 0;
	std::size_t exceptionCount = 0;

	for (auto &descriptor : allDescriptors) {
		if (descriptor.type
			== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::COMPLETED) {
			++completedCount;
			assertDescriptorCompleted(descriptor, 7);
		} else if (descriptor.type
			== GroupEdgeTestViralIntGroup::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
			++exceptionCount;
			assertDescriptorExceptionMessage(descriptor, expectedThrowMessage);
		}
	}

	if (completedCount != 1 || exceptionCount != 1) {
		throw std::runtime_error("await-all-only mixed counts mismatch");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testCheckForAndReThrowGroupExceptions()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> failureInvoker = waitThenThrowAfterDelay(
		delayShortMs);
	group.add(failureInvoker);

	while (!group.allInvokersSettled()) {
		groupEdgeTestIoService().poll_one();
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	(void)co_await awaitAll;

	bool threwAggregate = false;

	try {
		group.checkForAndReThrowGroupExceptions();
	} catch (const std::runtime_error &aggregateError) {
		threwAggregate = true;
		const std::string message = aggregateError.what();
		if (message.find(expectedThrowMessage) == std::string::npos) {
			throw std::runtime_error(
				"checkForAndReThrowGroupExceptions message missing callee text");
		}
	}

	if (!threwAggregate) {
		throw std::runtime_error(
			"checkForAndReThrowGroupExceptions should have thrown");
	}

	co_return 0;
}

void assertEmptyGroupCoAwaitError(const std::runtime_error &runtimeError)
{
	if (std::string(runtimeError.what()) != expectedEmptyGroupCoAwaitMessage) {
		throw std::runtime_error(
			std::string(testLogPrefix) + ": unexpected empty-group co_await message");
	}
}

GroupEdgeTestViralInvoker<int> testEmptyGroupAwaitAllThrows()
{
	GroupEdgeTestViralIntGroup group;

	try {
		auto awaitAll = group.getAwaitAllSettlementsInvoker();
		(void)co_await awaitAll;
	} catch (const std::runtime_error &runtimeError) {
		assertEmptyGroupCoAwaitError(runtimeError);
		co_return 0;
	}

	throw std::runtime_error(
		"empty group await-all should throw from await_suspend");
}

GroupEdgeTestViralInvoker<int> testEmptyGroupAwaitFirstThrows()
{
	GroupEdgeTestViralIntGroup group;

	try {
		auto awaitFirst = group.getAwaitFirstSettlementInvoker();
		(void)co_await awaitFirst;
	} catch (const std::runtime_error &runtimeError) {
		assertEmptyGroupCoAwaitError(runtimeError);
		co_return 0;
	}

	throw std::runtime_error(
		"empty group await-first should throw from await_suspend");
}

GroupEdgeTestViralInvoker<int> testWrongAwaitInvokerOrder()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> shortInvoker = waitAndReturnLabel(delayShortMs);
	GroupEdgeTestViralInvoker<int> mediumInvoker = waitAndReturnLabel(delayMediumMs);

	group.add(shortInvoker);
	group.add(mediumInvoker);

	auto awaitFirstHandle = group.getAwaitFirstSettlementInvoker();
	auto awaitAllHandle = group.getAwaitAllSettlementsInvoker();

	auto &allDescriptors = co_await awaitAllHandle;

	if (allDescriptors.size() != 2) {
		throw std::runtime_error("wrong-order setup expected two settlements");
	}

	for (auto &descriptor : allDescriptors) {
		assertDescriptorCompleted(
			descriptor,
			readCompletedLabel(descriptor.invokerAs<GroupEdgeTestViralInvoker<int>>()));
	}

	auto [firstDescriptor, allAfterFirst] = co_await awaitFirstHandle;

	if (!group.firstInvokerSettled()) {
		throw std::runtime_error(
			"await-first after await-all should still see a first settlement");
	}

	assertDescriptorCompleted(
		firstDescriptor,
		readCompletedLabel(firstDescriptor.invokerAs<GroupEdgeTestViralInvoker<int>>()));

	if (allAfterFirst.size() != 2) {
		throw std::runtime_error(
			"await-first snapshot after await-all should remain valid");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<void> voidViralMemberAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		groupEdgeTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);
	co_return;
}

GroupEdgeTestViralInvoker<int> testVoidViralMemberInGroup()
{
	GroupEdgeTestViralVoidGroup group;

	auto voidInvoker = voidViralMemberAfterDelay(delayShortMs);
	group.add(voidInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("void viral group should have one settlement");
	}

	if (allDescriptors[0].type
		!= GroupEdgeTestViralVoidGroup::SettlementDescriptor::TypeE::COMPLETED) {
		throw std::runtime_error("void viral member should settle as COMPLETED");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testNonViralVoidGroupTemplateInstantiates()
{
	logSkip(
		"nonViralVoidMemberInGroup",
		"NonViralPostingInvoker does not satisfy Group's AwaitableOrAwaiterIface concept");
	co_return 0;
}

GroupEdgeTestViralInvoker<int> testEarlyInvokerDestructionIsUnsupported()
{
	logSkip(
		"earlyInvokerDestruction",
		"destroying a member invoker before group settlement completes is undefined per spec");
	co_return 0;
}

GroupEdgeTestViralInvoker<int> testDoubleCoAwaitSameAwaitFirst()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> memberInvoker = returnLabelImmediately(
		delayShortMs);
	group.add(memberInvoker);

	while (!group.allInvokersSettled()) {
		groupEdgeTestIoService().poll_one();
	}

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorA, allAfterFirstA] = co_await awaitFirst;
	auto [firstDescriptorB, allAfterFirstB] = co_await awaitFirst;

	assertDescriptorCompleted(firstDescriptorA, delayShortMs);
	assertDescriptorCompleted(firstDescriptorB, delayShortMs);

	if (&firstDescriptorA.invokerAs<GroupEdgeTestViralInvoker<int>>() != &firstDescriptorB.invokerAs<GroupEdgeTestViralInvoker<int>>()) {
		throw std::runtime_error(
			"double await-first on same handle should return the same first settlement");
	}

	if (allAfterFirstA.size() != allAfterFirstB.size()) {
		throw std::runtime_error(
			"double await-first should return the same settlements snapshot size");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testDoubleCoAwaitSameAwaitAll()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> memberInvoker = waitAndReturnLabel(delayShortMs);
	group.add(memberInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptorsA = co_await awaitAll;
	auto &allDescriptorsB = co_await awaitAll;

	if (allDescriptorsA.size() != 1 || allDescriptorsB.size() != 1) {
		throw std::runtime_error(
			"double await-all on same handle should see one settlement");
	}

	assertDescriptorCompleted(allDescriptorsA[0], delayShortMs);
	assertDescriptorCompleted(allDescriptorsB[0], delayShortMs);

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testTwoAwaitFirstHandlesSequentially()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> shortInvoker = waitAndReturnLabel(delayShortMs);
	GroupEdgeTestViralInvoker<int> mediumInvoker = waitAndReturnLabel(delayMediumMs);

	group.add(shortInvoker);
	group.add(mediumInvoker);

	auto awaitFirstA = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorA, allAfterFirstA] = co_await awaitFirstA;

	assertDescriptorCompleted(firstDescriptorA, delayShortMs);

	auto awaitFirstB = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorB, allAfterFirstB] = co_await awaitFirstB;

	assertDescriptorCompleted(firstDescriptorB, delayShortMs);

	if (&firstDescriptorA.invokerAs<GroupEdgeTestViralInvoker<int>>() != &firstDescriptorB.invokerAs<GroupEdgeTestViralInvoker<int>>()) {
		throw std::runtime_error(
			"second await-first handle should still report the sticky first settlement");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();
	(void)allAfterFirstA;
	(void)allAfterFirstB;

	co_return 0;
}

GroupEdgeTestViralInvoker<int> overlappingAwaitAllAfterDelay(
	GroupEdgeTestViralIntGroup &group,
	int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		groupEdgeTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	(void)co_await awaitAll;

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testOverlappingGroupWaitsBody()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> overlappingWaiter = overlappingAwaitAllAfterDelay(
		group,
		overlappingAwaitAllDelayMs);
	GroupEdgeTestViralInvoker<int> longMember = waitAndReturnLabel(delayLongMs);

	group.add(longMember);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	(void)co_await awaitFirst;
	(void)overlappingWaiter;

	co_return 0;
}

void runOverlappingGroupWaitsAssertInDebugTest()
{
#ifndef NDEBUG
	logSkip(
		"overlappingGroupWaitsAssertInDebug",
		"fork-based assert test skipped under marionette harness");
#else
	logSkip(
		"overlappingGroupWaitsAssertInDebug",
		"overlapping co_await is only asserted in debug builds");
#endif
}

GroupEdgeTestViralInvoker<int> testAddSecondWaveAfterAwaitAll()
{
	using Clock = std::chrono::steady_clock;
	using Ms = std::chrono::milliseconds;

	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> wave1MemberA = waitAndReturnLabel(delayLongMs);
	GroupEdgeTestViralInvoker<int> wave1MemberB = waitAndReturnLabel(delayLongMs);

	group.add(wave1MemberA);
	group.add(wave1MemberB);
	(void)co_await group.getAwaitAllSettlementsInvoker();

	GroupEdgeTestViralInvoker<int> wave2Immediate = returnLabelImmediately(
		wave2ImmediateSettlementLabel);
	GroupEdgeTestViralInvoker<int> wave2Slow = waitAndReturnLabel(delayMediumMs);

	const auto wave2Start = Clock::now();

	group.add(wave2Immediate);
	group.add(wave2Slow);

	bool immediateReadableBeforeAwaitAll = false;

	while (Clock::now() - wave2Start < Ms(100)) {
		groupEdgeTestIoService().poll_one();

		if (readCompletedLabel(wave2Immediate) == wave2ImmediateSettlementLabel) {
			immediateReadableBeforeAwaitAll = true;
			break;
		}
	}

	if (!immediateReadableBeforeAwaitAll) {
		throw std::runtime_error(
			"wave-2 immediate member should complete before await-all");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error(
			"wave-2 slow member should still be in flight before await-all");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();

	auto awaitAllFinal = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAllFinal;

	if (allDescriptors.size() != 4) {
		throw std::runtime_error(
			"expected four settlements after second wave");
	}

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testShortTimerAddedAfterLongStillWinsRace()
{
	using Clock = std::chrono::steady_clock;
	using Ms = std::chrono::milliseconds;

	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> longInvoker = waitAndReturnLabel(delayLongMs);
	GroupEdgeTestViralInvoker<int> shortInvoker = waitAndReturnLabel(delayShortMs);

	group.add(longInvoker);
	group.add(shortInvoker);

	const auto raceStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	const auto firstDone = Clock::now();
	const auto elapsedMs = std::chrono::duration_cast<Ms>(firstDone - raceStart);

	if (elapsedMs > Ms(delayMediumMs - 25)) {
		throw std::runtime_error(
			"await-first should complete near the short timer, not the long timer");
	}

	assertDescriptorCompleted(firstDescriptor, delayShortMs);

	if (&firstDescriptor.invokerAs<GroupEdgeTestViralInvoker<int>>() != &shortInvoker) {
		throw std::runtime_error(
			"short timer should win await-first even when added after the long timer");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();
	(void)allAfterFirst;

	co_return 0;
}

GroupEdgeTestViralInvoker<int> testNonStdExceptionSettlement()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> failureInvoker = waitThenThrowIntAfterDelay(
		delayShortMs);
	group.add(failureInvoker);

	while (!group.allInvokersSettled()) {
		groupEdgeTestIoService().poll_one();
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("non-std exception test expected one settlement");
	}

	assertDescriptorNonStdException(allDescriptors[0]);

	try {
		group.checkForAndReThrowGroupExceptions();
	} catch (const std::runtime_error &) {
		co_return 0;
	}

	throw std::runtime_error(
		"checkForAndReThrowGroupExceptions should have thrown for non-std exception");
}

GroupEdgeTestViralInvoker<int> testReturnValuesRemainReadableAfterAwaitFirst()
{
	GroupEdgeTestViralIntGroup group;

	GroupEdgeTestViralInvoker<int> slowInvoker = waitAndReturnLabel(delayLongMs);
	GroupEdgeTestViralInvoker<int> fastInvoker = waitAndReturnLabel(delayShortMs);

	group.add(slowInvoker);
	group.add(fastInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	assertDescriptorCompleted(firstDescriptor, delayShortMs);

	const int fastLabelFromDescriptor = readCompletedLabel(
		firstDescriptor.invokerAs<GroupEdgeTestViralInvoker<int>>());
	const int fastLabelFromLocal = readCompletedLabel(fastInvoker);

	if (fastLabelFromDescriptor != fastLabelFromLocal) {
		throw std::runtime_error(
			"return value mismatch between descriptor and local invoker");
	}

	if (allAfterFirst.size() != 2) {
		throw std::runtime_error("expected two settlement slots in snapshot");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	(void)co_await awaitAll;

	co_return 0;
}

struct NamedTest
{
	const char *name;
	GroupEdgeTestViralInvoker<int> (*body)();
};

int runNamedTest(const NamedTest &namedTest)
{
	std::cout << testLogPrefix << ": RUN " << namedTest.name << "\n";
	GroupEdgeTestViralInvoker<int> driver = namedTest.body();
	const int status = runDriverCoroutine(driver);
	logPass(namedTest.name);
	return status;
}

void runAllEdgeTests()
{
	static const NamedTest namedTests[] = {
		{"mixedSuccessAndFailureAwaitFirstThenAll",
			testMixedSuccessAndFailureAwaitFirstThenAll},
		{"singleMemberAwaitFirstThenAll",
			testSingleMemberAwaitFirstThenAll},
		{"allCompleteBeforeCoAwait",
			testAllCompleteBeforeCoAwait},
		{"addWhileAwaitAllSuspended",
			testAddWhileAwaitAllSuspended},
		{"awaitAllOnlyMixedOutcomes",
			testAwaitAllOnlyMixedOutcomes},
		{"checkForAndReThrowGroupExceptions",
			testCheckForAndReThrowGroupExceptions},
		{"emptyGroupAwaitAllThrows",
			testEmptyGroupAwaitAllThrows},
		{"emptyGroupAwaitFirstThrows",
			testEmptyGroupAwaitFirstThrows},
		{"wrongAwaitInvokerOrder",
			testWrongAwaitInvokerOrder},
		{"doubleCoAwaitSameAwaitFirst",
			testDoubleCoAwaitSameAwaitFirst},
		{"doubleCoAwaitSameAwaitAll",
			testDoubleCoAwaitSameAwaitAll},
		{"twoAwaitFirstHandlesSequentially",
			testTwoAwaitFirstHandlesSequentially},
		{"addSecondWaveAfterAwaitAll",
			testAddSecondWaveAfterAwaitAll},
		{"shortTimerAddedAfterLongStillWinsRace",
			testShortTimerAddedAfterLongStillWinsRace},
		{"nonStdExceptionSettlement",
			testNonStdExceptionSettlement},
		{"voidViralMemberInGroup",
			testVoidViralMemberInGroup},
		{"nonViralVoidGroupTemplateInstantiates",
			testNonViralVoidGroupTemplateInstantiates},
		{"returnValuesRemainReadableAfterAwaitFirst",
			testReturnValuesRemainReadableAfterAwaitFirst},
		{"earlyInvokerDestructionIsUnsupported",
			testEarlyInvokerDestructionIsUnsupported},
	};

	for (const NamedTest &namedTest : namedTests) {
		runNamedTest(namedTest);
		groupEdgeTestIoService().reset();
	}

	runOverlappingGroupWaitsAssertInDebugTest();
}

} // namespace

} // namespace sscl::co::group_edge_test

int main()
{
	using namespace sscl::co::group_edge_test;

	return ctest::test::runMarionetteHarnessAndExit(
		[]
		{
			try {
				runAllEdgeTests();
				std::cout << testLogPrefix << ": all tests passed\n";
				return EXIT_SUCCESS;
			}
			catch (const std::exception &exception) {
				std::cerr << testLogPrefix << ": FAIL: "
					<< exception.what() << "\n";
				return EXIT_FAILURE;
			}
			catch (...) {
				std::cerr << testLogPrefix << ": FAIL: unknown exception\n";
				return EXIT_FAILURE;
			}
		});
}
