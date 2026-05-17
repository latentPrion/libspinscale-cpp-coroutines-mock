#ifndef GROUP_H
#define GROUP_H

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "current_io_context.h"
#include "sharedResourceGroup.h"
#include "spinlock.h"

namespace sscl::co {

namespace detail {

template <typename T, typename H>
concept await_suspend_returns_void = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::same_as<void>;
};

template <typename T, typename H>
concept await_suspend_returns_bool = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::convertible_to<bool>;
};

template <typename T, typename H>
concept await_suspend_returns_handle = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::convertible_to<std::coroutine_handle<>>;
};

template <typename T, typename H>
concept await_suspend_ok = await_suspend_returns_void<T, H>
	|| await_suspend_returns_bool<T, H>
	|| await_suspend_returns_handle<T, H>;

template <typename T, typename H = std::coroutine_handle<>>
concept AwaiterIface = requires(T &t, H h) {
	{ t.await_ready() } -> std::convertible_to<bool>;
	{ t.await_resume() };
} && await_suspend_ok<T, H>;

template <typename T>
auto get_operator_co_await(T &t) -> decltype(operator co_await(t));

template <typename T>
concept AwaitableIface = requires(T &t) {
	{ get_operator_co_await(t) };
} && AwaiterIface<decltype(get_operator_co_await(std::declval<T &>()))>;

} // namespace detail

template <typename T>
concept AwaitableIface = detail::AwaitableIface<T>;

template <typename T>
concept AwaiterIface = detail::AwaiterIface<T>;

template <typename T>
concept AwaitableOrAwaiterIface = AwaiterIface<T> || AwaitableIface<T>;

template <typename Invoker, typename SubjectInvokerReturnType>
requires AwaitableOrAwaiterIface<Invoker>
struct Group
{
	enum class AwaitingCondition {
		NONE, FIRST_SETTLED, ALL_SETTLED
	};

	class SettlementDescriptor
	{
	public:
		enum class TypeE {
			/* We track EXCEPTIION_THROWN but we don't provide an
			 * awaitInvoker for exception events. The caller can
			 * wait for settlements and then scan the result set
			 * to manually deal with exceptions.
			 */
			UNSETTLED, COMPLETED, EXCEPTION_THROWN
		};

		SettlementDescriptor(Invoker &_invoker)
		: invoker(std::ref(_invoker))
		{}

		void setSettlementStatus() noexcept
		{
			assert(type == TypeE::UNSETTLED);

			if (calleeException)
			{
				type = TypeE::EXCEPTION_THROWN;
			}
			else
			{
				type = TypeE::COMPLETED;
			}
		}

		TypeE type = TypeE::UNSETTLED;
		SubjectInvokerReturnType returnValue{};
		std::exception_ptr calleeException = nullptr;
		std::exception_ptr adapterException = nullptr;
		std::reference_wrapper<Invoker> invoker;
	};

	struct SettlementAwaitingInvoker;
	struct AwaitFirstSettlementInvoker;
	struct AwaitAllSettlementsInvoker;

	// getAwaitNextSettlementInvoker();
	AwaitFirstSettlementInvoker getAwaitFirstSettlementInvoker()
		{ return AwaitFirstSettlementInvoker(*this); }

	AwaitAllSettlementsInvoker getAwaitAllSettlementsInvoker()
		{ return AwaitAllSettlementsInvoker(*this); }

	bool verifyAllInvokersSettled() const
	{
		for (auto &desc : s.rsrc.settlements) {
			if (desc.type == SettlementDescriptor::TypeE::UNSETTLED) {
				return false;
			}
		}

		return true;
	}

	bool firstInvokerSettled() const
		{ return s.rsrc.firstSettledInvokerIdx >= 0; }

	bool allInvokersSettled() const
	{
		auto nInvokersAdded = s.rsrc.settlements.size();
		assert(s.rsrc.nInvokersSettled <= nInvokersAdded);
		return s.rsrc.nInvokersSettled == nInvokersAdded;
	}

	struct SettlementAwaitingInvoker
	{
		explicit SettlementAwaitingInvoker(Group &_group)
		: parentGroup(_group)
		{}

		bool await_ready() const { return false; }

		/**	EXPLANATION:
		 * This exists for if we ever need to re-make the adapter coro
		 * throw exceptions. But we decided to make it noexcept in order
		 * to avoid this complication.
		 */
		void checkForAndReThrowAdapterExceptions() const
		{
			std::ostringstream ostream;
			bool doThrow = false;

			for (auto &item : parentGroup.s.rsrc.settlements)
			{
				if (!item.adapterException) {
					continue;
				}

				doThrow = true;
				ostream << "Exc thrown in Group Adapter: ";
				try {
					std::rethrow_exception(item.adapterException);
				} catch (const std::exception &e) {
					ostream << e.what();
				} catch (...) {
					ostream << "<unknown exception type>";
				}
				ostream << "\n";
			}

			if (doThrow) {
				throw std::runtime_error(ostream.str());
			}
		}

