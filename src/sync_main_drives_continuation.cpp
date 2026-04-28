#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <functional>
#include <type_traits>
#include <utility>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "current_io_context.h"
#include "coQutex.h"

thread_local boost::asio::io_context *tls_current_io_context = nullptr;

boost::asio::io_context &current_io_context() {
	if (!tls_current_io_context) {
		throw std::runtime_error("TLS io_context is not set");
	}
	return *tls_current_io_context;
}

boost::asio::io_context mainIoContext, bodyIoContext;
boost::asio::executor_work_guard<
	boost::asio::io_context::executor_type> bodyIoContextIdleDoesNotEndRun(
		bodyIoContext.get_executor());	

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

protected:
	void setSelfSchedHandle(std::coroutine_handle<> schedHandle) noexcept
	{
		selfSchedHandle = schedHandle;
	}
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

struct NonViralNonSuspendingInvoker
:	public PostingInvoker<BodyPostingPromise<void>, void>
{
	struct promise_type
	:	public BodyPostingPromise<void>
	{
		using BodyPostingPromise<void>::BodyPostingPromise;
		NonViralNonSuspendingInvoker get_return_object()
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning NonViralNonSuspendingInvoker.\n";
			if (!this->callerLambda) {
				throw std::runtime_error("Missing completion lambda: non-viral coroutine would leak frame because no destroy() owner was provided.");
			}
			return NonViralNonSuspendingInvoker(*this);
		}
	};

	using PostingInvoker<BodyPostingPromise<void>, void>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
		return true;
	}
	void await_suspend(std::coroutine_handle<NonViralNonSuspendingInvoker>) noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}
	void await_resume() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
	}
};

template <typename T>
struct ViralSuspendingInvoker
:	public PostingInvoker<BodyPostingPromise<T>, T>
{
	struct promise_type
	:	public BodyPostingPromise<T>
	{
		using BodyPostingPromise<T>::BodyPostingPromise;
		ViralSuspendingInvoker<T> get_return_object() noexcept
		{
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning ViralSuspendingInvoker.\n";
			return ViralSuspendingInvoker<T>(*this);
		}
	};

	using PostingInvoker<BodyPostingPromise<T>, T>::PostingInvoker;

	bool await_ready() const noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning false.\n";
		return false;
	}
	void await_suspend(std::coroutine_handle<> callerSchedHandle) noexcept
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Setting callerSchedHandle and 'suspending'.\n";
		this->setCallerSchedHandle(callerSchedHandle);
	}
	T await_resume() const
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Resumed on caller thread, hopefully.\n";
		return PostingInvoker<BodyPostingPromise<T>, T>::await_resume();
	}
};

ViralSuspendingInvoker<std::string> print2Strings(std::string arg1, std::string arg2)
{
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg1: " << arg1 << "\n";
//	throw "KEK!";
	std::cout << __func__ << ": " << std::this_thread::get_id() << " arg2: " << arg2 << "\n";
	co_return arg1 + " " + arg2;
}

NonViralNonSuspendingInvoker initializeCReq(
	std::exception_ptr &, std::function<void(CalleeCoroutineHandleDestroyer)>)
{
	std::cout << __func__ << ": " << std::this_thread::get_id() << " Executing.\n";
	// throw std::runtime_error("initializeCReq exception");
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to co_await print2Strings.\n";
	std::string returnedString = co_await print2Strings("Hello", "World");
	std::cout << __func__ << ": " << std::this_thread::get_id() << " print2Strings returned: " << returnedString << "\n";
	co_return;
}

void bodyThreadEntry(bool &body_keep_looping)
{
	tls_current_io_context = &bodyIoContext;

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";

	for (body_keep_looping = true; body_keep_looping;) {
		bool send_exception_ind = false;

		try {
			bodyIoContext.restart();
			bodyIoContext.run();
		} catch (const std::exception &exception) {
			send_exception_ind = true;
			std::cerr << "body: Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			send_exception_ind = true;
			std::cerr << "body: Unknown exception occurred\n";
		}

		if (send_exception_ind) {
			std::cerr << "body: loop exception hook stand-in\n";
		}
	}
	tls_current_io_context = nullptr;
}

int main() {
	tls_current_io_context = &mainIoContext;
	bool keep_looping = true;
	bool body_keep_looping = true;

	boost::asio::signal_set shutdownSignals(mainIoContext, SIGINT, SIGTERM);
	shutdownSignals.async_wait([&keep_looping, &body_keep_looping](
		const boost::system::error_code& errorCode, int)
	{
		if (errorCode) {
			return;
		}
		boost::asio::post(bodyIoContext, [&body_keep_looping] {
			bodyIoContext.stop();
			body_keep_looping = false;
		});
		boost::asio::post(mainIoContext, [&keep_looping] {
			mainIoContext.stop();
			keep_looping = false;
		});
	});

	std::thread body_thread(bodyThreadEntry, std::ref(body_keep_looping));

	std::cout << __func__ << ": My TID is: " << std::this_thread::get_id() << ".\n";
	/* Give the body thread time to start up and wait on its io context.
	 */
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to call initializeCReq.\n";
	/*NonViralNonSuspendingInvoker invoker =*/
	std::exception_ptr initializeCReqExceptionPtr = nullptr;
	initializeCReq(
		initializeCReqExceptionPtr,
		[&initializeCReqExceptionPtr, &keep_looping] (CalleeCoroutineHandleDestroyer)
	{
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to check if initializeCReq threw an exception.\n";
		if (initializeCReqExceptionPtr)
		{
			keep_looping = false;
			mainIoContext.stop();
			std::rethrow_exception(initializeCReqExceptionPtr);
		}
	});
	std::cout << __func__ << ": " << std::this_thread::get_id() << " initializeCReq returned.\n";

	for (keep_looping = true; keep_looping;) {
		bool send_exception_ind = false;

		try {
			mainIoContext.restart();
			mainIoContext.run();
		} catch (const std::exception &exception) {
			send_exception_ind = true;
			std::cerr << "main: Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			send_exception_ind = true;
			std::cerr << "main: Unknown exception occurred\n";
		}

		if (send_exception_ind) {
			std::cerr << "main: loop exception hook stand-in\n";
		}
	}

	std::cout << __func__ << ": " << std::this_thread::get_id() << " About to join body thread.\n";
	body_thread.join();
	tls_current_io_context = nullptr;
	return EXIT_SUCCESS;
}
