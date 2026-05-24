#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/system/error_code.hpp>

#include <spinscale/co/group.h>
#include <spinscale/co/invokers.h>
#include <spinscale/componentThread.h>

#include <marionette/marionette.h>
#include <test/marionetteTestRunner.h>

namespace sscl::co::viral_non_posting_test {

namespace {

constexpr const char *testLogPrefix = "viral_non_posting_test";
constexpr int delayShortMs = 50;
constexpr int expectedNonStdThrowValue = 42;
const char *const expectedThrowMessage =
	"viral_non_posting_test intentional failure";

template <typename T>
using TestViralNonPostingInvoker = ViralNonPostingInvoker<T>;

using TestDriver = TestViralNonPostingInvoker<int>;
using TestVoidDriver = TestViralNonPostingInvoker<void>;
using TestIntGroup = Group<TestViralNonPostingInvoker<int>>;
using TestVoidGroup = Group<TestViralNonPostingInvoker<void>>;

struct ThreadIdPair
{
	std::thread::id callerIdAtCoAwait;
	std::thread::id calleeId;
};

struct MoveCountedInt
{
	std::shared_ptr<std::size_t> moveCount;
	int value = 0;

	MoveCountedInt() = default;

	MoveCountedInt(std::shared_ptr<std::size_t> moveCountIn, int valueIn)
	: moveCount(std::move(moveCountIn)),
	value(valueIn)
	{}

	MoveCountedInt(const MoveCountedInt &) = delete;
	MoveCountedInt &operator=(const MoveCountedInt &) = delete;

	MoveCountedInt(MoveCountedInt &&other) noexcept
	: moveCount(std::exchange(other.moveCount, {})),
	value(other.value)
	{
		if (moveCount) {
			++(*moveCount);
		}
	}

	MoveCountedInt &operator=(MoveCountedInt &&other) noexcept
	{
		moveCount = std::exchange(other.moveCount, {});
		value = other.value;
		return *this;
	}
};

template <typename T>
struct CountingViralNonPostingAwaiter
{
	TestViralNonPostingInvoker<T> &invoker;
	std::size_t &awaitResumeCallCount;

	bool await_ready() const noexcept
		{ return invoker.await_ready(); }

