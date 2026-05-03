#ifndef PROMISES_H
#define PROMISES_H

#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "current_io_context.h"
#include "coConditionVariable.h"
#include "coQutex.h"

template <typename PromiseType, typename T>
class PostingInvoker;

struct CalleeCoroutineHandleDestroyer
{
	CalleeCoroutineHandleDestroyer() noexcept = default;

	explicit CalleeCoroutineHandleDestroyer(std::coroutine_handle<> schedHandle) noexcept
	: selfSchedHandle(schedHandle)
	{}

	CalleeCoroutineHandleDestroyer(CalleeCoroutineHandleDestroyer &&other) noexcept
	: selfSchedHandle(std::exchange(other.selfSchedHandle, {}))
	{}

	CalleeCoroutineHandleDestroyer &operator=(CalleeCoroutineHandleDestroyer &&other) noexcept
	{
		if (this != &other) {
			if (selfSchedHandle) {
				selfSchedHandle.destroy();
			}
			selfSchedHandle = std::exchange(other.selfSchedHandle, {});
		}
		return *this;
	}

	~CalleeCoroutineHandleDestroyer() noexcept
	{
		if (selfSchedHandle) {
			selfSchedHandle.destroy();
		}
	}

	CalleeCoroutineHandleDestroyer(const CalleeCoroutineHandleDestroyer &) = delete;
	CalleeCoroutineHandleDestroyer &operator=(const CalleeCoroutineHandleDestroyer &) = delete;

	std::coroutine_handle<> selfSchedHandle;
};

template <typename T, bool IsVoid = std::is_void_v<T>>
struct ReturnValueStorage;

template <typename T>
struct ReturnValueStorage<T, false>
{
	T myReturnValue{};
};

template <typename T>
struct ReturnValueStorage<T, true>
{
};

template <typename T>
struct ReturnValues
: public ReturnValueStorage<T>
{
	ReturnValues() noexcept
	: myExceptionPtr(myMemberExceptionPtr)
	{}

	explicit ReturnValues(std::exception_ptr &callerExceptionPtr) noexcept
	: myExceptionPtr(callerExceptionPtr)
	{}

	~ReturnValues() noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Destructing.\n";
	}

	/**	EXPLANATION:
	 * The exception_ptr ref here can either point to the exception_ptr
	 * a non-viral coroutine supplied to us as its storage space for
	 * where we should store any exception that is thrown;
	 *
	 * Or it could point to the member exception_ptr in this very class,
	 * which is used for viral coroutines that can bubble their exception
	 * up and automatically via the language runtime.
	 */
	std::exception_ptr &myExceptionPtr;
	std::exception_ptr myMemberExceptionPtr = nullptr;
};

/**	`return_value` / `return_void` only. ThreadTag is not a template parameter here:
 *	for tagged promises, PromiseType is `TaggedPostingPromise<T, ThreadTag>`.
 */
template <typename PromiseType, typename T, bool IsVoid = std::is_void_v<T>>
struct PostingPromiseReturnOps;

template <typename PromiseType, typename T>
struct PostingPromiseReturnOps<PromiseType, T, false>
{
	void return_value(T returnValue) noexcept
	{
		static_cast<PromiseType *>(this)->returnValues.myReturnValue = std::move(returnValue);
	}
};

template <typename PromiseType, typename T>
struct PostingPromiseReturnOps<PromiseType, T, true>
{
	void return_void() noexcept
	{
		return;
	}
};

