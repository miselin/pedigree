#!/usr/bin/env python3
"""Prepare isolated true-vfork and fork-mapped POSIX modules and initrds."""

import argparse
import difflib
import gzip
import hashlib
import io
import json
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import zlib
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command_words(command):
    words = shlex.split(command) if isinstance(command, str) else list(command)
    require(words and all(word not in {"&&", ";", "|", ">", "<"} for word in words),
            "captured commands must be single commands without shell operators")
    require(not any(word.startswith("@") for word in words),
            "response files are not supported")
    return words


def replace_output(words, output):
    require(words.count("-o") == 1, "expected exactly one -o output argument")
    index = words.index("-o") + 1
    require(index < len(words), "missing -o output value")
    old = words[index]
    words[index] = str(output)
    return old


def archive_members(raw):
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
        members = archive.getmembers()
    require(len({member.name for member in members}) == len(members),
            "initrd contains duplicate names")
    require(all(member.isfile() and member.offset_data == member.offset + 512
                and not member.pax_headers for member in members),
            "initrd must contain plain regular-file headers without extensions")
    return members


def replace_initrd(compressed, original_module, replacement):
    raw = gzip.decompress(compressed)
    before = archive_members(raw)
    targets = [member for member in before if member.name == "posix.o"]
    require(len(targets) == 1, "initrd must contain exactly one posix.o")
    target = targets[0]
    require(raw[target.offset_data:target.offset_data + target.size] == original_module,
            "built initrd posix.o differs from built module; rebuild the base initrd")
    require(len(replacement) < 8 ** 11, "replacement is too large for USTAR size field")
    header = bytearray(raw[target.offset:target.offset_data])
    header[124:136] = f"{len(replacement):011o}\0".encode()
    header[148:156] = b" " * 8
    header[148:156] = f"{sum(header):06o}\0 ".encode()
    old_end = target.offset_data + ((target.size + 511) // 512) * 512
    changed = (raw[:target.offset] + header + replacement
               + b"\0" * (-len(replacement) % 512) + raw[old_end:])
    after = archive_members(changed)
    require([m.name for m in before] == [m.name for m in after], "initrd order changed")
    manifest = []
    for left, right in zip(before, after):
        old = raw[left.offset_data:left.offset_data + left.size]
        new = changed[right.offset_data:right.offset_data + right.size]
        old_header = bytearray(raw[left.offset:left.offset_data])
        new_header = bytearray(changed[right.offset:right.offset_data])
        if left.name == "posix.o":
            old_header[124:136] = new_header[124:136]
            old_header[148:156] = new_header[148:156]
            require(new == replacement, "replacement payload verification failed")
        else:
            require(old == new, f"unrelated initrd payload changed: {left.name}")
        require(old_header == new_header, f"initrd metadata changed: {left.name}")
        manifest.append({"name": left.name, "original_sha256": hashlib.sha256(old).hexdigest(),
                         "variant_sha256": hashlib.sha256(new).hexdigest()})
    require(compressed[:4] == b"\x1f\x8b\x08\x00", "unsupported gzip header flags")
    compressor = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    packed = (compressed[:10] + compressor.compress(changed) + compressor.flush()
              + struct.pack("<II", zlib.crc32(changed), len(changed) & 0xFFFFFFFF))
    require(gzip.decompress(packed) == changed, "compressed initrd verification failed")
    return raw, changed, packed, manifest


def prepare(args):
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    require(not args.output_dir.exists() and not args.output_dir.is_symlink(),
            "output directory must be new")
    require(not output.is_relative_to(root) and not output.is_relative_to(build),
            "output directory must be outside the checkout and build directory")
    source = root / "src/modules/subsys/posix/PosixSyscallManager.cc"
    module_dir = build / "src/modules"
    database = build / "compile_commands.json"
    link_file = module_dir / "CMakeFiles/posix.dir/link.txt"
    cache_file = build / "CMakeCache.txt"
    original_module = module_dir / "posix.o"
    original_debug = module_dir / "posix.debug"
    original_initrd = module_dir / "initrd.tar"
    sources = [source, source.parent / "syscalls/translate.h",
               source.parent / "syscalls/linuxSyscallMappings-amd64.h"]
    inputs = [database, link_file, cache_file, original_module, original_debug,
              original_initrd, *sources]
    for path in inputs:
        require(path.is_file(), f"missing input: {path}")
    entries = [entry for entry in json.loads(database.read_text())
               if (Path(entry["directory"]) / entry["file"]).resolve() == source]
    require(len(entries) == 1, "expected one POSIX dispatcher compile command")
    entry = entries[0]
    cwd = Path(entry["directory"]).resolve()
    compile_original = command_words(entry.get("arguments") or entry["command"])
    link_original = command_words(link_file.read_text())
    require(not any(word.startswith(("-save-temps", "-ftime-trace", "-MJ", "-Wl,-Map",
                                     "-Wl,--dependency-file", "-Wp,-M", "-aux-info"))
                    for word in compile_original + link_original),
            "captured command has unsupported side-output flags")
    settings = {}
    for line in cache_file.read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            settings[key.split(":", 1)[0]] = value
    objcopy = settings.get("PEDIGREE_MODULE_OBJCOPY") or settings.get("CMAKE_OBJCOPY")
    strip = settings.get("PEDIGREE_MODULE_STRIP") or settings.get("CMAKE_STRIP")
    require(objcopy and strip, "missing module objcopy/strip tools in CMakeCache.txt")
    for tool in (compile_original[0], link_original[0], objcopy, strip):
        require(shutil.which(tool), f"missing executable: {tool}")
    mapping = sources[2].read_text()
    old = "PEDIGREE_LINUX_AMD64_SYSCALL(vfork, 58, POSIX_VFORK)"
    require(mapping.count(old) == 1, "expected exactly one true-vfork mapping")
    changed_mapping = mapping.replace(old, old.replace("POSIX_VFORK", "POSIX_FORK"))
    link_inputs = [link_file]
    for word in link_original[1:]:
        if not word.startswith("-") and word.endswith((".o", ".a", ".so")):
            path = (module_dir / word).resolve()
            if path != original_module:
                require(path.is_file(), f"missing link input: {path}")
                link_inputs.append(path)
        if word.startswith("-Wl,-T,"):
            link_inputs.append((module_dir / word[len("-Wl,-T,"):]).resolve())
    require(all(path.is_file() for path in link_inputs), "missing linker script")
    module_mtime = original_module.stat().st_mtime_ns
    stale_inputs = [str(path) for path in link_inputs if path.stat().st_mtime_ns > module_mtime]
    require(not stale_inputs, "base posix.o predates link inputs; rebuild the base module: "
            + ", ".join(stale_inputs))
    inputs.extend(link_inputs)
    original_hashes = {str(path): digest(path) for path in inputs}
    replace_initrd(original_initrd.read_bytes(), original_module.read_bytes(),
                   original_module.read_bytes())
    output.mkdir(parents=True, exist_ok=False)
    record = {"status": "preparing", "build_dir": str(build), "source_root": str(root),
              "input_sha256": original_hashes, "commands": [], "compile_entry": entry,
              "original_link_command": link_original}
    provenance = output / "provenance.json"

    def run(words, directory):
        log = output / f"command-{len(record['commands']):02d}.log"
        record["commands"].append({"argv": words, "cwd": str(directory), "log": log.name})
        with log.open("wb") as stream:
            subprocess.run(words, cwd=directory, stdout=stream, stderr=subprocess.STDOUT,
                           check=True, timeout=300)

    try:
        record["head"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        record["git_status"] = subprocess.check_output(
            ["git", "status", "--porcelain=v1"], cwd=root, text=True)
        (output / "mapping.diff").write_text("".join(difflib.unified_diff(
            mapping.splitlines(keepends=True), changed_mapping.splitlines(keepends=True),
            fromfile=str(sources[2]), tofile="fork-mapped/syscalls/linuxSyscallMappings-amd64.h")))
        shadow = output / "shadow/posix"
        (shadow / "syscalls").mkdir(parents=True)
        for path in sources:
            shutil.copy2(path, shadow / path.relative_to(source.parent))
        (shadow / "syscalls/linuxSyscallMappings-amd64.h").write_text(changed_mapping)
        first, second = output / "true-vfork", output / "fork-mapped"
        first.mkdir()
        second.mkdir()
        for path in (original_module, original_debug, original_initrd):
            shutil.copy2(path, first / path.name)
        variant_object = second / "PosixSyscallManager.cc.o"
        compile_command = compile_original.copy()
        dispatcher = (cwd / replace_output(compile_command, variant_object)).resolve()
        require(dispatcher.is_file(), f"missing original dispatcher object: {dispatcher}")
        require(all(dispatcher.stat().st_mtime_ns >= path.stat().st_mtime_ns for path in sources),
                "dispatcher predates source inputs; rebuild the base module")
        matches = [i for i, word in enumerate(compile_command)
                   if not word.startswith("-") and (cwd / word).resolve() == source]
        require(len(matches) == 1, "compile command does not name exactly one dispatcher")
        compile_command[matches[0]] = str(shadow / source.name)
        for flag in ("-MF", "-MT", "-MQ"):
            if flag in compile_command:
                index = compile_command.index(flag) + 1
                require(index < len(compile_command), f"missing {flag} value")
                compile_command[index] = str(variant_object) + (".d" if flag == "-MF" else "")
            for index, word in enumerate(compile_command):
                if word.startswith(flag) and word != flag:
                    compile_command[index] = flag + str(variant_object) + (
                        ".d" if flag == "-MF" else "")
        compile_command.extend(["-iquote", str(source.parent),
                                f"-fmacro-prefix-map={shadow}={source.parent}",
                                f"-fdebug-prefix-map={shadow}={source.parent}"])
        run(compile_command, cwd)
        link_command = link_original.copy()
        require((module_dir / replace_output(link_command, second / "posix.o")).resolve()
                == original_module, "link command output is not posix.o")
        matches = [i for i, word in enumerate(link_command)
                   if not word.startswith("-") and (module_dir / word).resolve() == dispatcher]
        require(len(matches) == 1, "link command does not name exactly one dispatcher object")
        link_command[matches[0]] = str(variant_object)
        run(link_command, module_dir)
        run([objcopy, "--only-keep-debug", str(second / "posix.o"),
             str(second / "posix.debug")], module_dir)
        run([strip, "-g", str(second / "posix.o")], module_dir)
        run([objcopy, f"--add-gnu-debuglink={second / 'posix.debug'}",
             str(second / "posix.o")], module_dir)
        require(digest(second / "posix.o") != original_hashes[str(original_module)],
                "fork-mapped module is identical to the original")
        raw, changed, packed, manifest = replace_initrd(
            original_initrd.read_bytes(), original_module.read_bytes(),
            (second / "posix.o").read_bytes())
        (first / "initrd.tar.uncomp").write_bytes(raw)
        (second / "initrd.tar.uncomp").write_bytes(changed)
        (second / "initrd.tar").write_bytes(packed)
        require(all(digest(Path(path)) == value for path, value in original_hashes.items()),
                "original inputs changed during preparation; discard this comparison")
        record["initrd_members"] = manifest
        record["output_sha256"] = {str(path.relative_to(output)): digest(path)
                                   for directory in (first, second) for path in directory.iterdir()}
        record["status"] = "complete"
        print(f"Prepared comparison: {output}")
    except Exception as error:
        record["status"] = "failed"
        record["error"] = str(error)
        raise
    finally:
        provenance.write_text(json.dumps(record, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    try:
        prepare(parser.parse_args())
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
