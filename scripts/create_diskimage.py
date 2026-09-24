"""
Copyright (c) 2008-2014, Pedigree Developers

Please see the CONTRIB file in the root of the source tree for a full
list of contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
"""

import argparse
import os
import posixpath
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path, PurePosixPath


def command_path(path):
    value = str(path)
    if not value or any(
        character.isspace() or character == "\0" for character in value
    ):
        raise ValueError(f"ext2img cannot represent this path: {value!r}")
    return value


def target_path(path):
    command_path(path)
    target = PurePosixPath(path)
    if not target.is_absolute() or ".." in target.parts:
        raise ValueError(f"Overlay destination must be an absolute image path: {path}")
    return str(target)


def protect_base_runtime(target):
    protected = (
        "/etc/passwd",
        "/etc/group",
        "/etc/shadow",
        "/etc/gshadow",
        "/etc/apk",
        "/lib/apk",
        "/var/lib/apk",
    )
    if any(target == path or target.startswith(path + "/") for path in protected):
        raise ValueError(
            f"Overlay would replace Alpine account or package data: {target}"
        )
    name = PurePosixPath(target).name
    if target.startswith(("/lib/", "/usr/lib/")) and (
        name in ("libc.so", "libc.a") or name.startswith(("ld-musl-", "libc.musl-"))
    ):
        raise ValueError(f"Overlay would replace Alpine libc: {target}")


def resolve_image_path(base_root, target):
    """Resolve existing image symlinks without following absolute links on the host."""
    parts = list(PurePosixPath(target).parts[1:])
    resolved = []
    links = 0
    while parts:
        part = parts.pop(0)
        candidate = base_root.joinpath(*resolved, part)
        if candidate.is_symlink():
            links += 1
            if links > 40:
                raise ValueError(f"Symlink loop in Alpine root: {target}")
            link = os.readlink(candidate)
            replacement = posixpath.normpath(posixpath.join("/", *resolved, link))
            parts = list(PurePosixPath(replacement).parts[1:]) + parts
            resolved = []
        else:
            resolved.append(part)
    return "/" + "/".join(resolved)


def build_file_list(base_root, files=(), trees=()):
    """Overlay only declared files and trees onto the matching Alpine root image."""
    base_root = Path(base_root)
    if not base_root.is_dir():
        raise ValueError(f"Alpine root directory is missing: {base_root}")
    copies = {}

    def add(source, destination):
        source = Path(source).absolute()
        command_path(source)
        destination = target_path(destination)
        protect_base_runtime(destination)
        parent = resolve_image_path(base_root, str(PurePosixPath(destination).parent))
        destination = posixpath.join(parent, PurePosixPath(destination).name)
        protect_base_runtime(destination)
        if destination == "/" or base_root.joinpath(destination.lstrip("/")).is_dir():
            raise ValueError(
                f"Overlay would replace an Alpine directory: {destination}"
            )
        if not source.is_symlink() and not source.is_file():
            raise ValueError(f"Overlay source is not a file: {source}")
        if destination in copies and copies[destination] != source:
            raise ValueError(f"Multiple overlay sources for {destination}")
        copies[destination] = source

    for source, destination in files:
        add(source, destination)
    for source, destination in trees:
        source = Path(source).absolute()
        destination = target_path(destination)
        if not source.is_dir():
            raise ValueError(f"Overlay source is not a directory: {source}")
        for directory, dirs, entries in os.walk(source):
            directory = Path(directory)
            for name in sorted(dirs + entries):
                entry = directory / name
                if entry.is_symlink() or entry.is_file():
                    add(
                        entry,
                        str(PurePosixPath(destination) / entry.relative_to(source)),
                    )

    directories = set()
    commands = []
    for destination, source in sorted(copies.items()):
        parent = PurePosixPath(destination).parent
        for directory in reversed((parent, *parent.parents)):
            path = str(directory)
            if path in copies:
                raise ValueError(f"Overlay file is also used as a directory: {path}")
            existing = base_root / path.lstrip("/")
            if existing.is_dir():
                continue
            if existing.exists() or existing.is_symlink():
                raise ValueError(f"Overlay parent is not a directory: {path}")
            if path not in directories:
                commands.append(f"mkdir {path}")
                directories.add(path)
        existing = base_root / destination.lstrip("/")
        if existing.exists() or existing.is_symlink():
            commands.append(f"rm {destination}")
        if source.is_symlink():
            link = command_path(os.readlink(source))
            commands.append(f"symlink {destination} {link}")
        else:
            commands.append(f"write {source} {destination}")
            mode = stat.S_IMODE(source.stat().st_mode) & 0o777
            commands.append(f"chmod {destination} {mode:o}")
    return commands


def image_size(base_size, commands):
    if not commands:
        return base_size
    block_size = 4096
    payload = 0
    for command in commands:
        if command.startswith("write "):
            source = command.split()[1]
            size = os.path.getsize(source)
            payload += max(
                block_size, (size + block_size - 1) // block_size * block_size
            )
        elif command.startswith(("mkdir ", "symlink ")):
            payload += block_size
    # Keep the base's free space and add room for overlay blocks and ext2 metadata.
    size = base_size + (payload * 5 + 3) // 4 + (16 << 20)
    alignment = 64 << 20
    return (size + alignment - 1) // alignment * alignment


def e2fsprog(name):
    located = shutil.which(name)
    if located:
        return located
    homebrew = Path("/opt/homebrew/sbin") / name
    if homebrew.is_file():
        return str(homebrew)
    raise FileNotFoundError(f"Install e2fsprogs to provide {name}")


def create_image(target, ext2img, base_image, commands):
    target = Path(target).absolute()
    base_image = Path(base_image).absolute()
    if target == base_image or (target.exists() and target.samefile(base_image)):
        raise ValueError("The output image must differ from the Alpine base image")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".pedigree-image-", dir=target.parent
    ) as temporary:
        image = Path(temporary) / "root.img"
        shutil.copyfile(base_image, image)
        size = image_size(image.stat().st_size, commands)
        if size > image.stat().st_size:
            with image.open("r+b") as stream:
                stream.truncate(size)
            checked = subprocess.run(
                [e2fsprog("e2fsck"), "-pf", str(image)], check=False
            )
            if checked.returncode not in (0, 1):
                raise subprocess.CalledProcessError(checked.returncode, checked.args)
            subprocess.run([e2fsprog("resize2fs"), str(image)], check=True)
        if commands:
            command_file = Path(temporary) / "commands"
            command_file.write_text("\n".join(commands) + "\n")
            subprocess.run(
                [str(ext2img), "-q", "-c", str(command_file), "-f", str(image)],
                check=True,
            )
        os.replace(image, target)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Overlay declared Pedigree artifacts onto an Alpine root image."
    )
    parser.add_argument("target")
    parser.add_argument("ext2img")
    parser.add_argument("--base-image", required=True)
    parser.add_argument("--base-root", required=True)
    parser.add_argument(
        "--file", nargs=2, action="append", default=[], metavar=("SOURCE", "TARGET")
    )
    parser.add_argument(
        "--tree", nargs=2, action="append", default=[], metavar=("SOURCE", "TARGET")
    )
    args = parser.parse_args(argv)
    commands = build_file_list(args.base_root, args.file, args.tree)
    create_image(args.target, args.ext2img, args.base_image, commands)


if __name__ == "__main__":
    main()