template <typename T>
struct PostingPromise
: public PromiseChainLink
{
	PostingPromise() noexcept
	: returnValues()
	{}

	PostingPromise(
		std::exception_ptr &_callerExceptionPtr,
		std::function<void(CalleeCoroutineHandleDestroyer)> _callerLambda) noexcept
	: returnValues(_callerExceptionPtr),
	callerLambda(std::move(_callerLambda))
	{}

	~PostingPromise() noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Destructing.\n";
	}

	void unhandled_exception() noexcept
	{
		returnValues.myExceptionPtr = std::current_exception();
	}

	void removeAcquiredLock(CoQutex &coQutex) noexcept override
	{
		eraseFirstMatchingAcquiredLock(coQutex);
	}

	const PromiseChainLink *callerPromiseChainLink() const noexcept override
		{ return callerChainLink; }

	PromiseChainLink *callerPromiseChainLink() noexcept override
		{ return callerChainLink; }

	ReturnValues<T> returnValues;
	std::function<void(CalleeCoroutineHandleDestroyer)> callerLambda;
	boost::asio::io_context &callerIoContext = current_io_context();
	std::coroutine_handle<> selfSchedHandle;
	std::coroutine_handle<void> callerSchedHandle;
	PromiseChainLink *callerChainLink = nullptr;
	CoConditionVariable callerSchedHandleIsSetCv;

	/**	Non-viral (`callerLambda`): post completion from `await_suspend` and suspend
	 * like `suspend_always` so the frame survives until the lambda destroys it.
	 *
	 * Viral: suspend on `callerSchedHandleIsSetCv` using `DecisionEnablingDerivableWaitForInvoker`
	 * until `PostingInvoker::setCallerSchedHandle` has stored the handle and called
	 * `callerSchedHandleIsSetCv.signal()`. Do not read `callerSchedHandle` until the
	 * condvar is signaled. Always **`return true`** from `await_suspend` so the callee
	 * stays suspended at `final_suspend` until the original caller runs
	 * `CalleeCoroutineHandleDestroyer`.
	 * If the condvar was already signaled, post the caller resume from
	 * `await_suspend`.
	 * If the callee was enqueued and woken by `signal()`, post from
	 * `await_resume` (see sketch in `docs/prompts/coro-condvars-in-promise.md`).
	 */
	struct FinalSuspendPostingInvoker
	{
		explicit FinalSuspendPostingInvoker(
			boost::asio::io_context &callerIoIn,
			PostingPromise &calleeIn,
			std::function<void(CalleeCoroutineHandleDestroyer)> lambdaIn)
			noexcept
		: callerIoContext(callerIoIn),
		calleePromise(calleeIn),
		callerLambdaStorage(std::move(lambdaIn)),
		decisionEnablingInvoker(std::nullopt)
		{}

		FinalSuspendPostingInvoker(
			boost::asio::io_context &callerIoIn,
			PostingPromise &calleeIn,
			CoConditionVariable::DecisionEnablingDerivableWaitForInvoker
				decisionInvokerIn)
			noexcept
		: callerIoContext(callerIoIn),
		calleePromise(calleeIn),
		callerLambdaStorage(std::nullopt),
		decisionEnablingInvoker(std::in_place, std::move(decisionInvokerIn))
		{}

		bool await_ready() const noexcept
		{
			return false;
		}

		template <typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h) noexcept
		{
			if (callerLambdaStorage)
			{
				std::cout << __func__ << ": " << std::this_thread::get_id()
					<< " Non-viral: posting callerLambda completion.\n";
				boost::asio::post(callerIoContext, [this]() noexcept {
					(*callerLambdaStorage)(
						CalleeCoroutineHandleDestroyer(
							calleePromise.selfSchedHandle));
				});
				return true;
			}

			decisionFactors.emplace((*decisionEnablingInvoker).awaitSuspend(h));
			if (decisionFactors->wasAlreadySignaled)
			{
				decisionFactors->cvInternalSpinLock.release();
				std::cout << __func__ << ": " << std::this_thread::get_id()
					<< " Viral: condvar was already signaled: post-back-ing callerSchedHandle resume to caller io_context.\n";
				boost::asio::post(
					callerIoContext,
					calleePromise.callerSchedHandle);
			}
			else
			{
				// Basically a no-op
				std::cout << __func__ << ": " << std::this_thread::get_id()
					<< " Viral: condvar was not signaled: Deferring post-back until condvar is signaled.\n";
				decisionFactors->cvInternalSpinLock.release();
			}

			return true;
		}

		void await_resume() noexcept
		{
			if (callerLambdaStorage
				|| (decisionFactors
				&& decisionFactors->wasAlreadySignaled))
			{
				return;
			}

			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " Viral: condvar was not signaled: posting callerSchedHandle resume to caller io_context.\n";
			/**	`post` (not `dispatch`): `await_resume` runs on the callee executor after
			 *	`signal()` posted this coroutine there, so we always queue the caller
			 *	resume onto `callerIoContext` instead of risking inline execution when the
			 *	two contexts coincide.
			 */
			boost::asio::post(
				callerIoContext,
				calleePromise.callerSchedHandle);
		}

	private:
		boost::asio::io_context &callerIoContext;
		PostingPromise &calleePromise;
		std::optional<std::function<void(CalleeCoroutineHandleDestroyer)>>
			callerLambdaStorage;
		std::optional<CoConditionVariable::DecisionEnablingDerivableWaitForInvoker>
			decisionEnablingInvoker;
		/**	Viral path only: `emplace`d in `await_suspend` after the `callerLambdaStorage`
		 *	branch. `await_resume` runs that branch only when `callerLambdaStorage` is
		 *	empty, so `decisionFactors` is always set there; it reads `wasAlreadySignaled`
		 *	like the sketch.
		 */
		std::optional<
			CoConditionVariable::DecisionEnablingDerivableWaitForInvoker::DecisionFactors>
			decisionFactors;
	};

	FinalSuspendPostingInvoker final_suspend() noexcept
	{
		if (callerLambda)
		{
			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " Non-viral: returning lambda-invoking FinalSuspendPostingInvoker.\n";
			return FinalSuspendPostingInvoker(
				callerIoContext, *this, std::move(callerLambda));
		}

		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Viral: returning condvar-checking FinalSuspendPostingInvoker.\n";
		return FinalSuspendPostingInvoker(
			callerIoContext, *this,
			callerSchedHandleIsSetCv
				.getDecisionEnablingDerivableWaitForInvoker());
	}

protected:
	void setSelfSchedHandle(std::coroutine_handle<> schedHandle) noexcept
	{
		selfSchedHandle = schedHandle;
	}

	void setCallerPromiseChainLink(PromiseChainLink *chainLink) noexcept
	{
		callerChainLink = chainLink;
	}

	template <typename, typename>
	friend class PostingInvoker;
};

template <typename T, typename ThreadTag>
struct TaggedPostingPromise
:	public PostingPromise<T>,
	public PostingPromiseReturnOps<TaggedPostingPromise<T, ThreadTag>, T>
{
	TaggedPostingPromise() noexcept
	:	PostingPromise<T>()
	{}

	TaggedPostingPromise(
		std::exception_ptr &_exceptionPtr,
		std::function<void(CalleeCoroutineHandleDestroyer)> _callerLambda) noexcept
	:	PostingPromise<T>(_exceptionPtr, std::move(_callerLambda))
	{}

	std::suspend_always initial_suspend() noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to post selfSchedHandle to " << typeid(ThreadTag).name() << ".\n";
		boost::asio::post(
			ThreadTag::io_context(),
			this->selfSchedHandle);
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning suspend_always.\n";
		return {};
	}
};

#endif // PROMISES_H
