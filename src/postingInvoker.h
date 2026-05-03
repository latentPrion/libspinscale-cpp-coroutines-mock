#ifndef POSTING_INVOKER_H
#define POSTING_INVOKER_H

#include <coroutine>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

#include "promises.h"

template <typename PromiseType, typename T>
class PostingInvoker
{
public:
	explicit PostingInvoker(PromiseType &_calleePromise) noexcept
	: calleePromise(_calleePromise),
	/**	Viral posting: the coroutine runtime destroys the callee frame after
	 *	`FinalSuspendPostingInvoker::await_resume` returns; only non-viral paths
	 *	use `CalleeCoroutineHandleDestroyer` from here.
	 */
	explicitDestroyCalleeFrame(static_cast<bool>(_calleePromise.callerLambda))
	{}

	~PostingInvoker() noexcept = default;

	void setCallerSchedHandle(std::coroutine_handle<void> _callerSchedHandle) noexcept
	{
		calleePromise.callerSchedHandleIsSetCv.lockedPublishThenSignal([&]() noexcept {
			calleePromise.callerSchedHandle = _callerSchedHandle;
			calleePromise.setCallerPromiseChainLink(nullptr);
		});
	}

	template <typename CallerPromise>
	void setCallerSchedHandle(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"PostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandleIsSetCv.lockedPublishThenSignal([&]() noexcept {
			calleePromise.callerSchedHandle =
				std::coroutine_handle<void>::from_address(callerSchedHandle.address());
			calleePromise.setCallerPromiseChainLink(&callerSchedHandle.promise());
		});
	}

	auto await_resume() const
	{
		ReturnValues<T> &returnValues = calleePromise.returnValues;
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to check for and rethrow any exception.\n";
		if (returnValues.myExceptionPtr) {
			std::exception_ptr const captured = returnValues.myExceptionPtr;
			if (explicitDestroyCalleeFrame) {
				CalleeCoroutineHandleDestroyer completion(calleePromise.selfSchedHandle);
				std::rethrow_exception(captured);
			}
			std::rethrow_exception(captured);
		}
		if constexpr (!std::is_void_v<T>) {
			T result = std::move(returnValues.myReturnValue);
			if (explicitDestroyCalleeFrame) {
				CalleeCoroutineHandleDestroyer completion(calleePromise.selfSchedHandle);
			}
			return result;
		}
		if (explicitDestroyCalleeFrame) {
			CalleeCoroutineHandleDestroyer completion(calleePromise.selfSchedHandle);
		}
	}

private:
	PromiseType &calleePromise;
	bool explicitDestroyCalleeFrame;
};

#endif // POSTING_INVOKER_H
