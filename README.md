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

## Build (CMake + C++23)

This repository uses CMake. The [`libspinscale`](libspinscale) git submodule is included and built as part of the tree so this repo can serve as a staging area for the joint coroutines + spinscale migration; coroutine theory executables in `src/` still use the local prototype headers under [`src/`](src/) until that migration lands in both repositories.

### Prerequisites

- `cmake` (3.16+)
- A C++ compiler with C++23 coroutine support
- Boost (`libboost-dev` / distro equivalent): `system` for the `src/` executables; `log` as well when building the `libspinscale` submodule

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

```bash
./src/group-edge-test
./src/group-timer-test
./src/sync-main-drives-continuation
```

### Add New Theory Executables

1. Add a new source file under `src/`.
2. Register a new target in [`src/CMakeLists.txt`](src/CMakeLists.txt) via `add_coroutines_executable(...)`.
3. Rebuild from your build tree:

```bash
cmake --build . -j"$(nproc)"
```