		Group &parentGroup;
	};

	/**	CAVEAT:
	 * If you create an instance of AwaitFirstSettlementInvoker and then
	 * immediately create an instance of AwaitAllSettlementsInvoker without
	 * first co_awaiting the AwaitFirstSettlementInvoker, then it is
	 * illegal to call co_await on the AwaitFirst invoker after the
	 * AwaitAll invoker has been co_awaited.
	 *
	 * Always consume (i.e: co_await) the current instantiated invoker
	 * before instantiating another one.
	 */
	struct AwaitFirstSettlementInvoker
	:	public SettlementAwaitingInvoker
	{
		using SettlementAwaitingInvoker::SettlementAwaitingInvoker;

		bool await_suspend(std::coroutine_handle<> groupAwaiterSchedHandle)
		{
			/* Each awaiter obj should only be co_awaited once.
			 * During its await_suspend therefore, it should always
			 * find that the callerSchedHandle hasn't been set, since
			 * it is await_suspend that sets that handle.
			 *
			 *	TODO: We may be able to allow a single AwaitFirst*Invoker
			 * to be co_awaited multiple times. If we do that, then this
			 * assert must be removed.
			 */
			assert(!this->parentGroup.s.rsrc.callerHasSetSchedHandle);

			sscl::SpinLock::Guard guard(this->parentGroup.s.lock);

			if (this->parentGroup.s.rsrc.calleeWasReadyToNotifyOfFirstSettlement) {
				return false;
			}

			/* We store away the coro_handle of the
			 * group awaiter, and suspend that group awaiter.
			 */
			this->parentGroup.s.rsrc.setCallerSchedHandleAndCondition(
				groupAwaiterSchedHandle, AwaitingCondition::FIRST_SETTLED);

			return true;
		}

		std::pair<SettlementDescriptor &, std::vector<SettlementDescriptor> &>
		await_resume()
		{
			assert(this->parentGroup.firstInvokerSettled());
			return {
				this->parentGroup.s.rsrc.settlements[
					this->parentGroup.s.rsrc.firstSettledInvokerIdx],
				this->parentGroup.s.rsrc.settlements
			};
		}
	};

	/**	CAVEAT:
	 * If you create an instance of AwaitAllSettlementsInvoker and then
	 * immediately create an instance of AwaitFirstSettlementInvoker without
	 * first co_awaiting the AwaitAllSettlementsInvoker, then it is
	 * illegal to call co_await on the AwaitAll invoker after the
	 * AwaitFirst invoker has been co_awaited.
	 *
	 * Always consume (i.e: co_await) the current instantiated invoker
	 * before instantiating another one.
	 *
	 * It's illegal to add() new invokers to the Group while co_awaiting
	 * an AwaitAllSettlementsInvoker.
	 * You may add new invokers after co_await returns, though.
	 *
	 *
	 */
	struct AwaitAllSettlementsInvoker
	:	public SettlementAwaitingInvoker
	{
		using SettlementAwaitingInvoker::SettlementAwaitingInvoker;

		bool await_suspend(std::coroutine_handle<> groupAwaiterSchedHandle)
		{
			/* Because we clear the groupAwaiterSchedHandle in each
			 * await_resume(), we should find that groupAwaiterSchedHandle
			 * is unset everytime we call co_await.
			 */
			assert(!this->parentGroup.s.rsrc.callerHasSetSchedHandle);

			sscl::SpinLock::Guard guard(this->parentGroup.s.lock);

			if (this->parentGroup.allInvokersSettled()) {
				return false;
			}

			this->parentGroup.s.rsrc.setCallerSchedHandleAndCondition(
				groupAwaiterSchedHandle, AwaitingCondition::ALL_SETTLED);

			return true;
		}

		std::vector<SettlementDescriptor> &await_resume()
		{
			assert(this->parentGroup.allInvokersSettled());
			return this->parentGroup.s.rsrc.settlements;
		}
	};

	struct NonAwaitableNonPostingAdapterCoro
	{
		struct promise_type
		{
			NonAwaitableNonPostingAdapterCoro get_return_object()
				{ return {}; }
			std::suspend_never initial_suspend() noexcept { return {}; }
			/**	EXPLANATION:
			 * final_suspend must return suspend_never here so that
			 * this fire-and-forget adapter coro will be self-destroying.
			 */
			std::suspend_never final_suspend() noexcept { return {}; }
			void return_void() noexcept { return; }
			void unhandled_exception() noexcept
			{
				try {
					auto eptr = std::current_exception();
					if (eptr) {
						std::rethrow_exception(eptr);
					}
				} catch (const std::exception &e) {
					std::cerr << "Unhandled exception in Group adapter coroutine:\n"
						<< e.what() << "\n";
				} catch (...) {
					std::cerr << "Unhandled non-std exception in Group adapter coroutine\n";
				}

				std::terminate();
			}
		};