	template <typename CallerPromise>
	bool await_suspend(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
		{ return invoker.await_suspend(callerSchedHandle); }

	auto await_resume()
	{
		++awaitResumeCallCount;
		return invoker.await_resume();
	}
};

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

boost::asio::io_service &standaloneTestIoService()
{
	static boost::asio::io_service ioService;
	return ioService;
}

boost::asio::io_service &currentTestIoService()
{
	if (sscl::ComponentThread::tlsInitialized()) {
		return sscl::ComponentThread::getSelf()->getIoService();
	}

	return standaloneTestIoService();
}

void pumpIoContext(
	std::chrono::milliseconds maxIdleTime = std::chrono::milliseconds(800),
	std::chrono::milliseconds maxTotalTime = std::chrono::milliseconds(10000))
{
	if (sscl::ComponentThread::tlsInitialized()) {
		ctest::test::pumpMarionetteIoContext(
			currentTestIoService(), maxIdleTime, maxTotalTime);
		return;
	}

	boost::asio::io_service &io = currentTestIoService();
	const auto totalDeadline =
		std::chrono::steady_clock::now() + maxTotalTime;
	auto lastProgress = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() < totalDeadline)
	{
		if (io.poll_one() > 0)
		{
			lastProgress = std::chrono::steady_clock::now();
			continue;
		}

		if (std::chrono::steady_clock::now() - lastProgress >= maxIdleTime) {
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void resetTestIoService()
{
	currentTestIoService().reset();
}

void throwIfTimerWaitFailed(const boost::system::error_code &waitError)
{
	if (waitError) {
		throw std::runtime_error(
			"deadline_timer wait failed: " + waitError.message());
	}
}

void logPass(const char *testName)
{
	std::cout << testLogPrefix << ": PASS " << testName << "\n";
}

TestViralNonPostingInvoker<int> returnLabelImmediately(int label)
{
	co_return label;
}

TestViralNonPostingInvoker<int> waitAndReturnLabel(int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		currentTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);
	co_return delayMilliseconds;
}

TestVoidDriver voidMemberAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		currentTestIoService(), delayMilliseconds};
	throwIfTimerWaitFailed(waitError);
	co_return;
}

TestViralNonPostingInvoker<int> throwRuntimeErrorImmediately()
{
	throw std::runtime_error(expectedThrowMessage);
}

TestViralNonPostingInvoker<int> throwIntImmediately()
{
	throw expectedNonStdThrowValue;
}

TestViralNonPostingInvoker<ThreadIdPair> recordThreadIdsAtReturn()
{
	ThreadIdPair pair;
	pair.calleeId = std::this_thread::get_id();
	co_return pair;
}

TestViralNonPostingInvoker<ThreadIdPair> recordThreadIdsAfterDelay(int delayMs)
{
	const boost::system::error_code waitError = co_await TimerAsyncWaitAwaiter{
		currentTestIoService(), delayMs};
	throwIfTimerWaitFailed(waitError);
	ThreadIdPair pair;
	pair.calleeId = std::this_thread::get_id();
	co_return pair;
}

TestViralNonPostingInvoker<MoveCountedInt> returnMoveCountedInt(
	std::shared_ptr<std::size_t> moveCount,
	int value)
{
	co_return MoveCountedInt{std::move(moveCount), value};
}

TestViralNonPostingInvoker<int> innerDelayedCoAwait(int delayMilliseconds)
{
	const int label = co_await waitAndReturnLabel(delayMilliseconds);
	co_return label;
}

TestViralNonPostingInvoker<int> nestedNonPostingSum(int left, int right)
{
	const int leftSum = co_await returnLabelImmediately(left);
	const int rightSum = co_await returnLabelImmediately(right);
	co_return leftSum + rightSum;
}

TestViralNonPostingInvoker<int> outerCoAwaitingDelayedInner(int delayMilliseconds)
{
	const int innerLabel = co_await innerDelayedCoAwait(delayMilliseconds);
	co_return innerLabel + 1;
}

int finishDriverCoroutine(TestDriver &driver)
{
	if (driver.completedReturnValues().myExceptionPtr) {
		std::rethrow_exception(
			driver.completedReturnValues().myExceptionPtr);
	}

	return driver.completedReturnValues().myReturnValue;
}

int runDriverCoroutine(TestDriver &driver)
{
	pumpIoContext();
	return finishDriverCoroutine(driver);
}

int runDriverCoroutineWithoutPump(TestDriver &driver)
{
	return finishDriverCoroutine(driver);
}

int readCompletedLabel(TestViralNonPostingInvoker<int> &invoker)
{
	return invoker.completedReturnValues().myReturnValue;
}

TestDriver testImmediateReturnFastPath()
{
	const int value = co_await returnLabelImmediately(42);
	if (value != 42) {
		throw std::runtime_error("immediateReturnFastPath: wrong return value");
	}
	co_return 0;
}

TestDriver testAllCompleteBeforeCoAwait()
{
	TestViralNonPostingInvoker<int> invokerTen = returnLabelImmediately(10);
	TestViralNonPostingInvoker<int> invokerTwenty = returnLabelImmediately(20);
	TestViralNonPostingInvoker<int> invokerThirty = returnLabelImmediately(30);

	const int valueTen = co_await invokerTen;
	const int valueTwenty = co_await invokerTwenty;
	const int valueThirty = co_await invokerThirty;

	if (valueTen != 10 || valueTwenty != 20 || valueThirty != 30) {
		throw std::runtime_error("allCompleteBeforeCoAwait: label mismatch");
	}

	co_return 0;
}

TestDriver testCallerSuspendsThenResumes()
{
	const int value = co_await waitAndReturnLabel(delayShortMs);
	if (value != delayShortMs) {
		throw std::runtime_error("callerSuspendsThenResumes: wrong label");
	}
	co_return 0;
}

TestDriver testMixedImmediateAndDelayedInSequence()
{
	const int immediate = co_await returnLabelImmediately(7);
	const int delayed = co_await waitAndReturnLabel(delayShortMs);

	if (immediate != 7 || delayed != delayShortMs) {
		throw std::runtime_error(
			"mixedImmediateAndDelayedInSequence: label mismatch");
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceFastPath()
{
	std::size_t awaitResumeCallCount = 0;
	TestViralNonPostingInvoker<int> invoker = returnLabelImmediately(42);
	const int value = co_await CountingViralNonPostingAwaiter<int>{
		invoker, awaitResumeCallCount};

	if (value != 42) {
		throw std::runtime_error(
			"awaitResumeCalledOnceFastPath: wrong return value");
	}
	if (awaitResumeCallCount != 1) {
		throw std::runtime_error(
			"awaitResumeCalledOnceFastPath: await_resume count="
			+ std::to_string(awaitResumeCallCount));
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceSlowPath()
{
	std::size_t awaitResumeCallCount = 0;
	TestViralNonPostingInvoker<int> invoker =
		waitAndReturnLabel(delayShortMs);
	const int value = co_await CountingViralNonPostingAwaiter<int>{
		invoker, awaitResumeCallCount};

	if (value != delayShortMs) {
		throw std::runtime_error(
			"awaitResumeCalledOnceSlowPath: wrong return value");
	}
	if (awaitResumeCallCount != 1) {
		throw std::runtime_error(
			"awaitResumeCalledOnceSlowPath: await_resume count="
			+ std::to_string(awaitResumeCallCount));
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceNested()
{
	std::size_t awaitResumeCallCount = 0;
	TestViralNonPostingInvoker<int> inner =
		innerDelayedCoAwait(delayShortMs);
	const int value = co_await CountingViralNonPostingAwaiter<int>{
		inner, awaitResumeCallCount};

	if (value != delayShortMs) {
		throw std::runtime_error(
			"awaitResumeCalledOnceNested: wrong return value");
	}
	if (awaitResumeCallCount != 1) {
		throw std::runtime_error(
			"awaitResumeCalledOnceNested: await_resume count="
			+ std::to_string(awaitResumeCallCount));
	}

	co_return 0;
}

TestDriver testMoveCountedReturnNotDoubleMoved()
{
	auto moveCount = std::make_shared<std::size_t>(0);
	TestViralNonPostingInvoker<MoveCountedInt> invoker =
		returnMoveCountedInt(moveCount, 99);
	MoveCountedInt result = co_await invoker;

	if (result.value != 99) {
		throw std::runtime_error(
			"moveCountedReturnNotDoubleMoved: wrong value");
	}
	if (*moveCount > 2) {
		throw std::runtime_error(
			"moveCountedReturnNotDoubleMoved: excessive move count="
			+ std::to_string(*moveCount)
			+ " (possible double await_resume)");
	}
	if (*moveCount < 1) {
		throw std::runtime_error(
			"moveCountedReturnNotDoubleMoved: no move out of promise");
	}

	co_return 0;
}

TestVoidDriver voidReturnImmediately()
{
	co_return;
}

TestDriver testVoidReturnCompletes()
{
	co_await voidReturnImmediately();
	co_return 0;
}

TestDriver testIntReturnValuePropagates()
{
	const int value = co_await returnLabelImmediately(123);
	if (value != 123) {
		throw std::runtime_error("intReturnValuePropagates: wrong value");
	}
	co_return 0;
}

TestDriver testReturnValuesReadableBeforeDestroy()
{
	TestViralNonPostingInvoker<int> invoker = returnLabelImmediately(55);
	(void)co_await invoker;

	if (readCompletedLabel(invoker) != 55) {
		throw std::runtime_error(
			"returnValuesReadableBeforeDestroy: label not readable");
	}

	co_return 0;
}

TestDriver testExceptionRethrowsOnCoAwait()
{
	try {
		(void)co_await throwRuntimeErrorImmediately();
		throw std::runtime_error(
			"exceptionRethrowsOnCoAwait: expected exception");
	}
	catch (const std::runtime_error &runtimeError) {
		if (std::string(runtimeError.what()) != expectedThrowMessage) {
			throw std::runtime_error(
				"exceptionRethrowsOnCoAwait: unexpected message");
		}
	}

	co_return 0;
}

TestDriver testNonStdExceptionRethrows()
{
	try {
		(void)co_await throwIntImmediately();
		throw std::runtime_error(
			"nonStdExceptionRethrows: expected int exception");
	}
	catch (int caughtValue) {
		if (caughtValue != expectedNonStdThrowValue) {
			throw std::runtime_error(
				"nonStdExceptionRethrows: unexpected int value");
		}
	}

	co_return 0;
}

TestDriver testCalleeRunsOnCallerThread()
{
	const std::thread::id callerThreadId = std::this_thread::get_id();
	const ThreadIdPair pair = co_await recordThreadIdsAtReturn();

	if (pair.calleeId != callerThreadId) {
		throw std::runtime_error("calleeRunsOnCallerThread: thread mismatch");
	}

	co_return 0;
}

TestDriver testDelayedCalleeStillOnCallerThread()
{
	const std::thread::id callerThreadId = std::this_thread::get_id();
	const ThreadIdPair pair =
		co_await recordThreadIdsAfterDelay(delayShortMs);

	if (pair.calleeId != callerThreadId) {
		throw std::runtime_error(
			"delayedCalleeStillOnCallerThread: thread mismatch");
	}

	co_return 0;
}

TestDriver testNestedNonPostingCoAwait()
{
	const int sum = co_await nestedNonPostingSum(10, 32);
	if (sum != 42) {
		throw std::runtime_error("nestedNonPostingCoAwait: wrong sum");
	}
	co_return 0;
}

TestDriver testNestedInnerSuspension()
{
	const int value = co_await outerCoAwaitingDelayedInner(delayShortMs);
	if (value != delayShortMs + 1) {
		throw std::runtime_error("nestedInnerSuspension: wrong value");
	}
	co_return 0;
}

TestDriver testVoidMemberInGroup()
{
	TestVoidGroup group;

	TestVoidDriver voidInvoker = voidMemberAfterDelay(delayShortMs);
	group.add(voidInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("voidMemberInGroup: expected one settlement");
	}

	if (allDescriptors[0].type
		!= TestVoidGroup::SettlementDescriptor::TypeE::COMPLETED) {
		throw std::runtime_error(
			"voidMemberInGroup: expected COMPLETED settlement");
	}

	co_return 0;
}

TestDriver testGroupMixedImmediateAndDelayed()
{
	TestIntGroup group;

	TestViralNonPostingInvoker<int> immediateInvoker =
		returnLabelImmediately(11);
	TestViralNonPostingInvoker<int> delayedInvoker =
		waitAndReturnLabel(delayShortMs);

	group.add(immediateInvoker);
	group.add(delayedInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 2) {
		throw std::runtime_error(
			"groupMixedImmediateAndDelayed: expected two settlements");
	}

	for (auto &descriptor : allDescriptors) {
		const int label = readCompletedLabel(descriptor.invoker.get());
		if (label != 11 && label != delayShortMs) {
			throw std::runtime_error(
				"groupMixedImmediateAndDelayed: unexpected label");
		}
	}

	co_return 0;
}

struct NamedTest
{
	const char *name;
	TestDriver (*body)();
	bool requiresIoPump;
};

void runNamedTestTable(
	const NamedTest *namedTests,
	std::size_t namedTestCount)
{
	for (std::size_t testIndex = 0; testIndex < namedTestCount; ++testIndex)
	{
		const NamedTest &namedTest = namedTests[testIndex];
		std::cout << testLogPrefix << ": RUN " << namedTest.name << "\n";
		TestDriver driver = namedTest.body();
		const int status = namedTest.requiresIoPump
			? runDriverCoroutine(driver)
			: runDriverCoroutineWithoutPump(driver);
		(void)status;
		logPass(namedTest.name);
		resetTestIoService();
	}
}

void runStandaloneTests()
{
	static const NamedTest namedTests[] = {
		{"immediateReturnFastPath", testImmediateReturnFastPath, false},
		{"allCompleteBeforeCoAwait", testAllCompleteBeforeCoAwait, false},
		{"callerSuspendsThenResumes", testCallerSuspendsThenResumes, true},
		{"mixedImmediateAndDelayedInSequence",
			testMixedImmediateAndDelayedInSequence, true},
		{"awaitResumeCalledOnceFastPath",
			testAwaitResumeCalledOnceFastPath, false},
		{"awaitResumeCalledOnceSlowPath",
			testAwaitResumeCalledOnceSlowPath, true},
		{"awaitResumeCalledOnceNested",
			testAwaitResumeCalledOnceNested, true},
		{"moveCountedReturnNotDoubleMoved",
			testMoveCountedReturnNotDoubleMoved, false},
		{"voidReturnCompletes", testVoidReturnCompletes, false},
		{"intReturnValuePropagates", testIntReturnValuePropagates, false},
		{"returnValuesReadableBeforeDestroy",
			testReturnValuesReadableBeforeDestroy, false},
		{"exceptionRethrowsOnCoAwait", testExceptionRethrowsOnCoAwait, false},
		{"nonStdExceptionRethrows", testNonStdExceptionRethrows, false},
		{"calleeRunsOnCallerThread", testCalleeRunsOnCallerThread, false},
		{"delayedCalleeStillOnCallerThread",
			testDelayedCalleeStillOnCallerThread, true},
		{"nestedNonPostingCoAwait", testNestedNonPostingCoAwait, false},
		{"nestedInnerSuspension", testNestedInnerSuspension, true},
	};

	runNamedTestTable(
		namedTests,
		sizeof(namedTests) / sizeof(namedTests[0]));
}

void runGroupIntegrationTests()
{
	static const NamedTest namedTests[] = {
		{"voidMemberInGroup", testVoidMemberInGroup, true},
		{"groupMixedImmediateAndDelayed",
			testGroupMixedImmediateAndDelayed, true},
	};

	runNamedTestTable(
		namedTests,
		sizeof(namedTests) / sizeof(namedTests[0]));
}

} // namespace

} // namespace sscl::co::viral_non_posting_test

int main()
{
	using namespace sscl::co::viral_non_posting_test;

	try {
		runStandaloneTests();

		const int groupTestsExitCode =
			ctest::test::runMarionetteHarnessAndExit(
				[]
				{
					try {
						runGroupIntegrationTests();
						std::cout << testLogPrefix
							<< ": all tests passed\n";
						return EXIT_SUCCESS;
					}
					catch (const std::exception &exception) {
						std::cerr << testLogPrefix << ": FAIL: "
							<< exception.what() << "\n";
						return EXIT_FAILURE;
					}
					catch (...) {
						std::cerr << testLogPrefix
							<< ": FAIL: unknown exception\n";
						return EXIT_FAILURE;
					}
				});

		if (groupTestsExitCode != EXIT_SUCCESS) {
			return groupTestsExitCode;
		}

		std::cout << testLogPrefix << ": all tests passed\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception &exception) {
		std::cerr << testLogPrefix << ": FAIL: " << exception.what() << "\n";
		return EXIT_FAILURE;
	}
	catch (...) {
		std::cerr << testLogPrefix << ": FAIL: unknown exception\n";
		return EXIT_FAILURE;
	}
}
