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

## Application layout (salmanoff paradigm)

This repo now mirrors salmanoff's bootstrap layering, adapted to a test app with four
named component threads (MRNTT, body, world, leg):

```
coroutines/
  main.cpp                 # CRT JOLT + join marionette
  ctestcore/               # Marionette + harness + component threads
  tests/                   # Theory / integration executables
  libspinscale/            # Submodule: coroutine + CPS runtime
```

**Named component threads**

| Thread | Role |
|------|------|
| MRNTT | Puppeteer; runs tests, orchestrates harness lifecycle, SIGINT shutdown |
| BODY | Primary worker component thread |
| WORLD | Secondary worker component thread (cross-thread viral calls in sync-main demo) |
| LEG | Tertiary worker component thread |

**Bootstrap sequence:** CRT JOLT → marionette `preLoopHook` holds non-viral `initializeCReq` → harness JOLT/start → parallel body/world/leg init via `co::Group` → test body → parallel finalize → jolt/exit puppet threads.

Reference: salmanoff (`docs/3rdParty/smo`) is the production idiom; this repo is the coroutine staging app.

## Per-component posting invokers

Each component header owns its thread tag and invoker typedefs (no shared `threadTags.h`):

| Header | Tag | Typedefs |
|--------|-----|----------|
| `marionette/marionette.h` | `MrnttThreadTag` | `MrnttNonViralPostingInvoker`, `MrnttViralPostingInvoker<T>` |
| `body/body.h` | `BodyThreadTag` | `BodyNonViralPostingInvoker`, `BodyViralPostingInvoker<T>` |
| `world/world.h` | `WorldThreadTag` | same pattern |
| `leg/leg.h` | `LegThreadTag` | same pattern |

## Viral vs non-viral lifetime boundary

- **Non-viral** (`MrnttNonViralPostingInvoker`): top-level hook callers (`preLoopHook`, shutdown paths). Hooks call `holdInitializeCReq` / `holdFinalizeCReq`, which store the invoker and pass a completion lambda that drives program state (run test, exit loop, shutdown).
- **Viral** (`MrnttViralPostingInvoker<void>`, `BodyViralPostingInvoker<void>`, …): orchestrator-facing `initializeCReq` / `finalizeCReq` on `TestHarness` and named component threads (body, world, leg). Invoked via `co_await` from parent coroutines — not from hooks directly.
- **Harness init:** `TestHarness::initializeCReq` jolt/start puppet threads, then sequentially `co_await`s body/world/leg viral `initializeCReq`.

## Coro completion contract

Non-viral invokers wire the caller's completion lambda at construction. On `co_return`, `final_suspend` invokes that lambda automatically (after rethrowing if `exceptionPtr` is set). **Never call the completion lambda manually inside the coro body.**

`PuppetApplication` batch ops (`joltAllPuppetThreadsCReq`, etc.) are `ViralNonPostingInvoker` member coroutines on the caller thread (symmetric transfer); `TestHarness::initializeCReq` co_awaits them directly.

**Parameter order:** `*CReq` entry points take `(std::exception_ptr&, std::function<void()> callback)` only; context via `this`.

## Build (CMake + C++23)

The [`libspinscale`](libspinscale) git submodule is built as part of the tree.

### Prerequisites

- `cmake` (3.16+)
- A C++ compiler with C++23 coroutine support
- Boost (`libboost-dev` / distro equivalent): `system` for executables; `log` when building `libspinscale`

### Submodule and Build (Out-of-Tree)

```bash
git submodule update --init --recursive
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j"$(nproc)"
```

Optional: `-DENABLE_LTO=ON` for link-time optimization.

### Run Binaries

Binaries appear under `build/ctestcore/`:

```bash
./ctestcore/ctest-sync-main
./ctestcore/ctest-group-edge-test
./ctestcore/ctest-group-timer-test
./ctestcore/ctest-group-smoke
```

### libspinscale puppet lifetime ops

`PuppetApplication::*CReq` are public member coroutines using `co::Group<PuppetThread::ViralThreadLifetimeMgmtInvoker>` over the existing `*ThreadAReq` bridge.
