#!/usr/bin/env python3
"""Build the pinned Pedigree cross-toolchain without mutating source trees."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


TARGETS = {
    "i686-pedigree",
    "x86_64-pedigree",
    "amd64-pedigree",
    "i686-elf",
    "amd64-elf",
}
NASM_TARGETS = TARGETS
REQUIRED_COMMANDS = ("autoconf", "autoreconf", "cc", "c++", "make", "patch", "tar")
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


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the pinned Pedigree cross-toolchain."
    )
    parser.add_argument("target", choices=sorted(TARGETS))
    parser.add_argument(
        "prefix",
        type=Path,
        help="installation prefix (the legacy script used compilers/dir)",
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
        "--osx-compat",
        action="store_true",
        help="use the historical Homebrew/MacPorts dependency flags",
    )
    parser.add_argument(
        "--libcpp",
        action="store_true",
        help="also build and install the target libstdc++ archive",
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

    def ensure_prefix_link(self) -> None:
        link = self.source_root / "compilers/dir"
        if self.dry_run:
            self.log(f"would ensure {link} -> {self.prefix}")
            return
        if link.exists() or link.is_symlink():
            if not link.is_symlink() or link.resolve() != self.prefix:
                raise BootstrapError(
                    f"refusing to replace existing compiler path: {link}"
                )
            return
        link.symlink_to(self.prefix)

    def environment(self) -> dict[str, str]:
        environment = os.environ.copy()
        for name in (
            "CC",
            "CXX",
            "AS",
            "CPP",
            "CFLAGS",
            "CXXFLAGS",
            "LDFLAGS",
            "ASFLAGS",
        ):
            environment[name] = ""
        environment["ac_cv_prog_cc_c23"] = "no"
        return environment

    def host_configure_environment(self) -> dict[str, str]:
        environment = self.environment()
        environment["CC"] = "cc -std=gnu17"
        environment["CXX"] = "c++ -std=gnu++14"
        return environment

    def dependency_flags(self) -> list[str]:
        if not self.args.osx_compat:
            return []
        if shutil.which("brew") and not self.dry_run:
            result = subprocess.run(
                ["brew", "--prefix"],
                check=False,
                capture_output=True,
                text=True,
            )
            prefix = result.stdout.strip() if result.returncode == 0 else "/opt/local"
        else:
            prefix = "/opt/local"
        return [
            f"--with-gmp={prefix}",
            f"--with-libiconv-prefix={prefix}",
            "--with-system-zlib",
        ]

    def autoconf_version(self) -> str:
        if self.dry_run:
            return "<host-autoconf-version>"
        result = subprocess.run(
            ["autoconf", "--version"],
            check=True,
            capture_output=True,
            text=True,
        )
        match = re.search(r"\d+\.\d+(?:\.\d+)?", result.stdout)
        if not match:
            raise BootstrapError("unable to determine the installed Autoconf version")
        return match.group(0)

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
        self.build_root.mkdir(parents=True, exist_ok=True)
        self.run(["tar", "-xf", str(archive_path)], cwd=self.build_root)
        return source

    def patch_sources(self, sources: dict[str, Path]) -> None:
        for name, source in sources.items():
            marker = source / ".pedigree-patched"
            if marker.exists():
                continue
            patch = self.source_root / PATCHES[name]
            patch_contents = None if self.dry_run else patch.read_text(encoding="utf-8")
            self.run(["patch", "-p1"], cwd=source, input_text=patch_contents)
            if not self.dry_run:
                marker.touch()

    def fix_autoconf_and_regenerate(self, sources: dict[str, Path]) -> None:
        version = self.autoconf_version()
        for source in sources.values():
            override = source / "config/override.m4"
            if not self.dry_run and not override.exists():
                raise BootstrapError(f"missing Autoconf override file: {override}")
            if self.dry_run:
                self.log(f"update {override} for Autoconf {version}")
            else:
                contents = override.read_text(encoding="utf-8")
                updated = re.sub(
                    r"(_GCC_AUTOCONF_VERSION\], \])[0-9.]+",
                    rf"\g<1>{version}",
                    contents,
                )
                override.write_text(updated, encoding="utf-8")

        self.run(["autoreconf", "--force"], cwd=sources["binutils"], env=self.environment())
        self.run(["autoreconf", "--force"], cwd=sources["gcc"], env=self.environment())
        self.run(
            ["autoconf", "--force"],
            cwd=sources["gcc"] / "libstdc++-v3",
            env=self.environment(),
        )

    def component_installed(self, name: str) -> bool:
        archive = self.manifest[name]
        if name == "nasm":
            executable = self.prefix / "bin/nasm"
            command = [str(executable), "-version"]
            expected = f"NASM version {archive.version}"
        elif name == "binutils":
            executable = self.prefix / f"bin/{self.args.target}-objdump"
            command = [str(executable), "--version"]
            expected = f"GNU objdump (GNU Binutils) {archive.version}"
        else:
            executable = self.prefix / f"bin/{self.args.target}-gcc"
            command = [str(executable), "-dumpversion"]
            expected = archive.version
        if not executable.is_file():
            return False
        if self.dry_run:
            return False
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        return result.returncode == 0 and result.stdout.startswith(expected)

    def libcpp_installed(self) -> bool:
        return (
            self.prefix / f"{self.args.target}/lib/libstdc++.a"
        ).is_file()

    def build_nasm(self, archive: Archive, archive_path: Path) -> None:
        if self.component_installed("nasm"):
            self.log("Nasm: already installed")
            return
        source = self.extract(archive, archive_path)
        self.run(
            ["./configure", f"--prefix={self.prefix}"],
            cwd=source,
            env=self.environment(),
        )
        self.run(["make"], cwd=source, env=self.environment())
        self.run(["make", "install"], cwd=source, env=self.environment())

    def configure_binutils(self, source: Path) -> None:
        flags = [
            "--target=" + self.args.target,
            "--prefix=" + str(self.prefix),
            "--disable-nls",
            "--enable-gold",
            "--enable-ld",
            "--with-sysroot",
            "--enable-lto",
            "--disable-werror",
            *self.dependency_flags(),
        ]
        self.run([str(source / "configure"), *flags], cwd=self.build_root / "build", env=self.host_configure_environment())

    def configure_gcc(self, source: Path) -> None:
        flags = [
            "--target=" + self.args.target,
            "--prefix=" + str(self.prefix),
            "--disable-nls",
            "--enable-languages=c,c++",
            "--without-headers",
            "--without-newlib",
            "--enable-lto",
            "--disable-werror",
            *self.dependency_flags(),
        ]
        self.run([str(source / "configure"), *flags], cwd=self.build_root / "build", env=self.host_configure_environment())

    def configure_libcpp(self, source: Path) -> None:
        flags = [
            "--target=" + self.args.target,
            "--disable-nls",
            "--enable-languages=c++",
            "--without-newlib",
            "--disable-libstdcxx-pch",
            "--enable-shared",
            "--enable-lto",
            *self.dependency_flags(),
        ]
        self.run([str(source / "configure"), *flags], cwd=self.build_root / "build", env=self.host_configure_environment())

    def build_gcc(self, sources: dict[str, Path]) -> None:
        build = self.build_root / "build"
        if self.component_installed("binutils"):
            self.log("Binutils: already installed")
        else:
            build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
            self.configure_binutils(sources["binutils"])
            self.run(["make", "all"], cwd=build, env=self.environment())
            self.run(["make", "install"], cwd=build, env=self.environment())
            if not self.dry_run and build.exists():
                shutil.rmtree(build)
        if self.component_installed("gcc"):
            self.log("GCC: already installed")
        else:
            build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
            self.configure_gcc(sources["gcc"])
            self.run(["make", "all-gcc", "all-target-libgcc"], cwd=build, env=self.environment())
            self.run(["make", "install-gcc", "install-target-libgcc"], cwd=build, env=self.environment())
        if self.args.libcpp:
            build.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
            if self.libcpp_installed() and not self.dry_run:
                self.log("libstdc++: already installed")
            else:
                self.configure_libcpp(sources["gcc"])
                self.run(["make", "all-target-libstdc++-v3"], cwd=build, env=self.environment())
                self.run(["make", "install-target-libstdc++-v3"], cwd=build, env=self.environment())

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
            self.log(f"link {destination} -> {source}")
            if not self.dry_run:
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.unlink(missing_ok=True)
                destination.symlink_to(source)
        include = self.prefix / self.args.target / "include"
        self.log(f"link {include} -> {self.sysroot / 'include'}")
        if not self.dry_run:
            include.parent.mkdir(parents=True, exist_ok=True)
            include.unlink(missing_ok=True)
            include.symlink_to(self.sysroot / "include")
        include_fixed = self.prefix / f"lib/gcc/{self.args.target}/{gcc_version}/include-fixed"
        if include_fixed.is_dir() and not include_fixed.is_symlink() and not self.dry_run:
            shutil.rmtree(include_fixed)

    def clean(self) -> None:
        if self.args.keep_build or self.dry_run:
            return
        if self.build_root.exists():
            shutil.rmtree(self.build_root)

    def build(self) -> None:
        self.require_commands()
        self.ensure_prefix_link()
        self.prefix.mkdir(parents=True, exist_ok=True) if not self.dry_run else None
        selected = ["gcc", "binutils"]
        if self.args.target in NASM_TARGETS:
            selected.append("nasm")
        if (
            not self.dry_run
            and all(self.component_installed(name) for name in selected)
            and (not self.args.libcpp or self.libcpp_installed())
        ):
            self.log("Toolchain: already installed")
            self.link_sysroot()
            self.log("Toolchain bootstrap complete.")
            return
        archives = {name: self.manifest[name] for name in selected}
        archive_paths = {name: self.download(item) for name, item in archives.items()}
        sources = {
            name: self.extract(item, archive_paths[name])
            for name, item in archives.items()
            if name != "nasm"
        }
        if "nasm" in archives:
            self.build_nasm(archives["nasm"], archive_paths["nasm"])
        self.patch_sources(sources)
        self.fix_autoconf_and_regenerate(sources)
        self.build_gcc(sources)
        self.link_sysroot()
        self.clean()
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
