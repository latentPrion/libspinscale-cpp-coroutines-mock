# Project Instructions

- **Build out-of-tree only.** Never configure or build inside the repository root or `src/` in a way that writes objects or binaries next to sources. Always use a dedicated directory (for example `build/`: `mkdir -p build && cd build && cmake .. && cmake --build . -j"$(nproc)"`). Binaries appear under `build/src/` (see [README.md](README.md)).
- Always break functions into logical subfunctions. No long-scrolling functions, in any language. This applies to source code, scripts, build scripts, CMake, Makefiles, and similar project files. Preserve this subfunction splitting discipline during refactors.
- Modularity is non-negotiable. Always group logically related functions together into a module. Preserve modularity during refactors.
- Reuse or extend existing abstractions instead of duplicating logic wherever possible. Don't repeat yourself. The goal here is to prevent duplication. Not to discourage appropriate logical separation of prior abstractions into new logical abstractions where sensible.
- Always isolate configurable behaviour into configuration variables appropriate for the language and framework being used.
- Never bake in literals; at minimum, declare them at the top of the file with a semantically meaningful name.
- Don't add unnecessary getters and setters in classes. Just access member vars directly.
- Don't use hungarian notation in var names. Class member vars should not have prefixed or postfixed underscores. Instead do the inverse: use prefixed or postfixed underscores for auto-scoped var names when they shadow static or member var names.
- UI should be responsive. Always prefer to use pre-packaged UI toolkit widgets, containers and colour sets harmoniously, instead of writing custom CSS overrides. Write custom CSS only if there's no UI toolkit mechanism available.
- Aggressively isolate, split off, deduplicate and reuse code which can be made into common library code. Do the same with UI elements. Do this both when implementing new features and opportunistically while refactoring or changing old code/UI elements.
- Names of files, functions, classes, abstractions, database fields, etc should be aimed at disambiguating purpose and function, rather than at brevity.
