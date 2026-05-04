Ok. I think we've figured out how to solve this problem. We can take advantage of the fact that the state which must be modified by the callee when it's completed its execution is something which is accessible to the caller. So we can have the callee set a bool on its promise when it's ready to post-back to the caller. And, the caller also sets a flag when it's ready to set its callerSchedHandle. This way both the caller and callee can synchronize and decide on how the caller should be notified of the callee's completion.

In this design we get to keep the benefits of immediate execution (the callee begins executing as soon as the coro is invoked) but we also get the benefit of being able to have the callee lazily only check whether the caller has set its callerSchedHandle in the callee's final_suspend.

It's the best of both worlds. And we can do it without even using a condvar, and without busy-spinning as well.

I.e:

## Structural changes:

* Remove condvar from PostingPromise.
* Add new class PostBackStatus to PostingPromise:
```
class PostBackStatus
{
public:
	class FlowExecutor
	{
	protected:
		FlowExecutor(PostBackStatus &_parent)
		: parent(_parent)
		{}

		PostBackStatus &parent;
	};

	class CalleeFlowExecutor
	:	public FlowExecutor
	{
	public:
		using FlowExecutor::FlowExecutor;

		void operator()()
		{
			sscl::SpinLock::Guard(parent.lock);
			parent.calleeIsReadyToPostBack = true;
			if (parent.callerHasSetCallerSchedHandle)
			{
				boost::asio::post(
					parent.calleePromise.callerIoContext,
					parent.calleePromise.callerSchedHandle);
			}
			/* If the caller hasn't set its schedHandle yet,
			 * then do nothing.
			 */
		}
	};

	class CallerFlowExecutor
	:	public FlowExecutor
	{
	public:
		using FlowExecutor::FlowExecutor;

		bool operator()()
		{
			sscl::SpinLock::Guard(parent.lock);
			parent.callerHasSetCallerSchedHandle = true;
			if (parent.calleeIsReadyToPostBack)
			{
				/* Callee already ran final_suspend: caller must not
				 * post/dispatch callerSchedHandle to itself. Return false
				 * so await_suspend does not suspend; the compiler runs
				 * await_resume synchronously on the caller thread.
				 */
				return false;
			}
			/* Callee still running: caller should suspend until callee's
			 * CalleeFlowExecutor posts callerSchedHandle to callerIoContext.
			 */
			return true;
		}
	};

	PostBackStatus(PostingPromise &_calleePromise)
	: calleePromise(_calleePromise)
	{}

	void reset() noexcept
	{
		sscl::SpinLock::Guard(lock);
		callerHasSetCallerSchedHandle = false;
		calleeIsReadyToPostBack = false;
	}

	CalleeFlowExecutor getCalleeFlowExecutor(void)
		{ return CalleeFlowExecutor(*this); }

	CallerFlowExecutor getCallerFlowExecutor(void)
		{ return CallerFlowExecutor(*this); }

private:
	PostingPromise &calleePromise;
	sscl::SpinLock lock;
	bool callerHasSetCallerSchedHandle,
		calleeIsReadyToPostBack;
};
```

## Callee side:

Promise is constructed;
get_return_object is called;
Invocable is returned by coro;
initial_suspend is called;
	caller posts callee schedHandle to callee's io_context.
Callee begins executing on callee thread.
final_suspend is called;
	Callee runs CalleeFlowExecutor: sets calleeIsReadyToPostBack;
	Callee checks callerHasSetCallerSchedHandle.
	If callerHasSetCallerSchedHandle == true:
		post back to caller from callee (asio::post to callerIoContext).
	else
		/* Do nothing:
		 * We don't have the callerSchedHandle so we can't post a
		 * complete message to the caller.
		 * The caller will eventually setCallerSchedHandle; if the caller
		 * co_awaits before we finish, CallerFlowExecutor returns true and
		 * the caller suspends until this CalleeFlowExecutor posts.
		 */

## Caller side:

Invoker return object is retrieved from the callee's coro;
Eventually caller invokes co_await on returned invoker;
	Caller calls await_suspend;
		Caller sets setCallerSchedHandle (stores callerSchedHandle, etc.);
		Caller runs CallerFlowExecutor (sets callerHasSetCallerSchedHandle,
		checks calleeIsReadyToPostBack).
		Caller returns the bool from CallerFlowExecutor::operator()()
			as the result of await_suspend:
		* If calleeIsReadyToPostBack: return false — do not post-back
		  the caller to itself; await_resume runs immediately on the
		  caller thread (fast path).
		* If callee is not ready: return true — caller suspends until
		  callee's CalleeFlowExecutor posts callerSchedHandle to
		  callerIoContext, then await_resume runs.

In PostingInvoker::await_resume (the caller's await_resume on the viral
invoker), call postBackStatus.reset() so both bools are cleared for a
clean state before or after consuming return values (implementation
chooses one consistent place; document in code).

## Common notes:

The callee still returns suspend_always from final_suspend.

The caller returns **false** from await_suspend when the callee was
already ready to post-back (calleeIsReadyToPostBack): **no** self-post
or self-dispatch; synchronous await_resume.

The caller returns **true** from await_suspend when the callee was not
ready yet: caller suspends until the callee's CalleeFlowExecutor posts
callerSchedHandle onto callerIoContext, then await_resume runs on the
caller executor.

PostBackStatus::reset() clears both bools under the spinlock and is
invoked from the caller's await_resume on the viral posting invoker.
