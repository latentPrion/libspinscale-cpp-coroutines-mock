# Parameterized Posting Promise

## Summary

This design document compares viable approaches for parameterizing coroutine posting
promises by target `boost::asio::io_context`, so we can define promise families
such as `BodyPostingPromise`, `WorldPostingPromise`, and `LegPostingPromise`
without duplicating scheduling behavior in `get_return_object()` / `initial_suspend()`
paths.

**Implemented baseline:** compile-time promise aliases bound to global
`io_context` references through thread **tag types** that expose a static
`io_context()` hook. No separate resolver type.

## Current State Recap

Today the coroutine plumbing is split as follows:

- `PostingPromise<T>` owns shared behavior and state:
  - return value / exception storage
  - caller resumption and final post-back logic
  - chain-link behavior for deadlock detection and lock tracking
- `TaggedPostingPromise<T, ThreadTag>` extends `PostingPromise<T>` and adds
  thread-target scheduling:
  - initializes `selfSchedHandle`
  - posts that handle to `ThreadTag::io_context()` in `initial_suspend()`
- `PostingPromiseReturnOps<PromiseType, T>` supplies `return_value` /
  `return_void` only; it is parameterized by the concrete promise type and `T`.
  For tagged promises, `PromiseType` is `TaggedPostingPromise<T, ThreadTag>`,
  so the thread tag is already part of the promise type and is **not** repeated
  as a separate template parameter on the return-ops mixin.

Named aliases (`BodyPostingPromise<T>`, etc.) map to
`TaggedPostingPromise<T, BodyThreadTag>` and similar.

## Key Constraint

`initial_suspend()` has no parameters. That means a dynamic target `io_context`
cannot be passed directly into it.

Runtime target selection therefore must come from promise state established
before `initial_suspend()` runs (for example via constructor-fed state,
tag static hooks, or wrapper policy).

## Approach 1: Compile-Time Tag + Static `io_context()` Hook (Implemented)

### Idea

Create one reusable posting promise template that depends on a thread tag type.
Each tag type defines `static boost::asio::io_context &io_context() noexcept`
returning the correct global context. Then define named aliases for each
thread-target promise.

### Sketch

```cpp
// tags (each carries the mapping to its context)
extern boost::asio::io_context bodyIoContext;
extern boost::asio::io_context worldIoContext;
extern boost::asio::io_context legIoContext;

struct BodyThreadTag {
    static boost::asio::io_context &io_context() noexcept { return bodyIoContext; }
};
struct WorldThreadTag {
    static boost::asio::io_context &io_context() noexcept { return worldIoContext; }
};
struct LegThreadTag {
    static boost::asio::io_context &io_context() noexcept { return legIoContext; }
};

// reusable promise
template <typename T, typename ThreadTag>
struct TaggedPostingPromise
: PostingPromise<T>,
  PostingPromiseReturnOps<TaggedPostingPromise<T, ThreadTag>, T>
{
    TaggedPostingPromise() noexcept : PostingPromise<T>() { initializeSelfHandle(); }

    TaggedPostingPromise(
        std::exception_ptr &exceptionPtr,
        std::function<void(CalleeCoroutineHandleDestroyer)> callerLambda) noexcept
    : PostingPromise<T>(exceptionPtr, std::move(callerLambda))
    {
        initializeSelfHandle();
    }

    std::suspend_always initial_suspend() noexcept
    {
        boost::asio::post(ThreadTag::io_context(), this->selfSchedHandle);
        return {};
    }

private:
    void initializeSelfHandle() noexcept
    {
        this->setSelfSchedHandle(
            std::coroutine_handle<TaggedPostingPromise<T, ThreadTag>>::from_promise(*this));
    }
};

// aliases
template <typename T>
using BodyPostingPromise = TaggedPostingPromise<T, BodyThreadTag>;
template <typename T>
using WorldPostingPromise = TaggedPostingPromise<T, WorldThreadTag>;
template <typename T>
using LegPostingPromise = TaggedPostingPromise<T, LegThreadTag>;
```

### Pros

- Minimum call-site disruption.
- No per-call target argument needed.
- Mapping lives on the tag type; no second resolver template.
- Eliminates duplicated scheduling boilerplate across thread-specific promise
  types.
- Works naturally with existing `PostingInvoker<PromiseType, T>` patterns.

### Cons

- Adding a new target requires a new tag type (with `io_context()`), global
  context declaration/definition, and an alias.
- Target remains static per promise type.

## Approach 2: Runtime io_context Injection

### Idea

Use one generic runtime-configurable posting promise, with target context stored
in promise state and provided through coroutine function argument plumbing.

### Sketch

```cpp
template <typename T>
struct RuntimePostingPromise : PostingPromise<T>
{
    RuntimePostingPromise(
        boost::asio::io_context &targetContext,
        std::exception_ptr &exceptionPtr,
        std::function<void(CalleeCoroutineHandleDestroyer)> callerLambda) noexcept
    : PostingPromise<T>(exceptionPtr, std::move(callerLambda)),
      initialTargetContext(targetContext)
    {
        initializeSelfHandle();
    }

    std::suspend_always initial_suspend() noexcept
    {
        boost::asio::post(initialTargetContext, this->selfSchedHandle);
        return {};
    }

    boost::asio::io_context &initialTargetContext;
};


### Pros

- Maximum flexibility: same promise type can target different contexts per call.
- Useful when target thread is a runtime decision.

### Cons

- Higher complexity in coroutine signatures and promise constructor matching.
- Harder ergonomics for common fixed-target workflows.
- More opportunities for incorrect context wiring at call sites.

## Approach 3: Hybrid (Compile-Time Default + Optional Runtime Override)

### Idea

Combine approach 1 defaults with optional runtime override support. If override
is absent, use `ThreadTag::io_context()`. If provided, use explicit runtime target.

### Sketch

```cpp
template <typename T, typename ThreadTag>
struct HybridPostingPromise : PostingPromise<T>
{
    boost::asio::io_context *runtimeOverride = nullptr;

