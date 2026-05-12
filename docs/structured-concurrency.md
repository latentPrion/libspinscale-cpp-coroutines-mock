Ok. We need to be able to compose groups of coros like:
```
sscl::co::Group group;

for (item: collection) {
	group.add(item.fooCReq);
}

co_await group.getAwaitAllSettled();
```

We need to be able support the following group wait operations:
* awaitFirstSettled(): Cancels all other in-flight invokers as soon
	as the first in the group is settled.
* awaitAllSettled(): Waits until all members are settled.

I think that every other group await operation can be implemented
atop these two, right?

----

Ok: so a few things have come out immediately. There are analogies between promise-bearing coros and non-promise bearing invokers. Invokers should ideally invoke their wrapped function inside of their constructor. Then the co_await operator will truly only be used to figure out whether the invoked async fn has been settled. Consider:

```
/* Non-coro (i.e: non-promise-bearing) invoker: */
struct OpenClInvoker
{
	template <Func, Args &&...>
	OpenClInvoker(Func, Args)
	{
		Func(Args);
	}

	auto await_ready() { return false; }
	await_suspend();
	await_resume();
};

/* Coro (i.e: promise-bearing) invoker: */
SomeInvokerType myCoroFn(arg0, arg1, arg2)
{
	co_return doStuffWith(arg0, arg1, arg2);
}

struct SomeInvokerType
{
	struct promise_type
	{
		get_return_object()
		{
			return SomeInvokerType();
		}
	};
};
```

Basically, OpenClInvoker and SomeInvokerType both perform analogous functions: they
initiate the async sequence's execution. The await_* functions are named aptly: they
check to see whether the result is ready. The differences between a coro invoker and a
non-coro invoker are that:
* The coro invoker's invoked fn is the body of the coro; whereas the non-coro invoker's invoked fn is passed into the invoker's ctor.
* The non-coro invoker's arguments are passed into the invoker's ctor; whereas the coro
  invoker's args are marshaled by the compiler into the promise. Then the compiler just
  initializes the invoker object and passes it back to the caller in order to preserve
  the abstraction of an invoker.

In the coro case, the invoker is the coro function signature. The returned invoker is
technically well-named as an awaitable, since it doesn't perform the act of invoking.
It performs the function of an awaiter for the results from the coro body.
However, overall, the C++20 feature which is known as an awaitable, is, in totality, an
invoker. It's an RAII object whose construction invokes (or accompanies the invocation of, in the coro case) a function, and then whose methods await that function's results.

In order to support co::Groups, we need to support async fn body settlement and signaling
of that settlement without needing the invoker to be co_awaited. Why?
* Because co_await suspends the caller.
* Because the next resume point for the caller is the invoker's await_resume.

Actually, what if we can get around this by using co_yield? Maybe there's a way for the
caller to call co_await on every member of the array, and to keep the callee coro alive,
and to allow the caller not to be suspended on merely __one__ of the invokers in the
co::Group, but rather to call co_await on all of the invokers in the co::Group in sequence, then return and finally co_await the co::Group itself? What if co_yield is the key to this?

----

# Solution: The Non-Awaitable Conveyor coroutine:

What we need is a coro which cannot itself be awaited, but which will await a given
invoker, and adapt that invoker's Awaitable interface to convey its results to the
Group object's internals.

NB: This will not transform the group invoker into a synchronous fn.
This is not an asynchronous bridge adapter.
It is a serial-to-parallel fanout adapter.
Each child is given a fanout adapter. I.e: there is 1 adapter per child.
Each adapter coro sits in the middle and creates a promise (continuation) for its own fanout child. The adapter coro is aware of both its child's invoker's lifecycle, and of
the Group object's internals. So it can act as the link between the group invoker
and its fanout child's invoker's results...without itself needing to be co_awaited.
It erases the need to be, itself, co_awaited, by:
* Not having a return value of its own.
* Conveying the return value of its fanout child to the parent group object.
* Returning without needing to be join'd to the group parent.
* The lifecycle of the adapter coros is managed by co-operative synchronization
  using shared state inside of the Group object.
* Hence the fanout adapters all get eagerly executed, but they all co-ordinate to awaken
  the group invoker only once.

We don't need the Group class to support cancelation. We can simply have the group
co_await caller cancel the individual invoked fns after the
`co_await group.getAwaitFirstSettlementInvoker()` call returns. Then the group co_awaiter
can manually cancel the individual invoked fns. The group co_awaiter must also call
co_await `group.getAwaitAllSettlementsInvoker()` as well in order to ensure that all
group-member invokers' fns have settled. This is to ensure that when the group
co_awaiter's promise exits, and it destroys the group-member promises, then it
won't be destroying promises whose fns were still being executed.

It's useful to note that even if we had added a generic cancelation management facility
in the Group class, it would still have had to wait for the canceled co_awaited fns.
I.e: it would still have had to call co_await `group.getAwaitAllSettlementsInvoker()`
after calling `co_await group.getAwaitFirstSettlementInvoker()`. We can't leave the
unsettled invoked fns running and then just destroy their promises while they're still
executing.

