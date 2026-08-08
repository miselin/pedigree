#!/usr/bin/env python3
"""Capture the source inputs consumed by verify.sh.

The verifier intentionally permits a prepared, dirty engineering tree. A
snapshot therefore describes that initial state rather than requiring HEAD.
It excludes generated build/cache outputs, so a source edit during
verification is still a failure.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


GENERATED_DIRECTORIES = (
    b".cache",
    b"build",
    b"build-verify",
    b"logs",
)
GENERATED_TOPLEVEL_PREFIXES = (
    b"build-",
    b"cmake-build-",
)
SOURCE_TOPLEVEL_DIRECTORIES = {
    b"build-etc",
}


def run_git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(("git", "-C", os.fspath(root), *args))


def is_excluded(path: bytes, extra_prefixes: tuple[bytes, ...]) -> bool:
    top_level = path.split(b"/", 1)[0]
    if top_level in GENERATED_DIRECTORIES or (
        top_level not in SOURCE_TOPLEVEL_DIRECTORIES
        and any(
            top_level.startswith(prefix)
            for prefix in GENERATED_TOPLEVEL_PREFIXES
        )
    ):
        return True
    for prefix in extra_prefixes:
        normalized = prefix.rstrip(b"/")
        if path == normalized or path.startswith(normalized + b"/"):
            return True
    return False


def normalize_extra_prefixes(root: Path, paths: list[str]) -> tuple[bytes, ...]:
    """Accept only in-tree generated roots that cannot hide tracked inputs."""
    root = root.resolve()
    prefixes: list[bytes] = []
    for raw_path in paths:
        if not raw_path:
            raise ValueError("--exclude-relative must not be empty")
        candidate = Path(raw_path)
        if candidate.is_absolute():
            raise ValueError(f"--exclude-relative must be relative: {raw_path}")

        resolved = (root / candidate).resolve()
        try:
            relative = resolved.relative_to(root)
        except ValueError as error:
            raise ValueError(
                f"--exclude-relative escapes the repository: {raw_path}"
            ) from error
        if relative == Path("."):
            raise ValueError("--exclude-relative must not be the repository root")

        relative_text = os.fspath(relative)
        top_level = os.fsencode(relative.parts[0])
        tracked = run_git(root, "ls-files", "-z", "--", relative_text)
        if tracked:
            raise ValueError(
                "--exclude-relative contains tracked verifier input: "
                f"{relative_text}"
            )
        if top_level in SOURCE_TOPLEVEL_DIRECTORIES or not (
            top_level in GENERATED_DIRECTORIES
            or any(top_level.startswith(prefix) for prefix in GENERATED_TOPLEVEL_PREFIXES)
        ):
            raise ValueError(
                "--exclude-relative must be under a generated top-level root: "
                f"{relative_text}"
            )
        prefixes.append(os.fsencode(relative_text).rstrip(b"/"))
    return tuple(prefixes)


def digest(label: bytes, payload: bytes) -> str:
    hasher = hashlib.sha256()
    hasher.update(label)
    hasher.update(b"\0")
    hasher.update(payload)
    return hasher.hexdigest()


def filtered_diff(root: Path, cached: bool, extra_prefixes: tuple[bytes, ...]) -> bytes:
    args = ["diff", "--binary", "--full-index"]
    if cached:
        args.append("--cached")
    args.extend(["HEAD", "--", "."])
    for prefix in GENERATED_DIRECTORIES + extra_prefixes:
        args.append(":(exclude)" + os.fsdecode(prefix.rstrip(b"/")) + "/**")
    # Generated build-* trees are untracked and therefore absent here.  Do
    # not use a broad tracked pathspec: build-etc is versioned verifier input.
    return run_git(root, *args)


def untracked_manifest(
    root: Path,
    extra_prefixes: tuple[bytes, ...],
) -> bytes:
    paths = run_git(root, "ls-files", "--others", "--exclude-standard", "-z")
    manifest = bytearray()
    for path in paths.split(b"\0"):
        if not path or is_excluded(path, extra_prefixes):
            continue
        filesystem_path = os.fsencode(root) + b"/" + path
        try:
            status = os.lstat(filesystem_path)
        except FileNotFoundError:
            # A concurrently deleted input is captured by the next boundary.
            manifest.extend(b"missing\0" + path + b"\0")
            continue
        manifest.extend(str(status.st_mode).encode("ascii") + b"\0" + path + b"\0")
        if os.path.islink(filesystem_path):
            target = os.readlink(filesystem_path)
            manifest.extend(
                b"link\0"
                + (target if isinstance(target, bytes) else os.fsencode(target))
                + b"\0"
            )
        elif os.path.isfile(filesystem_path):
            with open(filesystem_path, "rb") as source:
                manifest.extend(hashlib.sha256(source.read()).hexdigest().encode("ascii"))
            manifest.extend(b"\0")
        else:
            manifest.extend(b"other\0")
    return bytes(manifest)


def submodule_state(root: Path) -> bytes:
    # The foreach body returns absolute worktrees so recursive submodules can
    # be fingerprinted with their own untracked content rather than merely a
    # porcelain status bit.
    locations = run_git(
        root,
        "submodule",
        "foreach",
        "--quiet",
        "--recursive",
        "printf '%s\\0%s\\0' \"$displaypath\" \"$PWD\"",
    )
    fields = locations.split(b"\0")
    if fields and not fields[-1]:
        fields.pop()
    if len(fields) % 2:
        raise RuntimeError("submodule foreach returned an incomplete path pair")

    state = bytearray()
    pairs = sorted(zip(fields[::2], fields[1::2]))
    for display_path, worktree in pairs:
        module_root = Path(os.fsdecode(worktree))
        state.extend(b"submodule\0" + display_path + b"\0")
        state.extend(run_git(module_root, "rev-parse", "HEAD"))
        state.extend(run_git(module_root, "diff", "--binary", "--full-index", "HEAD"))
        state.extend(
            run_git(module_root, "diff", "--cached", "--binary", "--full-index", "HEAD")
        )
        state.extend(b"untracked\0")
        state.extend(untracked_manifest(module_root, ()))
    return bytes(state)


def snapshot(root: Path, extra_prefixes: tuple[bytes, ...]) -> dict[str, str]:
    return {
        "head": digest(b"head", run_git(root, "rev-parse", "HEAD")),
        "tracked-worktree": digest(
            b"tracked-worktree", filtered_diff(root, False, extra_prefixes)
        ),
        "tracked-index": digest(
            b"tracked-index", filtered_diff(root, True, extra_prefixes)
        ),
        "untracked-inputs": digest(
            b"untracked-inputs", untracked_manifest(root, extra_prefixes)
        ),
        "submodules": digest(b"submodules", submodule_state(root)),
    }


def write_snapshot(output: Path, values: dict[str, str]) -> None:
    output.write_text("".join(f"{key}={value}\n" for key, value in values.items()))


def read_snapshot(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text().splitlines():
        key, value = line.split("=", 1)
        values[key] = value
    return values


def changed_keys(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(key for key in set(before) | set(after) if before.get(key) != after.get(key))


def compare(expected: Path, actual: Path) -> int:
    changed = changed_keys(read_snapshot(expected), read_snapshot(actual))
    if changed:
        print("source provenance changed: " + ", ".join(changed), file=sys.stderr)
        return 1
    return 0


def watch(
    root: Path, baseline: Path, extra_prefixes: tuple[bytes, ...], interval: float
) -> int:
    expected = read_snapshot(baseline)
    while True:
        changed = changed_keys(expected, snapshot(root, extra_prefixes))
        if changed:
            print("source provenance changed: " + ", ".join(changed), file=sys.stderr)
            return 1
        time.sleep(interval)


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="pedigree-verify-provenance-") as temp:
        root = Path(temp)
        subprocess.check_call(("git", "init", "--quiet", root))
        subprocess.check_call(("git", "-C", root, "config", "user.email", "verify@example.invalid"))
        subprocess.check_call(("git", "-C", root, "config", "user.name", "verify"))
        (root / "source.txt").write_text("initial\n")
        (root / "build-etc").mkdir()
        (root / "build-etc" / "toolchain.cmake").write_text("initial\n")
        subprocess.check_call(
            ("git", "-C", root, "add", "source.txt", "build-etc/toolchain.cmake")
        )
        subprocess.check_call(("git", "-C", root, "commit", "--quiet", "-m", "initial"))

        try:
            if normalize_extra_prefixes(root, ["build-custom"]) != (b"build-custom",):
                raise ValueError("custom build root was not normalized")
            for forbidden in (
                "source.txt",
                "src/untracked-output",
                "build-etc",
                "../outside",
                ".",
            ):
                try:
                    normalize_extra_prefixes(root, [forbidden])
                except ValueError:
                    continue
                raise ValueError(f"unsafe exclusion was accepted: {forbidden}")
        except ValueError as error:
            print(f"self-test: {error}", file=sys.stderr)
            return 1

        (root / ".cache").mkdir()
        (root / ".cache" / "generated").write_text("one\n")
        baseline = snapshot(root, ())
        (root / ".cache" / "generated").write_text("two\n")
        (root / "build-hosted-check").mkdir()
        (root / "build-hosted-check" / "generated").write_text("three\n")
        if snapshot(root, ()) != baseline:
            print("self-test: excluded paths changed the snapshot", file=sys.stderr)
            return 1

        (root / "source.txt").write_text("changed\n")
        if snapshot(root, ()) == baseline:
            print("self-test: tracked source change was not detected", file=sys.stderr)
            return 1

        (root / "source.txt").write_text("initial\n")
        (root / "build-etc" / "toolchain.cmake").write_text("changed\n")
        if snapshot(root, ()) == baseline:
            print("self-test: tracked build-etc change was not detected", file=sys.stderr)
            return 1

        (root / "build-etc" / "toolchain.cmake").write_text("initial\n")
        watch_baseline = root / "watch-baseline.txt"
        write_snapshot(watch_baseline, snapshot(root, ()))
        watcher = subprocess.Popen(
            (
                sys.executable,
                os.fspath(Path(__file__).resolve()),
                "--watch",
                "--root",
                os.fspath(root),
                "--baseline",
                os.fspath(watch_baseline),
                "--interval",
                "0.01",
            ),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.05)
            (root / "source.txt").write_text("changed while watched\n")
            if watcher.wait(timeout=2) != 1:
                print("self-test: watcher did not reject source drift", file=sys.stderr)
                return 1
        except subprocess.TimeoutExpired:
            print("self-test: watcher did not observe source drift", file=sys.stderr)
            return 1
        finally:
            if watcher.poll() is None:
                watcher.terminate()
                watcher.wait()
        (root / "source.txt").write_text("initial\n")

        (root / "input.txt").write_text("untracked input\n")
        before_content_change = snapshot(root, ())
        (root / "input.txt").write_text("changed input\n")
        if snapshot(root, ()) == before_content_change:
            print("self-test: untracked input content was not detected", file=sys.stderr)
            return 1

        source_root = root / "submodule-sources"
        source_root.mkdir()
        nested_source = source_root / "nested-source"
        module_source = source_root / "module-source"
        for repository in (nested_source, module_source):
            subprocess.check_call(("git", "init", "--quiet", repository))
            subprocess.check_call(
                ("git", "-C", repository, "config", "user.email", "verify@example.invalid")
            )
            subprocess.check_call(("git", "-C", repository, "config", "user.name", "verify"))

        (nested_source / "nested.txt").write_text("initial\n")
        subprocess.check_call(("git", "-C", nested_source, "add", "nested.txt"))
        subprocess.check_call(("git", "-C", nested_source, "commit", "--quiet", "-m", "initial"))
        subprocess.check_call(
            (
                "git",
                "-C",
                module_source,
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "add",
                "--quiet",
                os.fspath(nested_source),
                "nested",
            )
        )
        subprocess.check_call(("git", "-C", module_source, "add", ".gitmodules", "nested"))
        subprocess.check_call(("git", "-C", module_source, "commit", "--quiet", "-m", "nested"))
        subprocess.check_call(
            (
                "git",
                "-C",
                root,
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "add",
                "--quiet",
                os.fspath(module_source),
                "module",
            )
        )
        subprocess.check_call(("git", "-C", root, "add", ".gitmodules", "module"))
        subprocess.check_call(("git", "-C", root, "commit", "--quiet", "-m", "module"))
        subprocess.check_call(
            (
                "git",
                "-C",
                root,
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "update",
                "--init",
                "--recursive",
            ),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        nested_untracked = root / "module" / "nested" / "untracked-input"
        nested_untracked.write_text("one\n")
        before_submodule_content_change = snapshot(root, ())
        nested_untracked.write_text("two\n")
        if snapshot(root, ()) == before_submodule_content_change:
            print(
                "self-test: recursive submodule untracked content was not detected",
                file=sys.stderr,
            )
            return 1
    print("verify provenance self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare", nargs=2, type=Path, metavar=("EXPECTED", "ACTUAL"))
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--validate-exclusions", action="store_true")
    parser.add_argument("--exclude-relative", action="append", default=[])
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()

    if arguments.self_test:
        return self_test()
    if arguments.compare:
        return compare(*arguments.compare)
    if arguments.root is None:
        parser.error("--root is required unless --self-test or --compare is used")

    root = arguments.root.resolve()
    try:
        prefixes = normalize_extra_prefixes(root, arguments.exclude_relative)
    except ValueError as error:
        parser.error(str(error))
    if arguments.validate_exclusions:
        return 0
    if arguments.watch:
        if arguments.baseline is None:
            parser.error("--baseline is required with --watch")
        if arguments.interval <= 0:
            parser.error("--interval must be positive")
        return watch(root, arguments.baseline, prefixes, arguments.interval)
    if arguments.output is None:
        parser.error("--output is required unless --self-test, --compare, or --watch is used")
    write_snapshot(arguments.output, snapshot(root, prefixes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
