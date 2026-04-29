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
	: calleePromise(_calleePromise)
	{}

	~PostingInvoker() noexcept = default;

	void setCallerSchedHandle(std::coroutine_handle<void> _callerSchedHandle) noexcept
	{
		calleePromise.callerSchedHandle = _callerSchedHandle;
		calleePromise.setCallerPromiseChainLink(nullptr);
	}

	template <typename CallerPromise>
	void setCallerSchedHandle(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"PostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandle =
			std::coroutine_handle<void>::from_address(callerSchedHandle.address());

		calleePromise.setCallerPromiseChainLink(&callerSchedHandle.promise());
	}

	auto await_resume() const
	{
		ReturnValues<T> &returnValues = calleePromise.returnValues;
		CalleeCoroutineHandleDestroyer completion(calleePromise.selfSchedHandle);
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to check for and rethrow any exception.\n";
		if (returnValues.myExceptionPtr) {
			std::rethrow_exception(returnValues.myExceptionPtr);
		}
		else if constexpr (!std::is_void_v<T>) {
			return std::move(returnValues.myReturnValue);
		}
		else {
			return;
		}
	}

private:
	PromiseType &calleePromise;
};

#endif // POSTING_INVOKER_H
