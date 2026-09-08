# SPDX-License-Identifier: ISC
"""Host-side fixture and evidence checks; these tests do not launch QEMU."""

import argparse
import configparser
import errno
import importlib.util
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "qemu_ahci", Path(__file__).resolve().parents[1] / "scripts" / "test_qemu_ahci.py")
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)

TARGET_READ_TRACE = """handle_cmd_fis_dump ahci(0xabc)[1]: FIS:
0x00: 27 80 25 00 00 c0 00 40 00 00 00 00 08 00 00 00
0x10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

ide_bus_exec_cmd IDE exec cmd: bus 0x4; state 0x5; cmd 0x25
"""


class FixtureTests(unittest.TestCase):
    def test_header_and_pattern_vectors(self):
        header = smoke.header()
        self.assertEqual(len(header), 4096)
        self.assertEqual(header[:32], b"PEDIGREE-AHCI-SMOKE-v1".ljust(32, b"\0"))
        self.assertEqual(struct.unpack_from("<IIQ", header, 32), (1, 0x5A, 33554432))
        self.assertEqual(header[48:], bytes(4096 - 48))
        for offset, expected in ((0, 90), (1, 127), (255, 129), (256, 91),
                                 (4096, 74), (65535, 126), (65536, 91)):
            self.assertEqual(smoke.pattern(offset, 1, smoke.BASE_SEED), bytes([expected]))

    def test_expected_write_boundaries_preserve_neighbours(self):
        for offset, length in smoke.WRITE_RANGES:
            self.assertGreaterEqual(offset, smoke.HEADER_SIZE)
            self.assertLessEqual(offset + length, smoke.DISK_SIZE)
            self.assertEqual(offset % 512, 0)
            self.assertEqual(length % 512, 0)
            first = smoke.expected_chunk(offset - 1, 3, written=True)
            self.assertEqual(first[:1], smoke.pattern(offset - 1, 1, smoke.BASE_SEED))
            self.assertEqual(first[1:], smoke.pattern(offset, 2, smoke.WRITE_SEED))
            if offset + length < smoke.DISK_SIZE:
                last = smoke.expected_chunk(offset + length - 1, 2, written=True)
                self.assertEqual(last[:1], smoke.pattern(offset + length - 1, 1, smoke.WRITE_SEED))
                self.assertEqual(last[1:], smoke.pattern(offset + length, 1, smoke.BASE_SEED))

    def test_error_mode_changes_only_header_flag(self):
        normal = smoke.header()
        failure = smoke.header(inject_read_error=True)
        self.assertEqual([i for i in range(len(normal)) if normal[i] != failure[i]], [48])
        self.assertEqual(failure[48], 1)
        for offset, length in (*smoke.READ_RANGES, *smoke.WRITE_RANGES):
            self.assertFalse(offset <= smoke.ERROR_OFFSET < offset + length)

    def test_whole_fixture_and_untouched_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "scratch.img"
            smoke.create_fixture(path)
            with self.assertRaises(FileExistsError):
                smoke.create_fixture(path)
            with self.assertRaisesRegex(RuntimeError, "mismatch"):
                smoke.verify_fixture(path)
            with path.open("r+b") as stream:
                for offset, length in smoke.WRITE_RANGES:
                    stream.seek(offset)
                    stream.write(smoke.pattern(offset, length, smoke.WRITE_SEED))
            result = smoke.verify_fixture(path)
            self.assertEqual(result["verified_bytes"], 32 * 1024 * 1024)
            self.assertEqual(len(result["sha256"]), 64)
            with path.open("r+b") as stream:
                stream.seek(48)
                stream.write(b"\x01")
            self.assertEqual(smoke.verify_fixture(path, True)["verified_bytes"], smoke.DISK_SIZE)
            with self.assertRaisesRegex(RuntimeError, "byte 48"):
                smoke.verify_fixture(path)
            with path.open("r+b") as stream:
                stream.seek(48)
                stream.write(b"\0")
                stream.seek(4095)
                stream.write(b"\x01")
            with self.assertRaisesRegex(RuntimeError, "byte 4095"):
                smoke.verify_fixture(path)
            with path.open("r+b") as stream:
                stream.truncate(smoke.DISK_SIZE - 1)
            with self.assertRaisesRegex(RuntimeError, "size changed"):
                smoke.verify_fixture(path)


