Read this exchange to understand the context of my requested changes:

Hey, we use await_suspend to set setCallerSchedHandle. This appears to be timing sensitive rather than strictly ordered since initial_suspend is called whether or not we co_await. Hence, we can imagine that we call initializeCReq without co_awaiting its retobj. It will post(body, calleeSchedHandle), and bgin executing on body immediately whenever body's io_context picks up that calleeSchedHandle.

It's entirely plausible for main to go to sleep(5s), and then during that time, body finishes executing the fnbody of initializeCReq. Then body will call final_suspend, which will find that main ahs not yet set the callerSchedHandle (because main is slept(5s)), and so it finds no mechanism to re-post to caller.

This is timing sensitive and thus unreliable. Correct?
Then later main may wake up and set setCallerSchedHandle, but it's now too late. The callee body thread will never re-post its callback to main, and thus the callee promise will never get destroy()ed, and also, after main calls co_await, it will never wake up.

----
Your timing story is right **for a design where `final_suspend` depends on `callerSchedHandle` being filled in only from `await_suspend` on a `co_await` that might happen later** (the viral path).

**`initializeCReq` / non-viral is not that design.** In your code, `final_suspend` prefers `callerSchedHandle`, but if it is null it uses **`callerLambda`**:

```161:177:/media/latentprion/aafe96c9-7fcd-40ce-991d-ca2d23b5ba17/gits/coroutines/src/promises.h
	std::suspend_always final_suspend() noexcept
	{
		if (callerSchedHandle)
		{
			// ...
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
```

Non-viral `initializeCReq` is invoked with **`(exception_ptr &, std::function<...>)`**, which feeds the **two-argument `PostingPromise` constructor** and sets **`callerLambda`** before the body runs on body. **`get_return_object`** even requires that lambda to be non-null. So when body later runs **`final_suspend`**, **`callerSchedHandle`** may still be unset (you never **`co_await`** the non-viral invoker, so its **`await_suspend`** never runs), and that is fine: the **`callerLambda`** branch still posts completion back to **`callerIoContext`** (captured when the promise was built on main, via **`current_io_context()`**).

So for **this** non-viral pattern: **not** unreliable in the way you described; completion does **not** wait on **`setCallerSchedHandle`**.

Where your analysis **does** apply:

- **Viral** coroutines that complete **`final_suspend`** before any caller **`co_await`**s them: then **`callerSchedHandle`** is still null, **`callerLambda`** is typically empty, and you hit the **“No mechanism…”** branch — leak / broken wake-up as you said.
- Any hypothetical path that relied only on **`callerSchedHandle`** without a **`callerLambda`** fallback would have the same race if **`co_await`** could be delayed or omitted.

**Summary:** **`await_suspend` → `setCallerSchedHandle`** is ordering-sensitive for **viral** “wire the caller handle at **`co_await`** time.” **`initial_suspend`** always runs early; that is orthogonal to the non-viral **lambda** completion path, which avoids that particular race for **`initializeCReq`**.

----
How can I either feed callerSchedHandle to the callee before final_suspend on the callee is invoked; or strictly order the operations (bsically requires a signaling mechanism between caller and callee, possibly a coro-aware sleep and coro-aware wake); or cause final_suspend to re-enqueue itself and poll until callerSchedHandle has been called?

----
Here are practical ways to think about it; they differ in where you put the “wait for the caller handle” logic.

### 1. **Guarantee the handle exists before `final_suspend` runs (best: extra suspend point)**

`callerSchedHandle` only exists after the caller has actually reached **`co_await`** and **`await_suspend`** runs. So the callee must **not run to `final_suspend` until that has happened**.

The clean way is **not** to rely on `final_suspend` racing the caller, but to add an explicit **`co_await`** barrier **before** `co_return` (or before any work that must see the handle):

- Custom awaitable: “wait until `callerSchedHandle` is non-null (or a ‘registered’ flag is set)”.
- **`await_suspend` on the caller’s invoker** (or `PostingInvoker::setCallerSchedHandle`) sets the flag / stores the handle and **posts the callee’s continuation** on the callee executor so it can resume.

That is your “coro-aware wake”: it is just **“suspend callee until an event”**, where the event is “caller registered the handle”. Ordering is strict: callee blocks on that await; caller’s `await_suspend` fires first (when caller suspends), then it signals the callee.

