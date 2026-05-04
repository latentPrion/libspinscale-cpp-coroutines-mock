# Caller Pull Own SchedHandle

## Problem

Viral posting coroutines need the callee's `final_suspend()` path to resume or post
the original caller continuation. That requires `callerSchedHandle` to have been
stored by the invoker's `await_suspend()`.

The current condition-variable style can make this work, but it is easy to misuse:
if `final_suspend()` queues the completed callee coroutine handle and later resumes
that callee handle, the program has undefined behavior. A coroutine suspended at
`final_suspend()` is at a terminal suspend point; it should be destroyed later, not
resumed.

Moving the wait to `initial_suspend()` is lifetime-correct because the callee still
has a real body entry point to resume into. However, it is pessimistic. It forces
the common path through a synchronization gate before the callee body starts, even
though the caller will often set `callerSchedHandle` almost immediately.

## Caller-Pull-Own-SchedHandle Idea

Keep the eager callee start. In `final_suspend()`, if the caller handle is not ready
yet, do not block the thread and do not enqueue the final-suspended callee as a
normal condition-variable waiter.

Instead, post a small completion probe back to the callee's own `io_context`. The
probe checks whether `callerSchedHandle` is ready. If it is ready, the probe posts
or resumes the caller continuation on the caller's executor. If it is not ready,
the probe reposts itself.

In other words, the callee does not busy-wait by burning CPU inside
`final_suspend()`. It "busy-waits" cooperatively by yielding back to its event loop
between checks.

Sketch:

```text
final_suspend.await_suspend(callee):
    if callerSchedHandle is ready:
        post callerSchedHandle to callerIoContext
        return true

    post completion_probe to calleeIoContext
    return true

completion_probe:
    if callerSchedHandle is ready:
        post callerSchedHandle to callerIoContext
        return

    post completion_probe to calleeIoContext
```

The completed callee frame remains suspended at `final_suspend()` until the caller's
`await_resume()` consumes the result and destroys it through the existing frame
ownership path.

## Why It Helps

- Preserves eager execution of the callee body.
- Avoids a non-responsive spin inside `final_suspend()`.
- Avoids resuming a coroutine already suspended at `final_suspend()`.
- Keeps the fast path cheap when `callerSchedHandle` has already been set.
- Converts the late-caller case into cooperative event-loop polling.

## Required Synchronization

This still needs a real cross-thread synchronization mechanism for the readiness
flag and stored handle. A plain write in `setCallerSchedHandle()` and plain read in
`final_suspend()` would be a data race if they can happen on different threads.

Possible implementation directions:

- Use an atomic readiness flag with release/acquire ordering.
- Store the caller handle before publishing readiness.
- Have the completion probe acquire-read readiness before reading or using the
  handle.
- Consider using a compact atomic state machine if both sides may race to schedule
  completion.

Example states:

```text
waiting_for_caller
caller_ready
completion_probe_posted
caller_posted
```

The exact state machine should ensure that exactly one side schedules the caller
continuation.

## Caveats

This is still a polling design. If the caller never awaits the invoker, the probe
can repost forever unless there is a cancellation, abandonment, or owner-destruction
protocol.

Repeated self-posting can also starve other work if the event loop is otherwise
busy. Consider adding a backoff, yielding through a timer, or switching to a
one-shot atomic handshake where the late side schedules completion without polling.

The cleaner alternative may be a lock-free two-sided handshake:

```text
await_suspend(caller):
    publish callerSchedHandle
    if callee already completed:
        schedule caller continuation

final_suspend(callee):
    if callerSchedHandle already published:
        schedule caller continuation
    else:
        mark callee completed before caller
```

That handshake has the same lifetime benefits and avoids repost polling, but needs
careful atomic state transitions.