		NonAwaitableNonPostingAdapterCoro() = delete;
		NonAwaitableNonPostingAdapterCoro operator co_await() const = delete;
		bool await_ready() const { std::terminate(); return false; }
		void await_suspend() const { std::terminate(); }
		void await_resume() const { std::terminate(); }
	};

	std::pair<bool, bool>
	updateSettlementsStateAndAwakenCallerIfConditionMet(
		typename std::vector<SettlementDescriptor>::iterator settlementIt) noexcept
	{
		bool isFirstSettlement = false;
		bool isLastSettlement = false;
		std::coroutine_handle<> groupAwaiterSchedHandleToWake = nullptr;

		{
			sscl::SpinLock::Guard guard(s.lock);

			/* If we can be certain that the AllSettled condition won't
			 * be triggered repeatedly, then we can get rid of
			 * calleeWasReadyToNotifyOfLastSettlementForCurrentSet.
			 */
			assert(s.rsrc.nInvokersSettled < s.rsrc.settlements.size());
			s.rsrc.nInvokersSettled++;

			if (!firstInvokerSettled())
			{
				isFirstSettlement = true;
				s.rsrc.firstSettledInvokerIdx = static_cast<int>(std::distance(
					s.rsrc.settlements.begin(), settlementIt));

				/* This should be set-once & sticky throughout the lifetime
				 * of the Group object. The first invoker only gets
				 * settled once, irrespective of how many
				 * AwaitFirstSettlementInvoker instances we create.
				 */
				s.rsrc.calleeWasReadyToNotifyOfFirstSettlement = true;
			}

			if (allInvokersSettled())
			{
				assert(s.rsrc.nInvokersSettled == s.rsrc.settlements.size());
				assert(verifyAllInvokersSettled());
				isLastSettlement = true;
			}

			/* Each new settlement condition awaiter must reset
			 * callerHasSetSchedHandle in order to enable that awaiter
			 * to know whether it was possible for the adapter to awaken
			 * the group awaiter.
			 * We currently do this resetting in SettlementAwaitingInvoker's
			 * ctor.
			 *
			 * (Because the adapter can only awaken the group awaiter if
			 * the group awaiter's coro_handle has been set.)
			 */
			if (!s.rsrc.callerHasSetSchedHandle) {
				return {isFirstSettlement, isLastSettlement};
			}

			/* If we're here, then callerHasSetSchedHandle must be true.
			 * I.e: an invoker has been created and co_awaited for one of the
			 * conditions.
			 * Therefore currentAwaitingCondition must also have been set,
			 * since currentAwaitingCondition is set in the invokers' ctors.
			 */
			assert(s.rsrc.currentAwaitingCondition != AwaitingCondition::NONE);

			if ((isFirstSettlement
					&& s.rsrc.currentAwaitingCondition == AwaitingCondition::FIRST_SETTLED)
				|| (isLastSettlement
					&& s.rsrc.currentAwaitingCondition == AwaitingCondition::ALL_SETTLED))
			{
				groupAwaiterSchedHandleToWake = s.rsrc.groupAwaiterSchedHandle;

				/** We only clear here and not in await_resume, because if
				 * the caller hasn't already set it schedHandle by the time we're
				 * called, then when it eventually does call await_suspend, it
				 * won't set it then either.
				 *
				 * I.e: callerSchedHandle only needs to be cleared it if gets set
				 * in the first place;
				 * And it only gets set if we need to invoke the schedHandle from
				 * here.
				 * If the group co_awaiter is able to call await_resume, then it
				 * simply doesn't set its schedHandle at all.
				 */
				s.rsrc.clearCallerSchedHandleState();
			}
		}

		if (groupAwaiterSchedHandleToWake)
		{
			/* We should be able to just directly resume() the group awaiter's handle
			 * here because that would invoke await_resume, which may destroy the
			 * callee's promise.
			 * And who is the callee? Is it not this coro here? And this coro
			 * hasn't been suspended. So we'd be destroying ourself while we're
			 * not suspended.
			 *
			 * But all of that only applies __IFF__ we actually do try to destroy
			 * the callee within the caller's Invoker. If we don't, then the callee
			 * should persist just fine. There's no implicit mechanism that
			 * will always destroy the callee coro state before the invoker
			 * is destroyed.
			 * If that was in fact the way it worked, then fire-and-forget coros
			 * would be impossible.
			 *
			 * So we should be able to call resume() directly here without
			 * post()ing to current_io_context().
			 *
			 *	EXPLANATION:
			 * However, in order to ensure that we keep this adapter coro
			 * method exception-free, we are forced to post() rather than
			 * directly calling the handle.
			 */
			boost::asio::post(
				current_io_context(), groupAwaiterSchedHandleToWake);
		}

		return {isFirstSettlement, isLastSettlement};
	}

