#!/usr/bin/env python3
"""Build the pinned Pedigree cross-toolchain without mutating source trees."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


TARGETS = {"x86_64-pedigree"}
GCC_PREREQUISITES = ("gmp", "mpfr", "mpc")
REQUIRED_COMMANDS = ("cc", "c++", "make", "patch", "tar")
TOOLCHAIN_STATE_SCHEMA = 1
TOOLCHAIN_RECIPE = 1
PATCHES = {
    "gcc": "compilers/pedigree-gcc.patch",
    "binutils": "compilers/pedigree-binutils.patch",
}


class BootstrapError(RuntimeError):
    pass


@dataclass(frozen=True)
class Archive:
    name: str
    version: str
    archive: str
    source_dir: str
    url: str
    sha256: str


def load_archives(path: Path) -> dict[str, Archive]:
    with path.open(encoding="utf-8") as stream:
        raw = json.load(stream)
    return {
        name: Archive(name=name, **details)
        for name, details in raw.items()
    }


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the pinned Pedigree cross-toolchain."
    )
    parser.add_argument("target", choices=sorted(TARGETS))
    parser.add_argument(
        "prefix",
        type=Path,
        help="installation prefix",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Pedigree checkout containing compilers/ and the patches",
    )
    parser.add_argument(
        "--sysroot",
        type=Path,
        help="directory containing the libc startup objects and headers",
    )
    parser.add_argument(
        "--libcpp",
        action="store_true",
        help="finish the compiler against target headers and install libstdc++",
    )
    parser.add_argument(
        "--activate",
        action="store_true",
        help="point compilers/dir at this prefix after checking it is safe",
    )
    parser.add_argument(
        "--jobs",
        type=positive_int,
        default=min(os.cpu_count() or 1, 8),
        help="parallel make jobs (default: up to 8)",
    )
    parser.add_argument(
        "--keep-build",
        action="store_true",
        help="retain the extracted and configured build tree",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the planned work without downloading or building",
    )
    return parser.parse_args(argv)


def shell_command(command: Iterable[str]) -> str:
    return shlex.join(command)


class Bootstrapper:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.source_root = args.source_root.resolve()
        self.prefix = args.prefix.resolve()
        self.build_root = self.prefix / "build_tmp"
        self.download_root = self.prefix / "dl_cache"
        self.manifest = load_archives(
            self.source_root / "build-etc/toolchain/pedigree-cross-toolchain.json"
        )
        self.sysroot = (
            args.sysroot.resolve()
            if args.sysroot
            else self.source_root / "build/musl"
        )
        self.dry_run = args.dry_run
        self.make = ["make", f"-j{args.jobs}"]

    def log(self, message: str) -> None:
        print(message)

    def run(
        self,
        command: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        input_text: str | None = None,
    ) -> None:
        location = f" (in {cwd})" if cwd else ""
        self.log(f"+ {shell_command(command)}{location}")
        if self.dry_run:
            return
        subprocess.run(
            command,
            cwd=cwd,
            env=env,
            input=input_text,
            text=input_text is not None,
            check=True,
        )

    def require_commands(self) -> None:
        if self.dry_run:
            return
        missing = [name for name in REQUIRED_COMMANDS if shutil.which(name) is None]
        if missing:
            raise BootstrapError(
                "required host commands are unavailable: " + ", ".join(missing)
            )

    def activate_prefix(self) -> None:
        link = self.source_root / "compilers/dir"
        if self.dry_run:
            self.log(f"would activate {link} -> {self.prefix}")
            return
        if not self.installation_current(require_libcpp=True):
            raise BootstrapError("refusing to activate an incomplete toolchain")
        if link.exists() and not link.is_symlink():
            raise BootstrapError(f"refusing to replace compiler directory: {link}")
        if link.is_symlink() and link.resolve() == self.prefix:
            return
        temporary = link.with_name(f".{link.name}.{os.getpid()}.tmp")
        if temporary.exists() or temporary.is_symlink():
            raise BootstrapError(f"temporary activation path already exists: {temporary}")
        try:
            temporary.symlink_to(self.prefix)
            os.replace(temporary, link)
        finally:
            if temporary.is_symlink():
                temporary.unlink()

    def prefix_is_active(self) -> bool:
        link = self.source_root / "compilers/dir"
        return link.is_symlink() and link.resolve() == self.prefix

    def environment(self) -> dict[str, str]:
        environment = os.environ.copy()
        for name in (
            "CC",
            "CXX",
            "AS",
            "CPP",
            "CFLAGS",
            "CXXFLAGS",
            "CFLAGS_FOR_TARGET",
            "CXXFLAGS_FOR_TARGET",
            "LDFLAGS",
            "ASFLAGS",
        ):
            environment[name] = ""
        return environment

    def host_configure_environment(self) -> dict[str, str]:
        environment = self.environment()
        environment["CC"] = "cc -std=gnu17"
        environment["CXX"] = "c++ -std=gnu++14"
        return environment

    def gcc_environment(
        self, *, with_headers: bool, configure: bool
    ) -> dict[str, str]:
        environment = (
            self.host_configure_environment() if configure else self.environment()
        )
        if with_headers:
            # Userspace DSOs consume the static target runtimes, so every target
            # object in the final compiler pass must be suitable for a DSO.
            environment["CFLAGS_FOR_TARGET"] = "-g -O2 -fPIC"
            environment["CXXFLAGS_FOR_TARGET"] = "-g -O2 -fPIC"
        return environment

    def host_dependency_flags(self) -> list[str]:
        # GCC's bundled zlib 1.2.11 mistakes modern macOS for classic Mac OS.
        # The SDK provides a maintained zlib and is already on the default path.
        if sys.platform == "darwin":
            return ["--with-system-zlib"]
        return []

    def download(self, archive: Archive) -> Path:
        destination = self.download_root / archive.archive
        if destination.exists() and not self.dry_run:
            self.verify_hash(destination, archive)
            return destination
        self.log(f"download {archive.name} {archive.version}: {archive.url}")
        if self.dry_run:
            return destination
        self.download_root.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        try:
            with urllib.request.urlopen(archive.url) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            self.verify_hash(temporary, archive)
            temporary.replace(destination)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        return destination

    @staticmethod
    def verify_hash(path: Path, archive: Archive) -> None:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != archive.sha256:
            raise BootstrapError(
                f"SHA-256 mismatch for {path}: expected {archive.sha256}, "
                f"got {digest.hexdigest()}"
            )

    def extract(self, archive: Archive, archive_path: Path) -> Path:
        source = self.build_root / archive.source_dir
        if source.exists():
            return source
        if not self.dry_run:
            self.build_root.mkdir(parents=True, exist_ok=True)
        self.run(["tar", "-xf", str(archive_path)], cwd=self.build_root)
        return source

    def patch_sources(self, sources: dict[str, Path]) -> None:
        for name, source in sources.items():
            marker = source / ".pedigree-patched"
            patch = self.source_root / PATCHES[name]
            patch_digest = hashlib.sha256(patch.read_bytes()).hexdigest()
            if marker.exists() and marker.read_text(encoding="utf-8").strip() == patch_digest:
                continue
            if marker.exists():
                raise BootstrapError(
                    f"patch changed after it was applied; remove {source} and retry"
                )
            patch_contents = None if self.dry_run else patch.read_text(encoding="utf-8")
            self.run(["patch", "-p1"], cwd=source, input_text=patch_contents)
            if not self.dry_run:
                marker.write_text(patch_digest + "\n", encoding="utf-8")

    def link_gcc_prerequisites(self, gcc_source: Path, sources: dict[str, Path]) -> None:
        for name in GCC_PREREQUISITES:
            destination = gcc_source / name
            source = sources[name]
            self.log(f"link {destination} -> {source}")
            if self.dry_run:
                continue
            if destination.is_symlink() and destination.resolve() == source:
                continue
            if destination.exists() or destination.is_symlink():
                raise BootstrapError(
                    f"refusing to replace GCC prerequisite path: {destination}"
                )
            destination.symlink_to(source)

    def libcpp_installed(self) -> bool:
        version = self.manifest["gcc"].version
        return (
            self.prefix / f"{self.args.target}/lib/libstdc++.a"
        ).is_file() and (
            self.prefix
            / f"{self.args.target}/include/c++/{version}/{self.args.target}/bits/c++config.h"
        ).is_file()

    @property
    def state_path(self) -> Path:
        return self.prefix / ".pedigree-toolchain-state.json"

    def state_fingerprint(self, *, libcpp: bool) -> dict[str, object]:
        archives = {
            name: {"version": archive.version, "sha256": archive.sha256}
            for name, archive in sorted(self.manifest.items())
        }
        patches = {
            name: hashlib.sha256(
                (self.source_root / relative).read_bytes()
            ).hexdigest()
            for name, relative in sorted(PATCHES.items())
        }
        return {
            "schema": TOOLCHAIN_STATE_SCHEMA,
            "recipe": TOOLCHAIN_RECIPE,
            "target": self.args.target,
            "host": {
                "platform": sys.platform,
                "machine": platform.machine(),
            },
            "archives": archives,
            "patches": patches,
            "libcpp": libcpp,
        }

    def read_state(self) -> dict[str, object] | None:
        try:
            with self.state_path.open(encoding="utf-8") as stream:
                state = json.load(stream)
        except (FileNotFoundError, json.JSONDecodeError, OSError):
            return None
        return state if isinstance(state, dict) else None

    def write_state(self, *, libcpp: bool) -> None:
        state = self.state_fingerprint(libcpp=libcpp)
        self.prefix.mkdir(parents=True, exist_ok=True)
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                dir=self.prefix,
                prefix=".pedigree-toolchain-state.",
                delete=False,
            ) as stream:
                temporary_path = Path(stream.name)
                json.dump(state, stream, indent=2, sort_keys=True)
                stream.write("\n")
            os.replace(temporary_path, self.state_path)
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)

    @staticmethod
    def checked_output(command: list[str]) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise BootstrapError(
                f"toolchain check failed: {shell_command(command)}"
                + (f": {detail}" if detail else "")
            )
        return result

    def validate_installation(self, *, require_libcpp: bool) -> None:
        target = self.args.target
        tools = {
            name: self.prefix / "bin" / ("nasm" if name == "nasm" else f"{target}-{name}")
            for name in (
                "gcc",
                "g++",
                "ld",
                "ar",
                "nm",
                "objdump",
                "objcopy",
                "strip",
                "nasm",
            )
        }
        for name, executable in tools.items():
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise BootstrapError(f"required installed tool is unavailable: {name}")

        gcc_version = self.manifest["gcc"].version
        binutils_version = self.manifest["binutils"].version
        nasm_version = self.manifest["nasm"].version
        for name in ("gcc", "g++"):
            version = self.checked_output(
                [str(tools[name]), "-dumpfullversion"]
            ).stdout.strip()
            machine = self.checked_output(
                [str(tools[name]), "-dumpmachine"]
            ).stdout.strip()
            if version != gcc_version or machine != target:
                raise BootstrapError(
                    f"installed {name} identifies as {machine} {version}, "
                    f"expected {target} {gcc_version}"
                )
        for name in ("ld", "ar", "nm", "objdump", "objcopy", "strip"):
            output = self.checked_output([str(tools[name]), "--version"]).stdout
            if f"(GNU Binutils) {binutils_version}" not in output.splitlines()[0]:
                raise BootstrapError(
                    f"installed {name} is not Binutils {binutils_version}"
                )
        output = self.checked_output([str(tools["nasm"]), "-version"]).stdout
        if not output.startswith(f"NASM version {nasm_version}"):
            raise BootstrapError(f"installed nasm is not NASM {nasm_version}")

        with tempfile.TemporaryDirectory(prefix="pedigree-toolchain-check-") as temporary:
            check_root = Path(temporary)
            compile_probes = (
                (
                    "gcc",
                    "c",
                    "int pedigree_compiler_probe(void) { return 0; }\n",
                ),
                (
                    "g++",
                    "c++",
                    "template<class T> T probe(T value) { return value; }\n"
                    "int pedigree_compiler_probe() { return probe(0); }\n",
                ),
            )
            for name, language, source in compile_probes:
                output_path = check_root / f"{name}.o"
                result = subprocess.run(
                    [
                        str(tools[name]),
                        "-ffreestanding",
                        "-x",
                        language,
                        "-c",
                        "-",
                        "-o",
                        str(output_path),
                    ],
                    input=source,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if result.returncode != 0 or not output_path.is_file():
                    raise BootstrapError(
                        f"installed {name} cannot compile {language}: "
                        f"{result.stderr.strip()}"
                    )

            if require_libcpp:
                if not self.libcpp_installed():
                    raise BootstrapError("installed libstdc++ is incomplete")
                verbose = self.checked_output([str(tools["g++"]), "-v"])
                if "Thread model: posix" not in verbose.stderr:
                    raise BootstrapError("final compiler does not use POSIX threads")
                shared = check_root / "libpedigree-cxx-probe.so"
                result = subprocess.run(
                    [
                        str(tools["g++"]),
                        "-shared",
                        "-fPIC",
                        "-fstack-protector-strong",
                        "-x",
                        "c++",
                        "-",
                        "-o",
                        str(shared),
                    ],
                    input=(
                        "#include <string>\n"
                        "extern \"C\" unsigned pedigree_cxx_probe() {\n"
                        "  volatile char stack_guard_probe[16] = {};\n"
                        "  std::string value(\"ok\");\n"
                        "  return static_cast<unsigned>(value.size()) + "
                        "stack_guard_probe[0];\n"
                        "}\n"
                    ),
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if result.returncode != 0 or not shared.is_file():
                    raise BootstrapError(
                        "installed libstdc++ is not usable from a shared library: "
                        + result.stderr.strip()
                    )
                libgcc = (
                    self.prefix
                    / f"lib/gcc/{target}/{gcc_version}/libgcc.a"
                )
                symbols = self.checked_output(
                    [str(tools["nm"]), "-A", str(libgcc)]
                ).stdout
                weak_pthread = [
                    line
                    for line in symbols.splitlines()
                    if len(line.split()) >= 2
                    and line.split()[-2].lower() == "w"
                    and line.split()[-1].startswith("pthread_")
                ]
                if weak_pthread:
                    raise BootstrapError(
                        "installed libgcc retains unsafe weak pthread references"
                    )

    def installation_current(self, *, require_libcpp: bool) -> bool:
        state = self.read_state()
        if state is None or not isinstance(state.get("libcpp"), bool):
            return False
        try:
            expected = self.state_fingerprint(libcpp=bool(state["libcpp"]))
        except OSError:
            return False
        if state != expected or (require_libcpp and not state["libcpp"]):
            return False
        try:
            self.validate_installation(require_libcpp=require_libcpp)
        except (BootstrapError, OSError):
            return False
        return True

    def build_nasm(self, archive: Archive, archive_path: Path) -> None:
        source = self.extract(archive, archive_path)
        self.run(
            ["./configure", f"--prefix={self.prefix}"],
            cwd=source,
            env=self.environment(),
        )
        self.run(self.make, cwd=source, env=self.environment())
        self.run(["make", "install"], cwd=source, env=self.environment())

    def configure_binutils(self, source: Path, build: Path) -> None:
        flags = [
            "--target=" + self.args.target,
            "--prefix=" + str(self.prefix),
            "--disable-nls",
            "--disable-gold",
            "--enable-ld=default",
            "--disable-multilib",
            "--with-sysroot=" + str(self.prefix / self.args.target),
            "--enable-lto",
            "--disable-werror",
            *self.host_dependency_flags(),
        ]
        self.run(
            [str(source / "configure"), *flags],
            cwd=build,
            env=self.host_configure_environment(),
        )

    def configure_gcc(self, source: Path, build: Path, with_headers: bool) -> None:
        flags = [
            "--target=" + self.args.target,
            "--prefix=" + str(self.prefix),
            "--disable-nls",
            "--enable-languages=c,c++",
            "--disable-multilib",
            "--with-sysroot=" + str(self.prefix / self.args.target),
            "--with-native-system-header-dir=/include",
            "--enable-lto",
            "--disable-werror",
            *self.host_dependency_flags(),
        ]
        if with_headers:
            flags.extend(
                (
                    "--without-newlib",
                    "--with-headers",
                    "--enable-threads=posix",
                    "--disable-libstdcxx-pch",
                    "--disable-shared",
                )
            )
        else:
            # The stage-one compiler precedes musl.  This keeps libgcc from
            # depending on libc and prevents fixincludes probing an empty sysroot.
            flags.extend(
                (
                    "--with-newlib",
                    "--without-headers",
                    "--disable-threads",
                    "--disable-fixincludes",
                )
            )
        self.run(
            [str(source / "configure"), *flags],
            cwd=build,
            env=self.gcc_environment(with_headers=with_headers, configure=True),
        )

    def build_base_compiler(self, sources: dict[str, Path]) -> None:
        binutils_build = self.build_root / f"build-binutils-r{TOOLCHAIN_RECIPE}"
        binutils_build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
        self.configure_binutils(sources["binutils"], binutils_build)
        self.run([*self.make, "all"], cwd=binutils_build, env=self.environment())
        self.run(["make", "install"], cwd=binutils_build, env=self.environment())

        gcc_build = self.build_root / f"build-gcc-r{TOOLCHAIN_RECIPE}"
        gcc_build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
        self.configure_gcc(sources["gcc"], gcc_build, with_headers=False)
        self.run(
            [*self.make, "all-gcc", "all-target-libgcc"],
            cwd=gcc_build,
            env=self.gcc_environment(with_headers=False, configure=False),
        )
        self.run(
            ["make", "install-gcc", "install-target-libgcc"],
            cwd=gcc_build,
            env=self.gcc_environment(with_headers=False, configure=False),
        )

    def build_final_compiler(self, gcc_source: Path) -> None:
        if not self.dry_run and not (self.sysroot / "include").is_dir():
            raise BootstrapError(
                f"--libcpp requires installed musl headers at {self.sysroot / 'include'}"
            )
        if not self.dry_run:
            self.link_sysroot()
        build = self.build_root / f"build-libcpp-r{TOOLCHAIN_RECIPE}"
        build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
        self.configure_gcc(gcc_source, build, with_headers=True)
        environment = self.gcc_environment(with_headers=True, configure=False)
        self.run(
            [
                *self.make,
                "all-gcc",
                "all-target-libgcc",
                "all-target-libstdc++-v3",
            ],
            cwd=build,
            env=environment,
        )
        self.run(
            [
                "make",
                "install-gcc",
                "install-target-libgcc",
                "install-target-libstdc++-v3",
            ],
            cwd=build,
            env=environment,
        )

    def link_sysroot(self) -> None:
        gcc_version = self.manifest["gcc"].version
        relative_targets = [
            self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/crt1.o",
            self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/rcrt1.o",
            self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/Scrt1.o",
            self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/crti.o",
            self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/crtn.o",
            self.prefix / f"{self.args.target}/lib/crt1.o",
            self.prefix / f"{self.args.target}/lib/rcrt1.o",
            self.prefix / f"{self.args.target}/lib/Scrt1.o",
            self.prefix / f"{self.args.target}/lib/crti.o",
            self.prefix / f"{self.args.target}/lib/crtn.o",
        ]
        for destination in relative_targets:
            source = self.sysroot / "lib" / destination.name
            self.link_path(source, destination, "startup object")
        target_lib = self.prefix / self.args.target / "lib"
        sysroot_lib = self.sysroot / "lib"
        if self.dry_run:
            self.log(f"link musl libraries from {sysroot_lib} into {target_lib}")
        elif sysroot_lib.is_dir():
            for source in sorted(sysroot_lib.iterdir()):
                destination = target_lib / source.name
                self.link_path(source, destination, "target library")
        include = self.prefix / self.args.target / "include"
        self.link_path(self.sysroot / "include", include, "target include directory")

    def link_path(self, source: Path, destination: Path, description: str) -> None:
        self.log(f"link {destination} -> {source}")
        if self.dry_run:
            return
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink() and destination.resolve() == source.resolve():
            return
        if destination.exists() or destination.is_symlink():
            raise BootstrapError(f"refusing to replace {description}: {destination}")
        destination.symlink_to(source)

    def clean(self) -> None:
        if self.args.keep_build or self.dry_run:
            return
        if self.build_root.exists():
            shutil.rmtree(self.build_root)

    def build(self) -> None:
        self.require_commands()
        if self.args.activate and not self.args.libcpp:
            raise BootstrapError("--activate requires the final --libcpp stage")
        if not self.dry_run and self.installation_current(
            require_libcpp=self.args.libcpp
        ):
            self.log("Toolchain: already installed")
            self.link_sysroot()
            self.clean()
            if self.args.activate:
                self.activate_prefix()
            self.log("Toolchain bootstrap complete.")
            return
        if not self.dry_run and self.prefix_is_active():
            raise BootstrapError(
                "refusing to rebuild the active toolchain; use a side-by-side prefix"
            )

        self.prefix.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
        base_current = (
            False
            if self.dry_run
            else self.installation_current(require_libcpp=False)
        )
        selected = ["gcc", *GCC_PREREQUISITES]
        if not base_current:
            selected.extend(("binutils", "nasm"))
        archives = {name: self.manifest[name] for name in selected}
        archive_paths = {name: self.download(item) for name, item in archives.items()}
        sources = {
            name: self.extract(item, archive_paths[name])
            for name, item in archives.items()
            if name != "nasm"
        }
        if not base_current:
            self.build_nasm(archives["nasm"], archive_paths["nasm"])
            self.patch_sources({name: sources[name] for name in PATCHES})
        else:
            self.patch_sources({"gcc": sources["gcc"]})
        self.link_gcc_prerequisites(sources["gcc"], sources)
        if not base_current:
            self.build_base_compiler({name: sources[name] for name in PATCHES})
            self.link_sysroot()
            if not self.dry_run:
                self.validate_installation(require_libcpp=False)
                self.write_state(libcpp=False)
        if self.args.libcpp:
            self.build_final_compiler(sources["gcc"])
            self.link_sysroot()
            if not self.dry_run:
                self.validate_installation(require_libcpp=True)
                self.write_state(libcpp=True)
        self.clean()
        if self.args.activate:
            self.activate_prefix()
        self.log("Toolchain bootstrap complete.")


def main(argv: list[str]) -> int:
    try:
        Bootstrapper(parse_args(argv)).build()
    except (BootstrapError, OSError, subprocess.CalledProcessError) as error:
        print(f"bootstrap_toolchain.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