class EvidenceTests(unittest.TestCase):
    @staticmethod
    def complete_serial(count=27):
        return "\n".join("NOTICE: AHCI-SMOKE: PASS " + step +
                         (f" count={count}" if step == "interrupt-completions" else "")
                         for step in smoke.STEPS)

    def test_complete_requires_every_step_and_positive_interrupts(self):
        steps, count = smoke.serial_progress(self.complete_serial())
        self.assertEqual(tuple(steps), smoke.STEPS)
        self.assertEqual(count, 27)
        missing_stages = [self.complete_serial().replace("AHCI-SMOKE: PASS " + stage, "unrelated")
                          for stage in ("writes-and-sync", "range-rejection")]
        for text in [self.complete_serial(0), *missing_stages]:
            with self.assertRaisesRegex(RuntimeError, "lacks required"):
                smoke.serial_progress(text)

    def test_guest_failures_override_completion(self):
        for text in ("AHCI-SMOKE: FAIL checked disk sync", "PANIC: kernel error"):
            with self.assertRaisesRegex(RuntimeError, "guest failure"):
                smoke.serial_progress(self.complete_serial() + "\n" + text)

    def test_flush_trace_is_bound_to_scratch_port(self):
        trace = """100@1 handle_cmd_fis_dump ahci(0xabc)[0]: fis
100@1 ide_bus_exec_cmd IDE exec cmd: bus 0x2; state 0x3; cmd 0xea
100@1 handle_cmd_fis_dump ahci(0xabc)[1]: fis
100@1 ide_bus_exec_cmd IDE exec cmd: bus 0x4; state 0x5; cmd 0x35
100@1 ide_bus_exec_cmd IDE exec cmd: bus 0x4; state 0x5; cmd 0xea
100@1 handle_cmd_fis_dump ahci(0xabc)[1]: fis
100@1 ide_bus_exec_cmd IDE exec cmd: bus 0x4; state 0x5; cmd 0xe7
100@1 handle_cmd_fis_dump ahci(0xabc)[1]: fis
100@1 ide_bus_exec_cmd IDE exec cmd: bus 0x4; state 0x5; cmd 0xEA
"""
        self.assertEqual(smoke.trace_summary(trace), {
            "scratch_flush_commands": 2, "scratch_flush_opcodes": ["0xe7", "0xea"],
            "scratch_taskfile_errors": 0, "root_reads_after_scratch_error": 0})
        self.assertEqual(smoke.trace_summary("")["scratch_flush_commands"], 0)

    def test_qemu_root_is_snapshot_and_scratch_is_persistent(self):
        args = argparse.Namespace(qemu="qemu", cpus=4, iso=Path("/test/a,b.iso"),
                                  root_image=Path("/test/root.img"), inject_read_error=False)
        command = smoke.qemu_command(args, Path("/test/new"))
        self.assertIn("file=/test/root.img,format=raw,if=none,id=rootdisk,snapshot=on", command)
        self.assertIn("file=/test/new/scratch.img,format=raw,if=none,id=scratch,cache=writeback,iops_rd=100", command)
        self.assertIn("file=/test/a,,b.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on", command)
        self.assertIn("ide-hd,drive=rootdisk,bus=ahci.0", command)
        self.assertIn("ide-hd,drive=scratch,bus=ahci.1", command)
        self.assertEqual(command[command.index("-nic") + 1], "none")
        self.assertNotIn("-snapshot", command)

    def test_error_mode_requires_extra_steps_and_keeps_irq_requirement(self):
        with self.assertRaisesRegex(RuntimeError, "lacks required"):
            smoke.serial_progress(self.complete_serial(), True)
        error_serial = "\n".join("AHCI-SMOKE: PASS " + step for step in smoke.ERROR_STEPS)
        steps, count = smoke.serial_progress(self.complete_serial() + "\n" + error_serial, True)
        self.assertTrue(all(step in steps for step in smoke.ERROR_STEPS))
        self.assertEqual(count, 27)
        with self.assertRaisesRegex(RuntimeError, "lacks required"):
            smoke.serial_progress(self.complete_serial(0) + "\n" + error_serial, True)

    def test_taskfile_error_trace_uses_port_and_new_interrupt_bit(self):
        trace = """ahci_trigger_irq ahci(0xabc)[0]: trigger irq +TFES (0x40000000); irqstat: 0 --> 0; effective: 0
ahci_trigger_irq ahci(0xabc)[1]: trigger irq +PCS (0x00400000); irqstat: 0x40000000 --> 0x40400000; effective: 0x40400000
ahci_trigger_irq ahci(0xabc)[1]: trigger irq +TFES (0x40000000); irqstat: 0 --> 0x40000000; effective: 0x40000000
"""
        self.assertEqual(smoke.trace_summary(TARGET_READ_TRACE + trace)["scratch_taskfile_errors"], 1)
        self.assertEqual(smoke.trace_summary(trace)["scratch_taskfile_errors"], 0)

    def test_root_read_trace_must_follow_scratch_error(self):
        root_read = ("handle_cmd_fis_dump ahci(0xabc)[0]: fis\n"
                     "ide_bus_exec_cmd IDE exec cmd: bus 0x1; state 0x2; cmd 0x25\n")
        error = ("ahci_trigger_irq ahci(0xabc)[1]: trigger irq +TFES (0x40000000); "
                 "irqstat: 0 --> 0x40000000; effective: 0x40000000\n")
        self.assertEqual(smoke.trace_summary(root_read + error)["root_reads_after_scratch_error"], 0)
        self.assertEqual(smoke.trace_summary(root_read + TARGET_READ_TRACE + error + root_read)[
            "root_reads_after_scratch_error"], 1)
        self.assertEqual(smoke.trace_summary(root_read + TARGET_READ_TRACE + error + root_read.replace(
            "cmd 0x25", "cmd 0x35"))["root_reads_after_scratch_error"], 0)

    def test_bios_errors_and_root_reads_do_not_count_as_injected_error(self):
        error = ("ahci_trigger_irq ahci(0xabc)[1]: trigger irq +TFES (0x40000000); "
                 "irqstat: 0 --> 0x40000000; effective: 0x40000000\n")
        root_read = ("handle_cmd_fis_dump ahci(0xabc)[0]: FIS:\n"
                     "ide_bus_exec_cmd IDE exec cmd: bus 0x1; state 0x2; cmd 0x25\n")
        bios_identify = TARGET_READ_TRACE.replace("27 80 25", "27 80 a1").replace("cmd 0x25", "cmd 0xa1")
        boot = bios_identify + error + root_read * 527
        before = smoke.trace_summary(boot)
        self.assertEqual(before["scratch_taskfile_errors"], 0)
        self.assertEqual(before["root_reads_after_scratch_error"], 0)
        after = smoke.trace_summary(boot + TARGET_READ_TRACE + error + error + root_read)
        self.assertEqual(after["scratch_taskfile_errors"], 2)
        self.assertEqual(after["root_reads_after_scratch_error"], 1)

    def test_target_fis_requires_correct_lba_flags_port_and_dispatch(self):
        error = ("ahci_trigger_irq ahci(0xabc)[1]: trigger irq +TFES (0x40000000); "
                 "irqstat: 0 --> 0x40000000; effective: 0x40000000\n")
        invalid = [TARGET_READ_TRACE.replace("00 c0 00 40", "08 c0 00 40"),
                   TARGET_READ_TRACE.replace("00 c0 00 40 00", "00 c0 00 40 01"),
                   TARGET_READ_TRACE.replace("27 80 25", "27 00 25"),
                   TARGET_READ_TRACE.replace("[1]", "[0]"),
                   TARGET_READ_TRACE.replace("cmd 0x25", "cmd 0x35"),
                   TARGET_READ_TRACE.replace("0x00:", "0x10:")]
        for trace in invalid:
            with self.subTest(trace=trace):
                self.assertEqual(smoke.trace_summary(trace + error)["scratch_taskfile_errors"], 0)
        self.assertEqual(smoke.trace_summary(TARGET_READ_TRACE + invalid[0] + error)[
            "scratch_taskfile_errors"], 0)

    def test_blkdebug_matches_only_one_scratch_read_with_native_eio(self):
        config = configparser.ConfigParser()
        config.read_string(smoke.error_config())
        fields = {key: value.strip('"') for key, value in config["inject-error"].items()}
        self.assertEqual(fields, {"event": "read_aio", "iotype": "read", "errno": str(errno.EIO),
                                  "sector": "49152", "once": "on", "immediately": "off"})
        args = argparse.Namespace(qemu="qemu", cpus=1, iso=Path("/test/a.iso"),
                                  root_image=Path("/test/root.img"), inject_read_error=True)
        command = smoke.qemu_command(args, Path("/test/fault"))
        self.assertIn("file=/test/root.img,format=raw,if=none,id=rootdisk,snapshot=on", command)
        wrapped = [arg for arg in command if "blkdebug" in arg]
        self.assertEqual(len(wrapped), 1)
        self.assertIn("file.image.filename=/test/fault/scratch.img", wrapped[0])
        self.assertIn("file.config=/test/fault/blkdebug.conf", wrapped[0])
        self.assertIn("rerror=report", wrapped[0])
        self.assertNotIn("root.img", wrapped[0])

    @mock.patch.object(smoke.os, "killpg")
    def test_cleanup_only_owns_child_and_escalates_after_timeout(self, killpg):
        child = mock.Mock(pid=12345)
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired("qemu", 5), 0]
        smoke.stop_child(child)
        self.assertEqual(killpg.call_args_list,
                         [mock.call(12345, signal.SIGTERM), mock.call(12345, signal.SIGKILL)])
        self.assertEqual(child.wait.call_args_list, [mock.call(timeout=5), mock.call(timeout=5)])
        killpg.reset_mock()
        child.poll.return_value = 0
        smoke.stop_child(child)
        killpg.assert_not_called()

    def test_launch_failure_keeps_diagnostic_report(self):
        with tempfile.TemporaryDirectory() as temp:
            args = argparse.Namespace(run_dir=Path(temp) / "run", cpus=1, timeout=1,
                                      qemu="missing-qemu", iso=Path("a.iso"), root_image=Path("a.img"),
                                      inject_read_error=False)
            with mock.patch.object(smoke, "create_fixture"), mock.patch.object(
                    smoke.subprocess, "Popen", side_effect=FileNotFoundError("no qemu")):
                report = smoke.run(args)
            self.assertFalse(report["success"])
            self.assertEqual(report["error"], "no qemu")
            self.assertTrue((args.run_dir / "report.json").is_file())

    def test_abnormal_exit_cannot_pass_with_complete_markers(self):
        with tempfile.TemporaryDirectory() as temp:
            args = argparse.Namespace(run_dir=Path(temp) / "run", cpus=1, timeout=1,
                                      qemu="qemu", iso=Path("a.iso"), root_image=Path("a.img"),
                                      inject_read_error=False)
            child = mock.Mock(returncode=2)
            child.poll.return_value = 2

            def fake_launch(*unused, **kwargs):
                (args.run_dir / "serial.log").write_text(self.complete_serial())
                return child

            with mock.patch.object(smoke, "create_fixture"), mock.patch.object(
                    smoke.subprocess, "Popen", side_effect=fake_launch):
                report = smoke.run(args)
            self.assertFalse(report["success"])
            self.assertIn("exited abnormally: 2", report["error"])
            self.assertEqual(report["qemu_returncode"], 2)


if __name__ == "__main__":
    unittest.main()
