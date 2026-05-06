Ok. It turns out that the internal async segment index to the next async segment to be resumed by coroutine_handle::resume(), only gets updated __after__ initial_suspend/final_suspend exit.

So we have a situation where we execute the post to callee on the caller thread; and if the caller doesn't exit intial_suspend quickly enough, then when the callee resume()s the posted hndle, it'll resume from the start of initial_suspend, and thus it'll post to itself twice.

We need to refactor things so that we perform the post-to in initial_suspend and the post-back in final_suspend, inside of awaitables that are returned by initial/final_suspend. This way the internal async segment index should have been updated by the time the invocable posts/posts-back.

So I'll need the PostingPromise class to have a 2 new nested classes with a common base:

```
template<>
struct InitialSuspendPostingInvoker
:	public suspend_always
{
	InitialSuspendPostingInvoker(
		boost::asio::io_context &_calleeIoContext,
		std::coroutine_handle<> _calleeSchedHandle)
	: targetIoContext(_calleeIoContext),
	targetSchedHandle(_calleeSchedHandle)
	{}

	bool await_suspend()
	{
		boost::asio::post(targetIoContext, targetSchedHandle);
		return true;
	}

	boost::asio::io_context &targetIoContext;
	std::coroutine_handle<> targetSchedHandle;
};

/* Add friend relationships to the CalleeFlowExecutor to enable
 * this class to access its internals.
 */
struct FinalSuspendPostingInvoker
:	public suspend_always
{
	template<>
	FinalSuspendPostingInvoker(CalleeFlowExecutor &_calleeFlowExecutor)
	: cfe(_calleeFlowExecutor)
	{}

	bool await_supend()
	{
		cfe();
		// Suspend so that the caller can destroy the callee promise.
		return true;
	}

	/* Intentionally a copy and not a reference to the passed-in
	 * CalleeFlowExecutor object.
	 */
	PostingPromise::PostBackStatus::CalleeFlowExecutor cfe;
};
```

Then in PostingPromise::initial/final_suspend, we can do something like:

```
initial_suspend()
{
	return InitialSuspendPostingInvoker(calleeIoContext, selfSchedHandle);
}

final_suspend()
{
	return FinalSuspendPostingInvoker(
		postBackStatus.getCalleeFlowExecutor());
}
```