This is the minimum abstraction we can get away with. The group co_awaiter will always
know whether the group-members can be canceled, so it works out fine. The alternative is
to try and have the Group class pretend to know whether the member invokers' fns can
be canceled. This tends to complicate everything significantly.

Also, cancelation is not actually a broadly useful operation. It's only useful for
situations where one of a set of outcomes is exclusively desirable, and the others must
*appear* not to have happened.

Other situations of program early exit can and should be handled by normal code execution
to settlement.

There are some other cases where voluntary cancelation may be useful such as our current
async daemons, but again: that's not some broadly useful case. Those are very specifically daemons implemented as fire-and-forget kinds of coros which we set up to
periodically poll some atomic var, to tell them when to stop. They're extremely eccentric
edge cases and implementing a generalized cancelation framework on all invokers just to
support that is silly.



```cpp: group.h
#include "sharedResourceGroup.h"

namespace sscl::co {

template <>
concept AwaitableIface = <...>;

template <>
concept AwaiterIface = <...>;

template <>
concept AwaitablerOrAwaiterIface = <...>;

struct Group
{
	enum AwaitingCondition {
		NONE, FIRST_SETTLED, ALL_SETTLED
	};

	template <class AwaitableOrAwaiterIface, class SubjectInvokerReturnType>
	class SettlementDescriptor
	{
		enum TypeE {
			/* We track EXCEPTIION_THROWN but we don't provide an
			 * awaitInvoker for exception events. The caller can
			 * wait for settlements and then scan the result set
			 * to manually deal with exceptions.
			 */
			UNSETTLED, COMPLETED, EXCEPTION_THROWN
		};

		SettlementDescriptor(AwaitableOrAwaiterIface &_invoker)
		: invoker(std::ref(_invoker))
		{}

		void setSettlementStatus()
		{
			if (exception)
			{
				// ...
				type = EXCEPTION_THROWN;
			}
			else
			{
				// ...
				type = COMPLETED;
			}
		}

		TypeE type = UNSETTLED;
		SubjectInvokerReturnType returnValue;
		std::exception_ptr exception;
		std::reference_wrapper<AwaitableOrAwaiterIface> invoker;
	};

	// getAwaitNextSettlementInvoker();
	getAwaitFirstSettlementInvoker()
		{ return AwaitFirstSettlementInvoker(*this); }

	getAwaitAllSettlementsInvoker()
		{ return AwaitAllSettlementsInvoker(*this); }

	bool verifyAllInvokersSettled(void)
	{
		for (desc: s.rsrc.settlements) {
			if (desc.type == UNSETTLED) { return false; }
		}

		return true;
	}

	bool firstInvokerSettled(void)
		{ return s.rsrc.firstSettledInvokerIdx >= 0; }

	bool allInvokersSettled(void)
	{
		auto nInvokersAdded = s.rsrc.settlements.size();
		assert(s.rsrc.nInvokersSettled <= nInvokersAdded);
		return s.rsrc.nInvokersSettled == nInvokersAdded;
	}

	struct SettlementAwaitingInvoker
	{
		SettlementAwaitingInvoker(Group &_group)
		: parentGroup(_group)
		{}

		bool await_ready(void) { return false; }

		Group &parentGroup;
	};

	struct AwaitFirstSettlementInvoker
	:	public SettlementAwaitingInvoker
	{
		AwaitFirstSettlementInvoker(Group &_group)
		:	SettlementAwaitingInvoker(_group),
		{
			parentGroup.currentAwaitingCondition = FIRST_SETTLED;
		}

		bool await_suspend(std::coroutine_handle<> _groupAwaiterSchedHandle)
		{
			/* Nothing to do here: maybe we can check again?
			 * But otherwise, we store away the coro_handle of the
			 * awaiting caller, and suspend the caller.
			 */
			sscl::SpinLock::Guard(parentGroup.s.lock);

			if (parentGroup.s.rsrc.firstInvokerSettled())
				{ return false; }

			parentGroup.s.rsrc.groupAwaiterSchedHandle =
				_groupAwaiterSchedHandle;

			parentGroup.s.rsrc.callerHasSetSchedHandle = true;
			return true;
		}

		std::pair<
			SettlementDescriptor &,
			std::vector<SettlementDescriptor> &>
		await_resume()
		{
			assert(parentGroup.s.rsrc.firstSettledInvokerIdx >= 0);

			return {
				parentGroup.s.rsrc.settlements[firstSettledInvokerIdx],
				parentGroup.s.rcrc.settlements
			}
		}
	};

	struct AwaitAllSettlementsInvoker
	:	public SettlementAwaitingInvoker
	{
		AwaitFirstSettlementInvoker(Group &_group)
		:	SettlementAwaitingInvoker(_group),
		{
			parentGroup.currentAwaitingCondition = ALL_SETTLED;
		}

		bool await_suspend(std::coroutine_handle<> _groupAwaiterSchedHandle)
		{
			sscl::SpinLock::Guard(parentGroup.s.lock);

			if (parentGroup.allInvokersSettled())
				{ return false; }

			parentGroup.s.rsrc.groupAwaiterSchedHandle
				= _groupAwaiterSchedHandle;

			parentGroup.s.rsrc.callerHasSetSchedHandle = true;
			return true;
		}

		std::vector<SettlementDescriptor> &await_resume()
		{
			assert(parentGroup.allInvokersSettled());
			return parentGroup.s.rcrc.settlements;
		}
	};

	struct NonAwaitableAdapterCoro
	{
		struct promise_type
		{
			NonAwaitableAdapterCoro get_return_object() { return {}; }
			std::suspend_never initial_suspend() { return {}; }
			std::suspend_never final_suspend() { return {}; }
			void return_void() { return; }
			void unhandled_exception()
			{
				/**	FIXME:
				 * We probably want to convey the exception from the co_awaited
				 * callee invoker along to the group awaiter.
				 */
			}
		};

		operator co_await=delete;
		bool await_ready() { std::terminate(); return false; }
		void await_suspend() { std::terminate(); }
		void await_resume() { std::terminate(); }
	}

	std::pair<bool, bool>
	updateSettlementsStateAndAwakenCallerIfConditionMet(
		std::vector<SettlementDescriptor>::iterator &_settlementIt)
	{
		bool isFirstSettlement=false, isLastSettlement=false;

		{
			sscl::SpinLock::Guard(s.lock);

			s.rsrc.nInvokersSettled++;

			if (s.rsrc.firstSettledInvokerIdx < 0)
			{
				isFirstSettlement = true;
				s.rsrc.firstSettledInvokerIdx = std::distance(
					s.rsrc.settlements.begin(), settlementIt);

				s.rsrc.calleeWasReadyToNotifyOfFirstSettlement = true;
			}

			if (s.rsrc.nInvokersSettled >= s.settlements.size())
			{
				assert(s.rsrc.nInvokersSettled == s.settlements.size());
				assert(verifyAllInvokersSettled() == true);
				isLastSettlement = true;
				s.rsrc.calleeWasReadyToNotifyOfLastSettlement = true;
			}

			if (!s.rsrc.callerHasSetSchedHandle) { return; }
		}

		if (s.rsrc.currentAwaitingCondition == NONE) { return; }

		if (isFirstSettlement && s.rsrc.currentAwaitingCondition == FIRST_SETTLED
			|| isLastSettlement && s.rsrc.currentAwaitingCondition == ALL_SETTLED)
		{
			s.rsrc.groupAwaiterSchedHandle.resume();
		}
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
	template <class SubjectInvokerReturnType>
	NonAwaitableAdapterCoro nonAwaitableAdapterCoro(
		std::exception_ptr &exceptionOccured, std::function<void()> cbFn,
		std::vector<SettlementDescriptor>::iterator &settlementIt,
		bool awaitingFirstSettlement, bool awaitingAllSettlements)
	{
		/**	EXPLANATION:
		* It's very convenient that our design for the NonViralNonSuspendingInvoker
		* coincidentally allows us to supply a lambda that can be used to test
		* for the settlement conditions that are being waited on by the Group's
		* co_awaiter.
		*/
		settlementDesc.returnValue = co_await settlementIt->invoker;
		settlementIt->setSettlementStatus();
		updateSettlementsStateAndAwakenCallerIfConditionMet(
			settlementIt);

		co_return;
	}

	template <class SubjectInvokerReturnType>
	NonAwaitableAdapterCoro nonAwaitableAdapterCoro<void>(
		std::exception_ptr &exceptionOccured, std::function<void()> cbFn,
		std::vector<SettlementDescriptor>::iterator &settlementIt)
	{
		co_await settlementIt->invoker;
		settlementIt->setSettlementStatus();
		updateSettlementsStateAndAwakenCallerIfConditionMet(
			settlementIt);

		co_return;
	}

	template <>
	void add(AwaitableOrAwaiterIface &_invoker)
	{
		auto posIt = s.settlements.push_back(_invoker);

		nonAwaitableAdapterCoro(
			posIt->exception, onSettledCb, posIt);
	}

	static onSettledCb() {}

	struct State
	{
		int firstSettledInvokerIdx=-1, nInvokersSettled=0;
		std::coroutine_handle<> groupAwaiterSchedHandle;
		bool callerHasSetSchedHandle=false,
			calleeWasReadyToNotifyOfFirstSettlement=false,
			calleeWasReadyToNotifyOfLastSettlement=false;
		std::vector<SettlementDescriptor> settlements;
	};

	AwaitingCondition currentAwaitingCondition=AwaitingCondition::NONE;
	SharedResourceGroup<sscl::SpinLock, State> s;
};

}
```
