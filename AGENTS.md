# Pedigree Agent Guide

## Repository scope

- The maintained compiler is the OCaml implementation under `src/`. Treat it as the default target for compiler work.
- `legacy/` contains the deprecated C compiler. Change or validate it only when the task explicitly concerns the legacy implementation or compatibility with it.
- `src-new/` is the experimental self-hosted compiler. Do not assume changes there are part of the mainline implementation.
- Keep patches focused. Preserve unrelated working-tree changes and do not commit generated build output.

## Source layout

The project is organized as follows:

- `docs`: documentation and design for kernel components
- `external`: third-party submodules for external dependencies
- `images`: local files and scripts for the disk images built in the build process
- `scripts`: miscellaneous scripts for development
- `src/buildutil`: utilities for the build that run on the build host, including a testsuite
- `src/modules`: kernel modules including drivers, subsystem layers, and system modules
- `src/po`: translations for a subset of our userspace programs
- `src/system`: the kernel proper
- `src/user`: userspace applications and libraries to bundle with the kernel

## Building

- `uv` should be used for the Python scripts in this repo to maintain a virtualenv
- Run `./easy_build_x64.sh` to build the amd64 kernel and userspace, including disk images.
  - After a successful run, artifacts will be in the `build/` directory and a simple `cmake --build build` will re-run the build
- Run `./verify.sh` to build a version of the kernel that runs on the host, for certain types of testing

## Code Style

`.clang-format` defines a style for the kernel that must be followed. Format only changed lines, do not format entire files, if not already formatted.

`ruff` can be used to format Python code.

## Tests and verification

A testsuite exists for kernel utilities; the testsuite runs on the host system (Linux/Mac/Windows) as a build utility. When changing or adding
utilities such as `Tree` or `String`, tests should be used to verify zero regressions. Add tests sparingly but as needed to minimize future regressions.

For interactive QEMU tests, prefer `scripts/qemu --serial`. It disables the
graphical display and connects the guest's COM1-backed `/dev/ttyS0` terminal to
the invoking shell. Use `scripts/run-qemu-iso.py` for bounded marker-based
checkpoints; it is also display-free and captures the guest serial log. Build a
fresh ISO before treating serial output as evidence.

## Running engineering spikes

- Define a bounded outcome, the affected subsystem, and acceptance checks before
  editing. Park newly discovered work outside that scope unless it blocks the
  repair. Finish the selected slice before widening the audit.
- Record the starting commit, working-tree diff, and untracked-file inventory.
  Save baseline contents of files with unrelated edits before sharing ownership;
  a filename list alone is insufficient for separating mixed hunks later.
- Delegate independent, bounded tasks with one writer per overlapping area.
  Give agents only the relevant context, file ownership, constraints, and expected
  result. Prefer short handoffs containing findings, changed symbols, verification,
  and blockers over full-history forks, repeated inventories, or duplicate reviews.
- Assign one integration owner per shared checkout/build directory, including
  across tasks using it. That owner runs builds and QEMU; other agents finish
  edits and review. Settle shared interfaces before the final build. If a header
  changed during compilation, explicitly rebuild its consumers before verification.
- Use affected compile targets, focused native tests, and routing checks for fast
  feedback. Reuse build directories and profile unexpected build delays before
  changing the build system. Run broader integration checks once the patch is
  stable; repeat them when changes, failures, or unresolved concerns justify it.
- Prefer the headless serial workflow above. For guest contract suites, capture
  per-suite exit status and an end marker, enforce a timeout, and stop on terminal
  failure. Use a fresh ISO and disposable HDD snapshots. Never rebuild or replace
  backing images while a guest uses them. Validate concurrency changes with one
  and four CPUs; parallel guests need independent writable state.
- For persistence checks, verify `PEDIGREE_CRIPPLE_HDD=FALSE` in the test build;
  the default disables runtime disk writes. Use only disposable disks with that
  configuration, keep the test ISO separate, and restore the shared build's
  original setting afterwards. A passing cache-only test does not prove disk
  writeback.
- Keep full logs in a task-specific artifact directory outside tracked source.
  Return summaries and paths, inspect targeted failure excerpts, and batch
  independent reads. Use bounded waits instead of repeatedly polling unchanged
  logs. Preserve failed runs: a green retry does not explain an intermittent fault.
- Test public behavior and meaningful failure paths. Distinguish native execution,
  routing checks, hosted compile-only checks, and guest execution; record skips and
  unavailable coverage explicitly. End with a compact handoff of changes, exact
  verification commands/results, artifact paths, remaining risks, and the next
  action. When committing, include new files and only owned hunks; compile split
  source/header combinations when partial staging could hide a dependency.
- Finish each completed pass with a focused local commit after verification.
  Preserve unrelated edits and keep pushes separate from these checkpoints.

## Pull requests

- Keep each PR centered on one coherent change. The description should summarize behavior, tests run, and any known limitations or deferred work.
- When creating or updating a PR, apply exactly one provenance label:
  - `ai-generated` when the majority of the PR's code or content was generated by AI.
  - `human-generated` when the majority was written by a human, even if AI tools assisted.
- Also apply at least one change-category label:
  - `bug` for correcting incorrect behavior.
  - `documentation` for documentation-only or documentation-led changes.
  - `enhancement` for new capabilities or intentional behavior improvements.
- Use multiple category labels only when the PR genuinely spans those categories. Update labels if the scope changes during review.
- Apply labels as part of opening the PR and verify the final PR state. If permissions or tooling prevent labeling, state exactly which labels are needed in the handoff rather than silently omitting them.
- Do not add workflow labels such as `good first issue`, `help wanted`, `duplicate`, `invalid`, `question`, or `wontfix` to a PR unless the task explicitly calls for them.
