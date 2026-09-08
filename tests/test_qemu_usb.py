# SPDX-License-Identifier: ISC
"""USB fixture and QMP protocol tests; no QEMU guest is launched."""

import argparse
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "usb_smoke", Path(__file__).resolve().parents[1] / "scripts/test_qemu_usb.py"
)
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


def arguments(folder, controller="ehci", sector=512):
    return argparse.Namespace(
        image=folder / "root.img", run_dir=folder / "run", qemu="qemu-system-x86_64",
        ovmf=folder / "ovmf.fd", cpus=4, controller=controller, sector_size=sector, timeout=2,
    )


def complete_serial():
    return "USB-SMOKE: READY input\n" + "\n".join(
        f"USB-SMOKE: PASS {step}" for step in smoke.STEPS
    ) + "\nUSB-SMOKE: PASS input-bits=20 observed=0x3f\n"


def scsi_command(name, tail_length, mode, bus="msd.0", first=0):
    return f"[{bus} id=0] {name} 0x{first:02x}" + " 0x00" * (tail_length - 1) + f" - {mode}\n"


class FixtureTests(unittest.TestCase):
    def test_header_and_both_geometries(self):
        for sector in (512, 4096):
            header = smoke.expected_chunk(0, 4096, sector)
            expected = bytearray(4096)
            expected[:21] = b"PEDIGREE-USB-SMOKE-v1"
            expected[32] = 1
            struct.pack_into("<QI", expected, 40, 33554432, sector)
            self.assertEqual(header, expected)
            self.assertEqual(smoke.expected_chunk(4095, 2, sector),
                             b"\0" + smoke.ahci.pattern(4096, 1, 0x5A))

    def test_writes_preserve_neighbours_and_terminal_sector(self):
        for sector in (512, 4096):
            for start, length in smoke.WRITE_RANGES:
                self.assertEqual(smoke.expected_chunk(start - 1, 2, sector, True),
                                 smoke.ahci.pattern(start - 1, 1, 0x5A)
                                 + smoke.ahci.pattern(start, 1, 0xA5))
                self.assertEqual(smoke.expected_chunk(start, length, sector, True),
                                 smoke.ahci.pattern(start, length, 0xA5))
                if start + length < smoke.DISK_SIZE:
                    self.assertEqual(smoke.expected_chunk(start + length - 1, 2, sector, True),
                                     smoke.ahci.pattern(start + length - 1, 1, 0xA5)
                                     + smoke.ahci.pattern(start + length, 1, 0x5A))

    def test_whole_fixture_rejects_missing_writes_corruption_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "usb.img"
            smoke.create_fixture(path, 4096)
            with self.assertRaises(FileExistsError):
                smoke.create_fixture(path, 4096)
            with self.assertRaisesRegex(RuntimeError, "byte 8388608"):
                smoke.verify_fixture(path, 4096)
            with path.open("r+b") as stream:
                for offset, length in smoke.WRITE_RANGES:
                    stream.seek(offset)
                    stream.write(smoke.ahci.pattern(offset, length, 0xA5))
            result = smoke.verify_fixture(path, 4096)
            self.assertEqual(result["verified_bytes"], 33554432)
            self.assertEqual(len(result["sha256"]), 64)
            with path.open("r+b") as stream:
                stream.seek(4095)
                stream.write(b"\x01")
            with self.assertRaisesRegex(RuntimeError, "byte 4095"):
                smoke.verify_fixture(path, 4096)
            with path.open("r+b") as stream:
                stream.truncate(smoke.DISK_SIZE - 1)
            with self.assertRaisesRegex(RuntimeError, "size changed"):
                smoke.verify_fixture(path, 4096)


