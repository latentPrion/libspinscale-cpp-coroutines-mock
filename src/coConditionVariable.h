#ifndef CO_CONDITION_VARIABLE_H
#define CO_CONDITION_VARIABLE_H

#include <coroutine>
#include <deque>
#include <iostream>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "current_io_context.h"
#include "spinlock.h"

/**	Coroutine-friendly handoff: wait until `signal()` before running a completion
 *	step (used from `final_suspend` so `PostingPromise::callerSchedHandle` is not
 *	read until the viral invoker has run `setCallerSchedHandle`).
 *
 *	`clear()` only clears `isSignaled`; it does not wake or drain waiters. If
 *	`clear()` runs while coroutines are still waiting, they stay queued until a
 *	later `signal()` posts them.
 *
 *	High-level completion for posting promises lives in `PostingPromise::FinalSuspendPostingInvoker`,
 *	not in callbacks on this type.
 */
class CoConditionVariable
{
public:
	struct WaitingCoroutine
	{
		std::coroutine_handle<void> callerSchedHandle;
		boost::asio::io_context &callerIoContext;
	};

	struct OperationInvoker
	{
		explicit OperationInvoker(CoConditionVariable &parentCvIn) noexcept
		: parentCv(parentCvIn)
		{}

		CoConditionVariable &parentCv;
	};

	struct WaitForInvoker
	:	public OperationInvoker
	{
		using OperationInvoker::OperationInvoker;

		bool await_ready() const noexcept { return false; }

		template <typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> cvCallerSchedHandle) noexcept
		{
			std::coroutine_handle<void> const voidHandle =
				std::coroutine_handle<void>::from_address(cvCallerSchedHandle.address());
			boost::asio::io_context &waiterIo = current_io_context();

			sscl::SpinLock::Guard guard(parentCv.spinLock);
			if (parentCv.isSignaled) {
				return false;
			}

			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " CV un-signaled: Enqueuing waiter coroutine.\n";
			parentCv.waitingCoroutines.emplace_back(WaitingCoroutine{
				voidHandle, waiterIo});
			return true;
		}

		void await_resume() const noexcept
		{}
	};

	/**	Manual await-style API only (lowerCamelCase); not a coroutine awaiter.
	 *	`FinalSuspendPostingInvoker` calls `awaitSuspend` explicitly.
	 */
	struct DecisionEnablingDerivableWaitForInvoker
	:	public OperationInvoker
	{
		struct DecisionFactors
		{
			sscl::SpinLock &cvInternalSpinLock;
			bool wasAlreadySignaled;
		};

		using OperationInvoker::OperationInvoker;

		void operator co_await() const = delete;

		bool awaitReady() const noexcept
		{
			return false;
		}

		template <typename Promise>
		DecisionFactors awaitSuspend(std::coroutine_handle<Promise> cvCallerSchedHandle) noexcept
		{
			std::coroutine_handle<void> const voidHandle =
				std::coroutine_handle<void>::from_address(cvCallerSchedHandle.address());
			boost::asio::io_context &waiterIo = current_io_context();

			parentCv.spinLock.acquire();
			if (parentCv.isSignaled) {
				std::cout << __func__ << ": " << std::this_thread::get_id()
					<< " CV signaled: wasAlreadySignaled path.\n";
				return DecisionFactors{parentCv.spinLock, true};
			}

			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " CV un-signaled: Enqueuing waiter coroutine.\n";
			parentCv.waitingCoroutines.emplace_back(WaitingCoroutine{
				voidHandle, waiterIo});
			return DecisionFactors{parentCv.spinLock, false};
		}

		void awaitResume() const noexcept
		{}
	};

	CoConditionVariable() noexcept = default;
	CoConditionVariable(const CoConditionVariable &) = delete;
	CoConditionVariable &operator=(const CoConditionVariable &) = delete;
	CoConditionVariable(CoConditionVariable &&) noexcept = delete;
	CoConditionVariable &operator=(CoConditionVariable &&) noexcept = delete;
	~CoConditionVariable() noexcept = default;

	WaitForInvoker getWaitForInvoker() noexcept
		{ return WaitForInvoker(*this); }

	DecisionEnablingDerivableWaitForInvoker
	getDecisionEnablingDerivableWaitForInvoker() noexcept
	{
		return DecisionEnablingDerivableWaitForInvoker(*this);
	}

	/**	Run `fn` under the internal spinlock, then set `isSignaled` and drain waiters.
	 *	Use from `PostingInvoker::setCallerSchedHandle` so `callerSchedHandle` is published
	 *	before any waiter woken by this signal observes `isSignaled`.
	 */
	template <typename Fn>
	void lockedPublishThenSignal(Fn &&fn) noexcept
	{
		std::deque<WaitingCoroutine> drained;

		{
			sscl::SpinLock::Guard guard(spinLock);
			fn();
			isSignaled = true;
			drained.swap(waitingCoroutines);
		}

		for (WaitingCoroutine &entry : drained)
		{
			if (entry.callerSchedHandle)
			{
				boost::asio::post(
					entry.callerIoContext,
					entry.callerSchedHandle);
			}
		}
	}

	void signal() noexcept
	{
		lockedPublishThenSignal([]() noexcept {});
	}

	/**	Only clears the signaled flag; waiters (if any) remain in the deque. */
	void clear() noexcept
	{
		sscl::SpinLock::Guard guard(spinLock);
		isSignaled = false;
	}

private:
	sscl::SpinLock spinLock;
	bool isSignaled = false;
	std::deque<WaitingCoroutine> waitingCoroutines;
};

#endif // CO_CONDITION_VARIABLE_H
