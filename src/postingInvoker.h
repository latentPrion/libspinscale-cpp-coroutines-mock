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
		{ calleePromise.callerSchedHandle = _callerSchedHandle; }

	/**	EXPLANATION:
	 * Used in the exceptional edge case where the caller obtains the
	 * invoker and then doesn't co_await it; but instead passes it
	 * to some other thread to be invoked. In this case, the caller
	 * will have to set its own thread's io_context here, so that the
	 * final_suspend call will properly post back it, instead of posting
	 * back to the original invoker-obtainer's io_context.
	 */
	void setCallerIoContext(boost::asio::io_context &_callerIoContext) noexcept
		{ calleePromise.callerIoContext = _callerIoContext; }

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
