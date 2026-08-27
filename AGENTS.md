# Weasel working agreements

## Scope and behavior

- Implement the requested change without expanding its scope. Do not add speculative features, compatibility layers, or defensive machinery unless there is a concrete need.
- Preserve unrelated user changes in a dirty worktree. Do not revert, delete, or rewrite them.
- Treat direct user instructions as the source of truth. Use a prompt for one-off exceptions rather than weakening durable rules below.
- For a question-only request, investigate and answer; do not make code changes unless asked.

## C++ style

- Use C++20 and four spaces for every indentation level. Do not use tabs.
- Keep simple assignments and simple conditional assignments on one line when they fit comfortably.
- Do not vertically align a lambda beneath an opening parenthesis. Indent callback headers one normal level from their containing call, and lambda bodies one additional level.
- Align member names in related declaration blocks when it makes the declaration easier to scan. Apply the same treatment to multi-member `struct`s.
- Declare class sections in this order: private data members, private member functions, protected members, then public members. Preserve an earlier nested-type declaration only when it is required by a member or API below it.
- Use PascalCase for named non-member functions. Keep existing member-function naming consistent with the surrounding type.
- Avoid `[[nodiscard]]` on ordinary queries, getters, predicates, and results that callers may intentionally ignore. Reserve it for values whose loss hides a failure, ownership transfer, or other meaningful outcome.
- Validate at real external or mathematical boundaries. Do not add blanket `std::isfinite` checks or redundant defensive validation for values controlled by the editor and current project format.


- Run `git diff --check` after edits.
- Build the Debug Weasel target when changes affect C++ code. Prefer `cmake --build build\\cmake --config Debug --target Weasel --parallel 4`; use the Visual Studio project as an additional check when appropriate.