	/**	EXPLANATION:
	 * This coro is a coro which has a promise, and does __not__ expose an awaitable
	 * iface and in fact should not be capable of being awaited, ultimately.
	 *
	 * Its purpose is to be an adapter that enables the Group class to invoke the
	 * invokers that are added to it, without having to co_await those invokers.
	 * Rather, the Group class simply invokes this function on them, and then this
	 * function both co_awaits the invoker on behalf of the Group class, and also
	 * performs the normal function of an invoker, which is both to invoke the
	 * target async fn, and also to convey its results back to the Group class.
	 * It's effectively a go-between coro that provides the outcomes that Invokers
	 * normally provide, without needing, itself, to be co_awaited.
	 */
	NonAwaitableNonPostingAdapterCoro nonAwaitableAdapterCoro(
		typename std::vector<SettlementDescriptor>::iterator settlementIt) noexcept
	{
		/**	EXPLANATION:
		 * It's very convenient that our design for the NonViralNonSuspendingInvoker
		 * coincidentally allows us to supply a lambda that can be used to test
		 * for the settlement conditions that are being waited on by the Group's
		 * co_awaiter.
		 */
		try {
			if constexpr (std::is_void_v<SubjectInvokerReturnType>) {
				co_await settlementIt->invoker.get();
			}
			else {
				settlementIt->returnValue = co_await settlementIt->invoker.get();
			}
		}
		catch (...)
		{
			settlementIt->calleeException = std::current_exception();
		}

		/* From here onwards, we mustn't throw(). Unhandled exceptions
		 * generated by the adapter coro itself will result in
		 * std::terminate().
		 */
		settlementIt->setSettlementStatus();
		updateSettlementsStateAndAwakenCallerIfConditionMet(settlementIt);

		co_return;
	}

	void add(Invoker &invoker)
	{
		typename std::vector<SettlementDescriptor>::iterator posIt;

		{
			sscl::SpinLock::Guard guard(s.lock);

			if (s.rsrc.groupAwaiterSchedHandle)
			{
				throw std::runtime_error(
					"add: New member invokers mustn't be added "
					"while co_awaiting a given set");
			}

			posIt = s.rsrc.settlements.emplace_back(invoker);
		}

		nonAwaitableAdapterCoro(posIt);
	}

	void checkForAndReThrowGroupExceptions() const
	{
		std::ostringstream ostream;
		bool doThrow = false;

		for (auto &item : s.rsrc.settlements)
		{
			if (item.type != SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
				continue;
			}

			assert(item.calleeException);

			doThrow = true;
			ostream << "Exc thrown in Group Adapter: ";
			try {
				std::rethrow_exception(item.calleeException);
			} catch (const std::exception &e) {
				ostream << e.what();
			} catch (...) {
				ostream << "<unknown exception type>";
			}
			ostream << "\n";
		}

		if (doThrow) {
			throw std::runtime_error(ostream.str());
		}
	}

	struct State
	{
		void clearCallerSchedHandleState() noexcept
		{
			groupAwaiterSchedHandle = nullptr;
			callerHasSetSchedHandle = false;
			currentAwaitingCondition = AwaitingCondition::NONE;
		}

		void setCallerSchedHandleAndCondition(
			std::coroutine_handle<> groupAwaiterSchedHandleIn,
			AwaitingCondition awaitingCondition) noexcept
		{
			groupAwaiterSchedHandle = groupAwaiterSchedHandleIn;
			callerHasSetSchedHandle = true;
			currentAwaitingCondition = awaitingCondition;
		}

		int firstSettledInvokerIdx = -1;
		int nInvokersSettled = 0;
		std::coroutine_handle<> groupAwaiterSchedHandle = nullptr;
		bool callerHasSetSchedHandle = false;
		/* calleWasReady*First* is an indelible record of what
		 * occured during the first settlement's adapter's update.
		 */
		bool calleeWasReadyToNotifyOfFirstSettlement = false;
		std::vector<SettlementDescriptor> settlements;
		AwaitingCondition currentAwaitingCondition = AwaitingCondition::NONE;
	};

	SharedResourceGroup<sscl::SpinLock, State> s;
};

} // namespace sscl::co

#endif // GROUP_H
