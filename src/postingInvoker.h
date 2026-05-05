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

	template <typename CallerPromise>
	bool setCallerSchedHandle(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"PostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandle = callerSchedHandle;
		calleePromise.setCallerPromiseChainLink(&callerSchedHandle.promise());
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Done setting callerSchedHandle; running CallerFlowExecutor.\n";
		return calleePromise.postBackStatus.getCallerFlowExecutor()();
	}

	auto await_resume()
	{
		calleePromise.postBackStatus.reset();

		ReturnValues<T> &returnValues = calleePromise.returnValues;
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " About to check for and rethrow any exception.\n";

		CalleeCoroutineHandleDestroyer completion(
			calleePromise.selfSchedHandle);

		if (returnValues.myExceptionPtr)
		{
			std::exception_ptr const captured = returnValues.myExceptionPtr;
			std::rethrow_exception(captured);
		}
		if constexpr (!std::is_void_v<T>)
		{
			T result = std::move(returnValues.myReturnValue);
			return result;
		}
	}

private:
	PromiseType &calleePromise;
};

#endif // POSTING_INVOKER_H