You cannot get strict ordering **without** some equivalent of that (memory + wake, or a scheduler primitive).

---

### 2. **Strict ordering with a signaling primitive (same idea, lower-level)**

Use a **latch / counting_semaphore / condition_variable + mutex / atomic + manual post** between:

- **caller:** in `await_suspend`, set handle (and any other state), then **signal** and optionally **post** to the callee’s `io_context`.
- **callee:** blocked on that signal (either in a plain wait if you’re OK blocking a worker thread, or in **`co_await`** as above so the thread isn’t blocked).

“Coro-aware sleep” is usually **“`co_await` something that completes later”** (timer, completion queue, or a posted completion), not `std::this_thread::sleep`.

---

### 3. **`final_suspend` polls / re-enqueues until `callerSchedHandle` is set**

Possible in theory, awkward in practice:

- `final_suspend()` must return an awaiter; you can return a **custom awaiter** whose `await_ready()` is false until the handle appears, and whose `await_suspend()` **schedules** the next check (e.g. `post` self with a short delay, or wait on a synchronization object from an Asio async op). That avoids blocking the executor thread with a spin loop.
- Downsides: **if the caller never `co_await`s**, you wait forever (or you need a timeout and explicit failure). You also extend the coroutine’s lifetime in a less obvious way.

So it is a **fallback** pattern, not as clear as an explicit **`co_await caller_wired{}`** before completion.

---

### 4. **“Feed `callerSchedHandle` before `final_suspend`” without a new `co_await`**

The only way to get the handle **before** the callee’s coroutine runs is if the **caller has already suspended** at a `co_await` that passes that handle **before** you `post` the callee’s body to another thread. That usually means **restructuring the API**: e.g. caller first `co_await`s a “prepare callee launch” object that captures `callerSchedHandle`, then that object posts work to body. Your current pattern (“callee posts itself in `initial_suspend`” immediately) makes “caller always first” hard without an extra gate.

---

### Summary

| Approach | Idea |
|----------|------|
| **Explicit `co_await` gate** | Callee waits on an awaitable until `setCallerSchedHandle` / equivalent runs; then `co_return` → `final_suspend` sees the handle. |
| **Sync + post** | Same, implemented with latch/CV/atomic + `asio::post` to resume the callee. |
| **`final_suspend` custom awaiter** | Deferred completion / “poll” via reposts until handle or timeout. |
| **API change** | Caller suspends and passes handle **before** callee is scheduled on body. |
| **Non-viral lambda** | Avoids `callerSchedHandle` entirely for that use case (what you already do for `initializeCReq`). |

For **viral** “fire-and-forget” while still needing a caller handle, you **must** add one of: a **pre-`final_suspend` await**, a **`final_suspend` awaiter that waits**, or a **launch protocol** that wires the handle before the callee runs. There is no magic ordering without such a mechanism.

----
It seems that PostingPromises need a coro-aware condvar. The condvar begins unset, and when it is set, then final_suspend can safely read the member callerSchedHandle.

----

Your intuition matches the problem: **`final_suspend` must not assume `callerSchedHandle` is ready unless something can suspend the callee until the caller has run `await_suspend`.** Framing that as a “condvar” is fine conceptually; in coroutine + **`io_context`** code you usually implement it as **an awaitable + handoff**, not **`std::condition_variable`** on a thread that must not block.

### Why not a literal `std::condition_variable` on the executor thread?

A worker running **`context.run()`** should avoid blocking on a pthread condvar while holding implicit executor assumptions. You want **“suspend this coroutine, resume later when signaled”**, i.e. **non-blocking** integration with the async runtime.

### What “coro-aware condvar” usually is here

A small **synchronization object** plus **one or more `coroutine_handle`s** (or `asio::post` targets):

- **Unset:** callee reaches the gate (your new **`co_await`**, or a **custom `final_suspend` awaiter**) and **suspends**; store its handle (or post a completion to the right executor).
- **Set:** in **`await_suspend`** / **`setCallerSchedHandle`**, after writing **`callerSchedHandle`**, mark state **set** and **resume** the waiting coroutine (or **`post`** the resume onto the callee’s executor if the caller must not run callee code).

