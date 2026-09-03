#!/usr/bin/env python
"""Create deterministic compressed and uncompressed Pedigree initrds."""

from __future__ import print_function

import argparse
import hashlib
import io
import struct
import tarfile
import zlib


def gzip_bytes(payload):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    compressed = compressor.compress(payload) + compressor.flush()
    header = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff"
    trailer = struct.pack(
        "<II", zlib.crc32(payload) & 0xFFFFFFFF, len(payload) & 0xFFFFFFFF
    )
    return header + compressed + trailer


def parse_entry(value):
    name, separator, source = value.partition("=")
    if not separator or not name or not source:
        raise argparse.ArgumentTypeError("entries must use NAME=PATH")
    if "/" in name or name in (".", ".."):
        raise argparse.ArgumentTypeError("entry names must be plain filenames")
    return name, source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--uncompressed", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("entry", nargs="+", type=parse_entry)
    args = parser.parse_args()

    # Module execution follows archive order. Keep the declaration order from
    # CMake while normalizing metadata around it.
    entries = args.entry
    names = [name for name, _ in entries]
    if len(names) != len(set(names)):
        parser.error("entry names must be unique")

    manifest_lines = []
    archive_buffer = io.BytesIO()
    with tarfile.open(fileobj=archive_buffer, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, source in entries:
            with open(source, "rb") as source_file:
                payload = source_file.read()

            info = tarfile.TarInfo(name)
            info.size = len(payload)
            info.mode = 0o755
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mtime = 0
            archive.addfile(info, io.BytesIO(payload))
            manifest_lines.append(
                "%s  %s\n" % (hashlib.sha256(payload).hexdigest(), name)
            )

    archive_payload = archive_buffer.getvalue()
    with open(args.uncompressed, "wb") as output:
        output.write(archive_payload)
    with open(args.output, "wb") as output:
        output.write(gzip_bytes(archive_payload))
    with open(args.manifest, "w") as manifest:
        manifest.write("".join(manifest_lines))


if __name__ == "__main__":
    main()