    std::suspend_always initial_suspend() noexcept
    {
        boost::asio::io_context &target =
            runtimeOverride ? *runtimeOverride : ThreadTag::io_context();
        boost::asio::post(target, this->selfSchedHandle);
        return {};
    }
};
```

### Pros

- Strong defaults with escape hatch.
- Enables incremental adoption of runtime override behavior.

### Cons

- More branching and constructor/API surface area.
- More testing burden and more edge cases than pure compile-time mapping.

## Tradeoff Matrix

| Criterion | Approach 1: Tag + static hook | Approach 2: Runtime Injection | Approach 3: Hybrid |
|---|---|---|---|
| Complexity | Low | Medium-High | High |
| Boilerplate Reduction | High | High | High |
| Type Safety for Target | High (compile-time binding) | Medium (runtime wiring) | Medium-High |
| Call-Site Ergonomics | High for fixed targets | Lower (extra args/plumbing) | Medium |
| Extensibility | High (new tag + hook) | High (runtime-driven) | Highest |
| Misconfiguration Risk | Low | Medium-High | Medium |

## Recommended Architecture (Decision-Complete)

Approach 1 is implemented.

### 1. Reusable base promise template with scheduling-policy abstraction

`TaggedPostingPromise<T, ThreadTag>`:

- inherits existing `PostingPromise<T>` for shared return/final behavior
- inherits `PostingPromiseReturnOps<TaggedPostingPromise<T, ThreadTag>, T>` for
  return-value / `return_void` only (`ThreadTag` is not a separate parameter on
  that mixin; it is already part of `PromiseType`)
- keeps handle initialization (`setSelfSchedHandle`) in one place
- calls `ThreadTag::io_context()` in `initial_suspend()`

### 2. Tag types with static `io_context()` backed by global refs

- `BodyThreadTag`, `WorldThreadTag`, `LegThreadTag`
- each `::io_context()` returns the matching external `boost::asio::io_context`

### 3. Promise aliases for named targets

```cpp
template <typename T>
using BodyPostingPromise = TaggedPostingPromise<T, BodyThreadTag>;

template <typename T>
using WorldPostingPromise = TaggedPostingPromise<T, WorldThreadTag>;

template <typename T>
using LegPostingPromise = TaggedPostingPromise<T, LegThreadTag>;
```

### 4. Preserve invoker-facing contracts

No behavioral/API change for invoker integration:

- `PostingInvoker<PromiseType, T>` remains the integration boundary.
- Existing promise-based coroutine return-object patterns remain valid.
- Caller-thread post-back in `final_suspend()` remains in `PostingPromise<T>`.

## Migration Guidance

### Replace subclass boilerplate with alias-based promises

For each thread-specific promise type that currently duplicates scheduling code,
replace subclass implementation with alias to the reusable tagged promise.

### Update coroutine invokers to use new aliases

Where an invoker currently binds to explicit promise subclasses, keep the same
`promise_type` pattern but swap base type to the corresponding alias.

Example direction:

```cpp
struct promise_type : BodyPostingPromise<void> { ... };
// and similarly for WorldPostingPromise<T>, LegPostingPromise<T>
```

This avoids duplicate `initial_suspend()` and handle setup in each new
thread-target promise family.

### Add new thread targets in one place

To introduce another thread-target promise:

1. add tag type with `static io_context &io_context() noexcept`
2. add `extern boost::asio::io_context ...` declaration (and definition in one TU)
3. add `using ...PostingPromise = TaggedPostingPromise<..., YourTag>` alias

No additional scheduling boilerplate should be required.

## Public Interface (Implemented)

- Tag types with static hook:
  - `BodyThreadTag`, `WorldThreadTag`, `LegThreadTag` — each `::io_context()`
- Generic aliasable promise template:
  - `TaggedPostingPromise<T, ThreadTag>`
- Return-op mixin (unchanged shape):
  - `PostingPromiseReturnOps<PromiseType, T>` — use
    `PromiseType = TaggedPostingPromise<T, ThreadTag>`; do not add a redundant
    `ThreadTag` template parameter to the mixin
- Optional future extension (not part of baseline implementation):
  - constructor path to override target `io_context` at runtime

## Test Plan for Implementation Phase

1. Body equivalence test
- Verify `BodyPostingPromise` still starts coroutine execution on `bodyIoContext`.
- Verify continuation posts back to caller context as before.

2. Multi-target scheduling parity
- Add `WorldPostingPromise` and `LegPostingPromise` and verify they mirror
  behavior using their contexts.

3. Nested viral chain correctness
- In nested `co_await` chains spanning mixed promise aliases, verify:
  - caller suspension/resumption ordering
  - return-value propagation
  - exception propagation via existing mechanisms

4. Non-viral completion lambda path
- Verify non-viral coroutine completion lambda still runs on caller context.
- Verify coroutine frame destruction ownership remains correct.

5. CoQutex behavior regression checks
- Verify deadlock detection on caller chain still works.
- Verify waiting coroutine queue wake-up context remains correct.

## Assumptions and Defaults

- Compile-time aliasing with global context references is implemented in
  `src/sync_main_drives_continuation.cpp` (per-thread tags, posting-promise
  aliases, and invoker aliases) and `src/promises.h` / `src/invokers.h`.
- Existing modularity constraints apply:
  - no duplicated scheduling logic
  - configurable thread-target behavior isolated in named abstractions
  - avoid hard-coded unnamed literals in implementation