That gives the same **happens-before** you want from a condvar: **set** happens after the handle is stored; **wait** side only reads **`callerSchedHandle`** after seeing **set**.

### Wiring it to `PostingPromise`

Reasonable shape:

- **State:** e.g. `enum { Pending, Ready }` or `atomic` + optional `std::coroutine_handle<>` to resume (or a small queue if you ever need multiple waiters—usually one).
- **Wait:** either  
  - an explicit **`co_await posting_promise.await_caller_wired()`** before `co_return`, or  
  - **`final_suspend`** returns a **custom awaiter** whose `await_ready()` is true only once “wired”, and `await_suspend` registers for wake when not ready.
- **Signal:** **`PostingInvoker::setCallerSchedHandle`** (or the invoker’s **`await_suspend`**) calls **`promise.signal_caller_wired()`** after assigning **`callerSchedHandle`**.

### Non-viral / lambda path

You still want **`callerLambda`** to complete without waiting on a handle. The gate should apply only when you **intend** to use **`callerSchedHandle`** (viral path), or use a policy template / runtime branch: **if lambda present, skip wait; else wait (or timeout).**

### Failure modes to design for

- **Caller never `co_await`s:** wait is infinite unless you add **timeout**, **assert**, or **logging + destroy policy**.
- **Who resumes whom on which executor:** avoid running callee destructor or arbitrary callee code on the wrong thread; **`asio::post(calleeExecutor, ...)`** for the resume is a common pattern when the signal originates on the caller thread.

So yes: **`PostingPromise` benefits from a coroutine-style “condvar” (awaitable gate) keyed off “caller wired”**, with **`setCallerSchedHandle`** as the **notify** side. The important implementation detail is **awaitable + explicit ordering**, not a blocking OS condvar on an **`io_context`** worker.

----
Use class CoQutex as a reference and implement a coro-aware condvar which will solve this problem, by allowing me to add a member CoConditionVariable (coConditionVariable.h) to class PostingPromise. This way we can wait on that condvar in PostingPromise::final_suspend, and we can signal that condvar in await_suspend. Final_suspend should now return this new CoConditionVariable's WaitForInvoker as its return type.

We would need to reorder the if () checks in PostingPromise final_suspend (and document why) to ensure that we check for the callerLambda before waiting on callerSchedHandle. And we shouldn't read callerSchedHandle inside of final_suspend before successfully acquiring the condvar anymore. So we end up not being able to meaningfully detect when no mechanism has been provided for post-back, since we assume that if no callerLambda is set then it must be a callerSchedHandle situation and we must wait.

For the CoConditionVariable class, its getWaitForInvoker() method returns a WaitForInvoker. We return this from final_suspend. This invoker, when co-awaited, will acquire the internal spinlock, check if isSignaled is true. If isSignaled==true, then it returns suspend_never and continues executing. If isSignaled==false, then it enqueues itself (i.e, the callerSchedHandle coroutine_handle given to it in await_suspend) on the internal WaitingCoroutines collection (with an accompanying current_io_context()) and then returns suspend_always.

CoConditionVariable::signal() first acquires the spinlock; then sets isSignaled=true, and then unconditionally posts the callerSchedHandles of all WaitingCoroutines to their respective captured values of current_io_context, as captured during await_suspend.

CoConditionVariable::clear() first acquires the spinlock; then unconditionally sets isSignaled=false. It doesn't do anything to the condvar's internal waitingCoroutines list.

----

But we can't have final_suspend return an awaiter that decides its behaviour, because the contents of final_suspend are the steps that will post to calleSchedHandle. Correct? Unless you know soething I don't which is likely here?

If we refuse to actually enqueue onto callerSchedHandle inside of final_suspend, and we instead return a suspend_always (?) in there, this makes the caller suspend. ... but the caller is the callee here, because it's the callee who was originally posted to his callee io_context and then he called final_suspend from that io_context's invocation of him. This is an interesting details. 

At any rate we still need a signal from the original caller which posted the original coro handle to the callee io_context so none of this helps. So final_suspend should call condvar.waitFor directly within final_suspend, if callerLambda isn't set. When final_suspend awakens, it will resume executing after the co_await for condvar.waitFor, and thus it will be able to safely post the callerSchedHandle to the original caller's callerIoContext.

----

