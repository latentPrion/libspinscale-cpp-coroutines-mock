# Coroutines Migration Notes

This repository exists to support a deliberate migration of the `smo` codebase (referenced at `docs/3rdParty/smo`) toward native C++ coroutines.

The goal is not to "just use async/await style syntax." The goal is to preserve the execution and responsiveness model that currently comes from continuation-passing style (CPS) infrastructure in `libspinscale`.

## Why This Work Exists

The existing system is performance-sensitive and built around explicit scheduling behavior:

- Continuations are suspended/resumed without putting worker threads to sleep.
- Threads avoid blocking/sleeping unless there is no runnable work.
- Synchronization includes continuation-aware primitives (for example, `Qutex`) and user-space spinlocks.
- Throughput and latency are protected by aggressively controlling where work runs (including OpenCL offload in paths such as `PcloudStimulusProducer`).

Those guarantees are central to correctness and user-visible responsiveness. Because of that, this migration cannot start with assumptions that a general coroutine framework will preserve those properties by default.

## Why Not Blindly Adopt `boost::asio`

Libraries like `boost::asio` can be excellent, but this project has unusual constraints:

- Scheduler semantics matter as much as language syntax.
- "Coroutine support" is not enough if suspension points block pools or alter queue fairness.
- Existing CPS behavior is intentionally eccentric to maximize throughput under load.

So this effort starts by understanding C++ coroutine operators and language mechanisms directly (`co_await`, `co_return`, `co_yield`, promise types, awaiters, suspension points, and resumption control) before deciding how much external runtime should be used.

## Migration Intent

The intended outcome is coroutine-based code that still behaves like the current CPS design where it matters:

- Suspend/resume continuations, not entire worker execution pipelines.
- Keep threads active when work is available.
- Preserve high-throughput scheduling and responsiveness characteristics.
- Introduce coroutine abstractions that can interoperate with existing project-specific execution models.

## Scope Guardrail

- Do not modify files inside the referenced `smo` repository from this repo.
- Use this repo to document, prototype, and reason about migration mechanics first.

## Repository Layout

This repo now keeps the reusable coroutine runtime and its GoogleTest coverage in
the `libspinscale` submodule. The old marionette executable test app has been
ported into `libspinscale/tests` and removed from the root repository.

```
coroutines/
  libspinscale/            # Submodule: coroutine + CPS runtime
```

The ported tests include private harness primitives under
`libspinscale/tests/support/` for exercising posting promises across distinct
OS threads. Those tests cover the former `Group`, viral non-posting, timer, and
component-continuation scenarios in a reusable GoogleTest form.

## Viral vs Non-Viral Invokers

- **Non-viral posting invokers** are coroutine entry points that are not
  `co_await`ed. The caller supplies exception storage and a completion lambda,
  and the invoker object must stay alive until completion.
- **Viral posting invokers** are awaitable coroutine results. A caller
  `co_await`s them, and posting promises move callee execution to the target
  component thread before posting resume back to the caller thread.
- **Viral non-posting invokers** run on the caller thread and use direct
  symmetric-transfer-style resumption rather than cross-thread posting.

## Coro completion contract

Non-viral invokers wire the caller's completion lambda at construction. On `co_return`, `final_suspend` invokes that lambda automatically (after rethrowing if `exceptionPtr` is set). **Never call the completion lambda manually inside the coro body.**

`PuppetApplication` batch ops (`joltAllPuppetThreadsCReq`, etc.) are
`ViralNonPostingInvoker` member coroutines on the caller thread.

**Parameter order:** `*CReq` entry points take `(std::exception_ptr&, std::function<void()> callback)` only; context via `this`.

## Build (CMake + C++23)

The [`libspinscale`](libspinscale) git submodule is built as part of the tree.

### Prerequisites

- `cmake` (3.16+)
- A C++ compiler with C++23 coroutine support
- Boost (`libboost-dev` / distro equivalent): `system` and `log` for `libspinscale`

### Submodule and Build (Out-of-Tree)

```bash
git submodule update --init --recursive
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j"$(nproc)"
```

Optional: `-DENABLE_LTO=ON` for link-time optimization.

### Run Tests

Enable the libspinscale GoogleTest targets and run them with CTest:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DLIBSPINSCALE_BUILD_TESTS=ON
cmake --build . -j"$(nproc)"
ctest --output-on-failure
```

### libspinscale puppet lifetime ops

`PuppetApplication::*CReq` are public member coroutines using `co::Group` over the existing `*ThreadAReq` bridge.
