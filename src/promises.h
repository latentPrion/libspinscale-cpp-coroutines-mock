#ifndef PROMISES_H
#define PROMISES_H

#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "current_io_context.h"
#include "coQutex.h"

extern boost::asio::io_context bodyIoContext;

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

template <typename T>
class BodyPostingPromise;

template <typename PromiseType, typename T, bool IsVoid = std::is_void_v<T>>
struct BodyPostingPromiseReturnOps;

template <typename PromiseType, typename T>
struct BodyPostingPromiseReturnOps<PromiseType, T, false>
{
	void return_value(T returnValue) noexcept
	{
		static_cast<PromiseType *>(this)->returnValues.myReturnValue = std::move(returnValue);
	}
};

template <typename PromiseType, typename T>
struct BodyPostingPromiseReturnOps<PromiseType, T, true>
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

	std::suspend_always final_suspend() noexcept
	{
		if (callerSchedHandle)
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " About to post-back callerSchedHandle to mainIoContext.\n";
			boost::asio::post(callerIoContext, callerSchedHandle);
		}
		else if (callerLambda)
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " About to post-back lambda to mainIoContext.\n";
			boost::asio::post(callerIoContext, [this] {
				callerLambda(CalleeCoroutineHandleDestroyer(selfSchedHandle));
			});
		}
		else {
			std::cout << __func__ << ": " << std::this_thread::get_id() << " No mechanism provided to post-back to caller. Coroutine frame will leak.\n";
		}
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning suspend_always.\n";
		return {};
	}

	/**	EXPLANATION:
	 * This is used to temporarily store the return values from the callee,
	 * in the case of a non-viral invoker which gives us a lambda and
	 * an exception pointer. The lambda+excPtr are given to us in the
	 * promise_type ctor. But this ctor runs prior to get_return_object(),
	 * so we need to store the values here temporarily.
	 *
	 * Inside of get_return_object(), we assign the values to the callerInvoker,
	 * and from then on, the callerInvoker is the authoritative single storage
	 * location for return values to the caller.
	 *
	 * Doing it this way enables us to use suspend_never in final_suspend().
	 */
	ReturnValues<T> returnValues;
	std::function<void(CalleeCoroutineHandleDestroyer)> callerLambda;
	boost::asio::io_context &callerIoContext = current_io_context();
	std::coroutine_handle<> selfSchedHandle;
	std::coroutine_handle<void> callerSchedHandle;
	PromiseChainLink *callerChainLink = nullptr;

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

template <typename T>
struct BodyPostingPromise
:	public PostingPromise<T>,
	public BodyPostingPromiseReturnOps<BodyPostingPromise<T>, T>
{
	BodyPostingPromise() noexcept
	:	PostingPromise<T>()
	{
		initializeSelfSchedHandle();
	}

	BodyPostingPromise(
		std::exception_ptr &_exceptionPtr,
		std::function<void(CalleeCoroutineHandleDestroyer)> _callerLambda) noexcept
	:	PostingPromise<T>(_exceptionPtr, std::move(_callerLambda))
	{
		initializeSelfSchedHandle();
	}

	std::suspend_always initial_suspend() noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to post to bodyIoContext.\n";
		boost::asio::post(bodyIoContext, this->selfSchedHandle);
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning suspend_always.\n";
		return {};
	}

private:
	void initializeSelfSchedHandle() noexcept
	{
		this->setSelfSchedHandle(
			std::coroutine_handle<BodyPostingPromise<T>>::from_promise(*this));
	}
};

#endif // PROMISES_H