Ok. I wan tto try to do this from the initial_suspend insted of final_suspend. So we'll be trying to waitFor the condvar to be set inside of initial_suspend. So now we'll keep the logic for posting to the callee (posting to *ThreadTag::io_context()) in initial_suspend if we can -- or we may need to move it into some kind of callback that's invoked by the condvar on signal(), or something.

The idea is:
```cpp
struct WaitForInvoker
:	public suspend_always
{
	await_suspend(coroutine_handle<> cvCallerSchedHandle)
	{
		sscl::SpinLock::Guard(spinLock);
		if (!cv.isSignaled)
		{
			waitingCoroutines.emplace_back(
				current_io_context(), cvCallerSchedHandle);

			return true;
		}

		return false;
	}

	void await_resume()
	{
		invokeOnSignaledCb();
	}

	void invokeOnSignaledCb()
	{
		if (onSignaledCb) {
			onSignaledCb();
		}
	}

	CoConditionVariable &cv;
	std::vector<WaitingCoroutines> waitingCoroutines;
	std::function<> onSignaledCb;
};

struct PostingPromise
{
	struct FinalSuspendPostingInvoker
	:	public suspend_always
	{
		FinalSuspendPostingInvoker(
			boost::asio::io_context &_callerIoContext,
			PostingPromise &_calleePromise)
		: callerIoContext(_callerIoContext),
		calleePromise(_calleePromise)
		{}

		virtual bool await_ready()=0;
		virtual bool await_suspend()=0;
		virtual void await_resume()=0;

		boost::asio::io_context &callerIoContext;
		PostingPromise &calleePromise;
	};

	struct NonViralInitialSuspendPostingInvoker
	:	public FinalSuspendPostingInvoker
	{
		NonViralInitialSuspendPostingInvoker(
			boost::asio::io_context &_callerIoContext
			PostingPromise &_calleePromise,
			std::function<> _callerLambda)
		:	FinalSuspendPostingInvoker(_callerIoContext, _calleePromise)
		callerLambda(_callerLambda)
		{}

		bool await_ready() override { return false; }
		bool await_suspend() override
		{
			boost::asio::post(
				calleeIoContext,
			[&calleePromise]
			{
				calleePromise.callerLambda(
					CalleeCoroutineHandleDestroyer(
						calleePromise.selfSchedHandle));
			});

			return true;
		}

		void await_resume() override {}

		std::function<> callerLambda;
	}

	struct ViralInitialSuspendPostingInvoker
	:	public FinalSuspendPostingInvoker
	{
		ViralInitialSuspendPostingInvoker(
			boost::asio::io_context &_callerIoContext,
			PostingPromise &_calleePromise,
			CoConditionVariable::DecisionEnablingDerivableWaitForInvoker
				&_decisionEnablingInvoker)
		:	FinalSuspendPostingInvoker(_callerIoContext, _calleePromise)
		decisionEnablingInvoker(_decisionEnablingInvoker),
		{}

		bool await_ready() override { return false; }

		/**	EXPLANATION:
		 * DecisionEnablingDerivableWaitForInvoker intentionally
		 * doesn't implement await_suspend -- instead it exposes a
		 * method `awaitSuspend` which is intended to be called by its
		 * derived classes, or by classes which hold an instance of
		 * that invoker.
		 *
		 * So this class calls upon
		 * DecisionEnablingDerivableWaitForInvoker::awaitSuspend,
		 * as part of its own implementation of await_suspend.
		 */
		bool await_suspend(template<> coroutine_handle cvCallerSchedHandle)
			override
		{
			decisionFactors = decisionEnablingInvoker.awaitSuspend(
					cvCallerSchedHandle);

			if (decisionFactors.isSignaled)
			{
				boost::asio::post(
					this->callerIoContext, promise.callerSchedHandle);
			}

			decisionFactors.cvInternalSpinLock.release();
			return true;
		}

		void await_resume() override
		{
			if (!decisionFactors.isSignaled)
			{
				boost::asio::post(
					this->callerIoContext, promise.selfSchedHandle);
			}
		}

		// Deliberately a copy and not a ref.
		DecisionEnablingDerivableWaitForInvoker decisionEnablingInvoker;
		DecisionEnablingDerivableWaitForInvoker::DecisionFactors
			decisionFactors;
		PostingPromise &promise;
		boost::asio::io_context &calleeIoContext;
	};

	/**	EXPLANATION:
	 * initial_suspend always posts to the callee, unconditionally.
	 * The logic to wait for the caller's callerSchedHandleIsSet
	 * condvar will be deferred until final_suspend. This gives the
	 * caller ample time to set its schedHandle on the callee's promise.
	 *
	 * In theory this should enable us to avoid the overhead of the
	 * extra condvar suspend+post() for most cases.
	 *
	 * In the real code this would be in the TaggedPostingPromise so
	 * that it'll have access to ThreadTag::io_context.
	 */
	suspend_always initial_suspend()
	{
		boost::asio::post(ThreadTag::io_context(), calleeSchedHandle);
	}

	FinalSuspendPostingInvoker final_suspend()
	{
		if (callerLambda)
		{
			return NonViralInitialSuspendPostingInvoker(
				callerIoContext, *this,
				callerLambda);
		}

		return ViralInitialSuspendPostingInvoker(
			callerIoContext, *this,
			callerSchedHandleIsSetCv
				.getDecisionEnablingDerivableWaitForInvoker());
	}
};

class CoConditionVariable
{
	struct OperationInvoker
	{
		OperationInvoker(CoConditionVariable &_cv)
		: parentCv(_cv)
		{}

		CoConditionVariable &parentCv;
	};

	struct WaitForInvoker
	:	public OperationInvoker
	{
		using OperationInvoker::OperationInvoker;

		bool await_ready() { return false; }

		bool await_suspend(
			template<> coroutine_handle cvCallerSchedHandle)
		{
			sscl::SpinLock::Guard(parentCv.spinLock);
			if (parentCv.isSignaled) {
				return false;
			}

			parentCv.waitingCoroutines.emplace_back(
				current_io_context(), cvCallerSchedHandle);

			return true;
		}

		void await_resume() {}
	};

	WaitForInvoker getWaitForInvoker()
		{ return WaitForInvoker(*this); }

	/**	EXPLANATION:
	 * This class' co_await hooks are intentionally named wrongly
	 * so that co_await will not work on this class. This class is
	 * not intended to be co_awaited directly. It is intended to be
	 * serve as a base class for some other, derived invoker class.
	 * The derived invoker can then call upon this class' co_await
	 * machinery in order to make higher level decicions around
	 * locking and coro suspension.
	 *
	 * We actually want to ensure that calling co_await on a
	 * DecisionEnablingDerivableWaitForInvoker will result in a
	 * compiler error.
	 */
	struct DecisionEnablingDerivableWaitForInvoker
	:	public OperationInvoker
	{
		struct DecisionFactors
		{
			sscl::SpinLock &cvInternalSpinLock;
			bool isSignaled
		};

		using OperationInvoker::OperationInvoker;

		awaitReady() { return false; }

		DecisionFactors awaitSuspend(
			template<> coroutine_handle cvCallerSchedHandle)
		{
			/**	CAVEAT:
			 * The derived caller must explicitly release the spinlock
			 * and also decide on suspension policy.
			 */
			parentCv.spinLock.acquire();
			if (isSignaled) {
				return DecisionFactors{ parentCv, isSignaled };
			}

			parentCv.waitingCoroutines.emplace_back(
				current_io_context(), cvCallerSchedHandle);
			
			return DecisionFactors{ parentCv, isSignaled };
		}
		void awaitResume() {}
	};

	/* Derived caller must explicitly release the spinlock,
	 * and also decide on suspension policy.
	 */
	getDecisionEnablingDerivableWaitForInvoker()
		{ return DecisionEnablingDerivableWaitForInvoker(*this); }

	signal()
	{
		{
			sscl::SpinLock::Guard(spinLock);
			cvWaiters = CLONE_COLLECTION(waitingCoroutines);
			isSignaled = true;
		}

		for (cvw: cvWaiter) {
			boost::asio::post(cvw.ioContext, cvw.schedHandle);
		}
	}

	clear()
	{
		sscl::SpinLock::Guard(spinLock);
		isSignaled = false;
	}
};
```

Can you help me to implement the conditional nature of what I want to do? Which is: I want the initial_suspend (or the invocable that it returns) to immediately post to the callee and suspend  if the condvar is already set; or to suspend on the cv until it gets set, and then post to the callee and suspend.

