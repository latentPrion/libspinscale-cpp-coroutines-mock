#ifndef CO_QUTEX_H
#define CO_QUTEX_H

#include <list>
#include <coroutine>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "current_io_context.h"
#include "spinlock.h"

class CoQutex
{
public:
	CoQutex() noexcept;
	CoQutex(const CoQutex&) = delete;
	CoQutex(CoQutex&&) noexcept;
	~CoQutex();

	struct AcquireInvocationAndSuspensionPolicy
	{
		AcquireInvocationAndSuspensionPolicy(CoQutex &_coQutex) noexcept
		: coQutex(_coQutex)
		{}

		~AcquireInvocationAndSuspensionPolicy() noexcept = default;

		struct WaitingCoroutine
		{
			WaitingCoroutine(
				std::coroutine_handle<void> _callerSchedHandle,
				boost::asio::io_context &_callerIoContext) noexcept
			: callerSchedHandle(_callerSchedHandle), callerIoContext(_callerIoContext)
			{}

			std::coroutine_handle<void> callerSchedHandle;
			boost::asio::io_context &callerIoContext;
		};

		bool await_ready() noexcept { return false; }
		bool await_suspend(std::coroutine_handle<void> callerSchedHandle) noexcept
		{
			sscl::SpinLock::Guard guard(coQutex.spinLock);
			if (!coQutex.isOwned)
			{
				coQutex.isOwned = true;
				return false;
			}

			coQutex.waitingCoroutines.emplace_back(callerSchedHandle, current_io_context());
			return true;
		}
		void await_resume() noexcept { /* nothing has to happen here */ }

		CoQutex &coQutex;
	};

	AcquireInvocationAndSuspensionPolicy getAcquireInvocationAndSuspensionPolicy() noexcept
	{
		return AcquireInvocationAndSuspensionPolicy(*this);
	}

	void releaseCInd() noexcept
	{
		sscl::SpinLock::Guard guard(spinLock);

		isOwned = false;
		if (waitingCoroutines.empty())
			{ return; }

		auto &frontWaitingCoroutine = waitingCoroutines.front();
		boost::asio::post(
			frontWaitingCoroutine.callerIoContext,
			frontWaitingCoroutine.callerSchedHandle);
		waitingCoroutines.pop_front();
	}

private:
	friend class AcquireInvocationAndSuspensionPolicy;
	sscl::SpinLock spinLock;
	bool isOwned = false;
	std::list<AcquireInvocationAndSuspensionPolicy::WaitingCoroutine> waitingCoroutines;
};

#endif // CO_QUTEX_H
