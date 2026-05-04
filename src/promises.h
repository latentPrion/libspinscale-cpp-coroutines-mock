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
#include "spinlock.h"

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
	struct PostBackStatus
	{
		struct CalleeFlowExecutor;
		struct CallerFlowExecutor;
		friend struct CalleeFlowExecutor;
		friend struct CallerFlowExecutor;

		explicit PostBackStatus(PostingPromise &calleePromiseIn) noexcept
		: calleePromise(calleePromiseIn)
		{}

		void reset() noexcept
		{
			sscl::SpinLock::Guard guard(lock);
			callerHasSetCallerSchedHandle = false;
			calleeIsReadyToPostBack = false;
		}

		struct FlowExecutor
		{
			explicit FlowExecutor(PostBackStatus &parentIn) noexcept
			: parent(parentIn)
			{}

			PostBackStatus &parent;
		};

		struct CalleeFlowExecutor
		:	public FlowExecutor
		{
			explicit CalleeFlowExecutor(PostBackStatus &parentIn) noexcept
			: FlowExecutor(parentIn)
			{}

			void operator()() noexcept
			{
				sscl::SpinLock::Guard guard(this->parent.lock);
				this->parent.calleeIsReadyToPostBack = true;
				if (this->parent.callerHasSetCallerSchedHandle)
				{
					boost::asio::post(
						this->parent.calleePromise.callerIoContext,
						this->parent.calleePromise.callerSchedHandle);
				}
			}
		};

		struct CallerFlowExecutor
		:	public FlowExecutor
		{
			explicit CallerFlowExecutor(PostBackStatus &parentIn) noexcept
			: FlowExecutor(parentIn)
			{}

			bool operator()() noexcept
			{
				sscl::SpinLock::Guard guard(this->parent.lock);
				this->parent.callerHasSetCallerSchedHandle = true;
				if (this->parent.calleeIsReadyToPostBack) {
					return false;
				}
				return true;
			}
		};

		CalleeFlowExecutor getCalleeFlowExecutor() noexcept
		{
			return CalleeFlowExecutor(*this);
		}

		CallerFlowExecutor getCallerFlowExecutor() noexcept
		{
			return CallerFlowExecutor(*this);
		}

		PostingPromise &calleePromise;

	private:
		sscl::SpinLock lock;
		bool callerHasSetCallerSchedHandle = false;
		bool calleeIsReadyToPostBack = false;
	};

	PostingPromise() noexcept
	: returnValues(), postBackStatus(*this)
	{}

	PostingPromise(
		std::exception_ptr &_callerExceptionPtr,
		std::function<void(CalleeCoroutineHandleDestroyer)> _callerLambda) noexcept
	: returnValues(_callerExceptionPtr),
	callerLambda(std::move(_callerLambda)),
	postBackStatus(*this)
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

	/**	Non-viral: post completion lambda to callerIoContext from this thread.
	 *	Viral: run CalleeFlowExecutor (handshake flags); caller may post caller resume
	 *	later via CallerFlowExecutor. See docs/caller-posts-to-own-io-context.md.
	 */
	std::suspend_always final_suspend() noexcept
	{
		if (callerLambda) {
			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " Non-viral: posting callerLambda completion to callerIoContext.\n";
			boost::asio::post(callerIoContext, [this]() noexcept {
				callerLambda(CalleeCoroutineHandleDestroyer(selfSchedHandle));
			});
		}
		else {
			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " Viral: running CalleeFlowExecutor.\n";
			postBackStatus.getCalleeFlowExecutor()();
		}
		return {};
	}

	ReturnValues<T> returnValues;
	std::function<void(CalleeCoroutineHandleDestroyer)> callerLambda;
	boost::asio::io_context &callerIoContext = current_io_context();
	std::coroutine_handle<> selfSchedHandle;
	std::coroutine_handle<void> callerSchedHandle;
	PromiseChainLink *callerChainLink = nullptr;
	PostBackStatus postBackStatus;

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
