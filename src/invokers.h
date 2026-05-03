#ifndef INVOKERS_H
#define INVOKERS_H

#include <coroutine>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "postingInvoker.h"

/**	Non-viral coroutine entry that must not be co_awaited: promise is always
 *	PostingPromiseTemplate<void> (no return-value path to a caller).
 */
template <template <typename> class PostingPromiseTemplate>
struct NonViralNonSuspendingInvoker
:	public PostingInvoker<PostingPromiseTemplate<void>, void>
{
	struct promise_type
	:	public PostingPromiseTemplate<void>
	{
		using PostingPromiseTemplate<void>::PostingPromiseTemplate;

		NonViralNonSuspendingInvoker<PostingPromiseTemplate> get_return_object()
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning NonViralNonSuspendingInvoker.\n";
			if (!this->callerLambda) {
				throw std::runtime_error("Missing completion lambda: non-viral coroutine would leak frame because no destroy() owner was provided.");
			}

			this->setSelfSchedHandle(
				std::coroutine_handle<promise_type>::from_promise(*this));

			return NonViralNonSuspendingInvoker<PostingPromiseTemplate>(*this);
		}
	};

	using PostingInvoker<PostingPromiseTemplate<void>, void>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
		return true;
	}

	void await_suspend(std::coroutine_handle<NonViralNonSuspendingInvoker<PostingPromiseTemplate>>) noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}

	void await_resume() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}
};

/**	Viral awaitable: promise_type inherits PostingPromiseTemplate<T> (posting
 *	target chosen by the posting-promise alias, e.g. BodyPostingPromise<int>).
 */
template <template <typename> class PostingPromiseTemplate, typename T>
struct ViralSuspendingInvoker
:	public PostingInvoker<PostingPromiseTemplate<T>, T>
{
	struct promise_type
	:	public PostingPromiseTemplate<T>
	{
		using PostingPromiseTemplate<T>::PostingPromiseTemplate;

		ViralSuspendingInvoker<PostingPromiseTemplate, T> get_return_object() noexcept
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning ViralSuspendingInvoker.\n";
			this->setSelfSchedHandle(
				std::coroutine_handle<promise_type>::from_promise(*this));

			return ViralSuspendingInvoker<PostingPromiseTemplate, T>(*this);
		}
	};

	using PostingInvoker<PostingPromiseTemplate<T>, T>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning false.\n";
		return false;
	}

	template <typename CallerPromise>
	void await_suspend(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"ViralSuspendingInvoker caller promise must derive from PromiseChainLink");
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Setting callerSchedHandle and 'suspending'.\n";
		this->setCallerSchedHandle(callerSchedHandle);
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Done setting callerSchedHandle. 'Suspending' now.\n";
	}

	T await_resume() const
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Resumed on caller thread, hopefully.\n";
		return PostingInvoker<PostingPromiseTemplate<T>, T>::await_resume();
	}
};

#endif // INVOKERS_H
