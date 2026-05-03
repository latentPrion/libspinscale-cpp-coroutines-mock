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
		assert(false && "setCallerSchedHandle(std::coroutine_handle<void>) should never be called (I think).");
		calleePromise.callerSchedHandle = std::noop_coroutine;
		calleePromise.setCallerPromiseChainLink(nullptr);
		calleePromise.callerSchedHandleIsSetCv.signal();
	}

	template <typename CallerPromise>
	void setCallerSchedHandle(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"PostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandle = callerSchedHandle;
		calleePromise.setCallerPromiseChainLink(&callerSchedHandle.promise());
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Done setting callerSchedHandle. Signaling condvar.\n";
		calleePromise.callerSchedHandleIsSetCv.signal();
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Done signaling condvar.\n";
	}

	auto await_resume() const
	{
		ReturnValues<T> &returnValues = calleePromise.returnValues;
		CalleeCoroutineHandleDestroyer completion(
			calleePromise.selfSchedHandle);
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " About to check for and rethrow any exception.\n";

		if (returnValues.myExceptionPtr)
		{
			std::exception_ptr const captured = returnValues.myExceptionPtr;
			std::rethrow_exception(captured);
		}
		if constexpr (!std::is_void_v<T>) {
			T result = std::move(returnValues.myReturnValue);
			return result;
		}
	}

private:
	PromiseType &calleePromise;
};

#endif // POSTING_INVOKER_H
