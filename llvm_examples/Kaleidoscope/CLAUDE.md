# Kaleidoscope Chapter directories

`Chapter2`–`Chapter9` refactor the LLVM Kaleidoscope tutorial chapters
(https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/) from the single-file
upstream `toy.cpp` into a `src/` + `include/` + `test/` layout with classes in
`namespace toy`. Directory N corresponds to tutorial chapter N
(`LangImpl0N.html`); each chapter builds on the previous one. `ChapterA` is
a separate MLIR-toy-style redesign with its own conventions — nothing in this
file applies to it.

`Kaleidoscope/README.md` (top level) is the reader-facing home for everything
shared: the general compiler intro ("The shape of a compiler": three phases +
chapter roadmap), directory map, the two tracks, the common class mapping,
language gotchas, build/run, and the full two-scheme testing + lit/`lit.cfg`
exposition. Chapter READMEs must NOT duplicate that material — they link to
`../README.md#testing-the-two-schemes` (and `#build-and-run`) and cover only
what their chapter adds. When common facts change, update the top README, not
nine chapter READMEs.

Policy: Chapter2–9 stay **faithful to upstream** — upstream quirks are kept
deliberately (e.g. `getchar()`-based lexer, `operator[]` symbol lookups).
Deviations are rare and intentional; when you find code that differs from the
tutorial snippet, treat it as deliberate and document it, don't "fix" it.
Known deviations/gotchas: the lexer validates a leading `.` in numbers
(`.5` → 0.5, lone `.` is an ASCII token); identifiers do NOT allow `_`;
no scientific notation; `getTokPrecedence()` uses `map::find` instead of
upstream's `operator[]` (same results, no default-insert side effect); the
first-token priming lives inside `mainLoop()` rather than `main()`, so no
`ready>` prompt appears before the first line of input; Chapter8
deliberately has no JIT.

## Task: write README.md for each Chapter directory

`Chapter2/README.md` is the reviewed reference — match its structure, depth,
and voice. For each `ChapterN`, use that chapter's tutorial doc as the
reference and base ALL diagrams and code snippets on the **refactored code in
that directory**, not on the upstream globals. Since chapters are
incremental, focus each README on what the chapter ADDS; link back to earlier
Chapter READMEs instead of re-explaining (e.g. don't re-explain the lexer in
Chapter5).

Required shape (learned from review of Chapter2):

1. **Title**: `# Chapter N — <topic>`, matching the directory name (the one
   exception: dir `Chapter2` covers two tutorial chapters and titles itself
   `# Chapter 1 & 2 — ...`). In `##` headings, the phrase after a colon
   starts with an uppercase letter (`## Chapter 1: The Lexer`,
   `## File-by-file: What changed from Chapter7`); `###` subsections are
   plain sentence case.
2. **Intro, one continuous flow under the title — no "Overview"/"Overall
   structure" heading.** Order: general concepts the chapter introduces
   (define terms like a textbook would) → a small generic ASCII figure →
   links to the chapter doc(s) → brief language/feature recap (a paragraph,
   not a section) → concrete pipeline/architecture diagram drawn from the
   refactored classes → design notes → file-mapping table (`| File |
   Contents — and its tutorial counterpart |`) stating per file which
   upstream globals/free functions became which class members/methods.
   For Chapter3+ the table becomes a "File-by-file: what changed from
   ChapterN-1" section with three groups — new files / same filename but
   byte-identical / same filename but modified — where every modified file
   gets a before → after snippet (side-by-side comments work well). Establish
   the groups by actually running `diff` against the previous dir, never by
   eyeballing. This section is reference material: place it near the END of
   the README, right before "Build and run", and leave a one-line pointer to
   it at the end of the intro.
3. **Chapter sections with code**: split big chapters into numbered
   subsections under a short chapter-level intro (Chapter2 has
   "## Chapter 2: The Parser and AST" introducing the approach, then
   "## 2.1 The Abstract Syntax Tree" and "## 2.2 The Parser"). Explain the
   code tutorial-style: real snippets from `src/`/`include/` followed by
   prose on WHY, plus walkthrough traces for tricky algorithms (e.g. the
   `parseBinOpRHS` trace of `a + b * c`). Block granularity: **one code
   block per function, each followed by full explanatory prose** (see
   Chapter2's parser subsections) — do NOT merge several functions into one
   marker-annotated block; the keyed-notes format compresses explanations
   too much. The single annotated block with `// [1]` markers is reserved
   for walking through the branches of ONE function (see Chapter2's
   `gettok()`), where a region covered in full elsewhere may be elided with
   `...` and a pointer comment. Give every deliberate deviation
   from upstream its own subsection: upstream snippet vs refactored snippet,
   flow chart, step-by-step rationale, behavior-comparison table.
4. **Diagrams**: ASCII boxes/arrows in fenced code blocks (no mermaid).
   Pipeline/data-flow, class hierarchies, call graphs, example AST trees.
5. **Code-block captions**: every fenced block quoting repo code gets a
   caption line directly above the fence: `` **`ChapterN/src/file.cpp`** ``
   (path always starts with `ChapterN/`). Upstream tutorial snippets are
   instead marked with a `// upstream (LangImplNN)` comment inside the block.
   Diagrams, grammar blocks, shell commands, and session transcripts get no
   caption.
6. **Build and run**: `./build.sh` or manual cmake+Ninja, plus an example
   session (`./build/toy < cmd.txt`) with expected output. Generic build
   facts (requirements, deployment target, piped-`ready>` note) live in the
   top README — link, don't repeat.
7. **Tests**: what each layer pins down HERE, plus commands. The generic
   two-scheme exposition and the lit/`lit.cfg` mechanics live in the top
   README's "Testing: the two schemes" section — open with one sentence and
   a link to `../README.md#testing-the-two-schemes`, then go straight to the
   chapter-specific coverage. Carry `.k` suites forward from the previous
   chapter while the behavior they pin still exists; document hand-offs (as
   Chapter8 does for `jit.k` → `errors.k`).

## Build & test infrastructure (same in every Chapter dir)

- Build: `cmake -B build -G Ninja -S . && cmake --build build` (or
  `./build.sh`, which also pipes `cmd.txt` through the binary). LLVM via
  Homebrew, linked per-chapter via `llvm_map_components_to_libnames`.
- gtest unit tests fetched via FetchContent; binaries like `./build/lexer_test`.
- lit/FileCheck: `test/filecheck/*.k` with `RUN:` lines; ctest registers a
  `filecheck` test that sets `TOY_BIN=$<TARGET_FILE:toy>` and
  `LIT_EXEC_ROOT=<build>/lit`. Run one test by hand with
  `lit -v test/filecheck/<file>.k` (needs `lit` on PATH; FileCheck comes from
  `llvm-config --bindir` via `lit.cfg`). Hand-runs create
  `test/filecheck/Output/` — already gitignored at the repo root.
- Run all: `ctest --test-dir build`.