class EvidenceTests(unittest.TestCase):
    def test_completion_requires_every_stage_and_actual_input(self):
        self.assertEqual(smoke.serial_progress(complete_serial())["observed"], 0x3F)
        for step in smoke.STEPS[:-1]:
            with self.assertRaisesRegex(RuntimeError, "lacks required"):
                smoke.serial_progress(complete_serial().replace(f"PASS {step}", "unrelated"))
        for old, new in (("READY input", "not ready"), ("observed=0x3f", "observed=0x1f")):
            with self.assertRaisesRegex(RuntimeError, "lacks required"):
                smoke.serial_progress(complete_serial().replace(old, new))
        self.assertNotIn("complete", smoke.serial_progress("USB-SMOKE: PASS complete-fake")["steps"])
        for failure in ("USB-SMOKE: FAIL short read", "panic: bad", "(FF) fatal"):
            with self.assertRaisesRegex(RuntimeError, "guest failure"):
                smoke.serial_progress(complete_serial() + failure)

    def test_hex_input_accumulates_independent_bit_notifications(self):
        text = "\n".join(
            f"USB-SMOKE: PASS input-bits={bit:x} observed={bit:x}"
            for bit in (1, 2, 4, 8, 16, 32)
        )
        self.assertEqual(smoke.serial_progress(text)["observed"], 0x3F)

    def test_flush_requires_valid_cdb_after_write_on_same_device(self):
        write = scsi_command("WRITE_10", 9, "to-dev len=4096")
        flush = scsi_command("SYNCHRONIZE_CACHE", 9, "none")
        flush16 = scsi_command("SPACE_16/SYNCHRONIZE_CACHE_16", 15, "none")
        result = smoke.verify_flushes(write + flush + flush16)
        self.assertEqual(result["scratch_flush_commands"], 2)
        self.assertEqual(result["scratch_flush_opcodes"], ["0x35", "0x91"])
        invalid = (
            flush + write, write, "SYNCHRONIZE_CACHE\n",
            write + scsi_command("SYNCHRONIZE_CACHE", 9, "none", bus="another.0"),
            write + scsi_command("SYNCHRONIZE_CACHE", 8, "none"),
            write + scsi_command("SYNCHRONIZE_CACHE", 9, "none", first=2),
            scsi_command("WRITE_10", 9, "to-dev len=0") + flush,
        )
        for text in invalid:
            with self.assertRaisesRegex(RuntimeError, "no guest SCSI flush"):
                smoke.verify_flushes(text)

    def test_commands_isolate_root_and_select_valid_controller_ports(self):
        folder = Path("/tmp/usb,fixture")
        for controller, version in (("ehci", 2), ("uhci", 1)):
            argv = smoke.command(arguments(folder, controller, 4096), folder)
            self.assertIn("q35,usb=off,i8042=off", argv)
            self.assertEqual(argv[argv.index("-qmp") + 1], "stdio")
            root = next(value for value in argv if "if=none,id=root," in value)
            scratch = next(value for value in argv if "if=none,id=scratch," in value)
            self.assertIn("usb,,fixture", root)
            self.assertIn("snapshot=on", root)
            self.assertNotIn("snapshot=on", scratch)
            self.assertTrue(any(f"usb-kbd,id=kbd,bus=usb.0,port=1,usb_version={version}" == value
                                for value in argv))
            storage = next(value for value in argv if value.startswith("usb-storage,"))
            self.assertIn("commandlog=on", storage)
            self.assertIn("logical_block_size=4096,physical_block_size=4096", storage)
            self.assertIn("bus=usb.0,port=3" if controller == "ehci"
                          else "bus=storageusb.0,port=1", storage)
            if controller == "uhci":
                self.assertIn("piix3-usb-uhci,id=storageusb", argv)

    def test_failed_spawn_preserves_report_and_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            args = arguments(Path(directory))
            args.qemu = str(Path(directory) / "missing-qemu")
            with mock.patch.object(smoke, "create_fixture",
                                   side_effect=lambda path, _: path.write_bytes(b"preserve me")):
                result = smoke.run(args)
            self.assertFalse(result["success"])
            self.assertEqual((args.run_dir / "usb.img").read_bytes(), b"preserve me")
            self.assertEqual(json.loads((args.run_dir / "report.json").read_text()), result)
            with self.assertRaises(FileExistsError):
                smoke.run(args)


class QmpTests(unittest.TestCase):
    def peer(self, mode="success"):
        program = r'''
import json, os, sys, time
os.write(1, b'{"QMP":{}}\r\n')
request = json.loads(sys.stdin.readline())
if sys.argv[1] == "timeout":
    time.sleep(10)
else:
    identifier = request["id"] + (1 if sys.argv[1] == "wrong-id" else 0)
    response = {"id": identifier, "return": request}
    if sys.argv[1] == "error":
        response = {"id": identifier, "error": {"class":"GenericError", "desc":"rejected"}}
    payload = b'{"event":"RESUME"}\n' + json.dumps(response).encode() + b'\r\n'
    os.write(1, payload[:7])
    time.sleep(0.01)
    os.write(1, payload[7:])
'''
        child = subprocess.Popen([sys.executable, "-c", program, mode], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

        def close():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=2)
            child.stdin.close()
            child.stdout.close()
            child.stderr.close()

        self.addCleanup(close)
        transcript = io.BytesIO()
        qmp = smoke.Qmp(child, transcript, lambda: None)
        self.assertIn("QMP", qmp.receive(time.monotonic() + 2))
        return qmp, transcript

    def test_partial_frames_events_and_request_payload(self):
        qmp, transcript = self.peer()
        steps = smoke.input_sequence()
        self.assertEqual([step[1] for step in steps], [1, 2, 4, 8, 16, 32])
        self.assertTrue(steps[0][2]["data"]["down"])
        self.assertFalse(steps[1][2]["data"]["down"])
        self.assertEqual(steps[2][2], {"type": "rel", "data": {"axis": "x", "value": 17}})
        self.assertEqual(steps[3][2], {"type": "rel", "data": {"axis": "y", "value": -9}})
        request = qmp.execute("input-send-event", {"events": [steps[0][2]]}, time.monotonic() + 2)
        self.assertEqual(request, {"execute": "input-send-event", "id": 1,
                                  "arguments": {"events": [{"type": "key", "data": {
                                      "down": True, "key": {"type": "qcode", "data": "a"}}}]}})
        self.assertIn(b"input-send-event", transcript.getvalue())

    def test_rejection_and_wrong_id_are_failures(self):
        for mode, message in (("error", "failed"), ("wrong-id", "unexpected request id")):
            qmp, _ = self.peer(mode)
            with self.assertRaisesRegex(RuntimeError, message):
                qmp.execute("qmp_capabilities", None, time.monotonic() + 2)

    def test_response_wait_is_bounded(self):
        qmp, _ = self.peer("timeout")
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            qmp.execute("qmp_capabilities", None, started + 0.05)
        self.assertLess(time.monotonic() - started, 1)


if __name__ == "__main__":
    unittest.main()
