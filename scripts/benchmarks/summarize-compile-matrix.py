#!/usr/bin/env python3
"""Compare two complete, uninstrumented compiler matrix reports."""

import argparse
import json
import math
from pathlib import Path
import re
import statistics


CASES = [
    "tiny", "tiny-pipe", "preprocess", "syntax", "codegen", "assemble",
    "link", "full", "full-pipe",
]
ROUNDS = ("r1", "r2", "r3")
INPUTS = {"which.cc", "tiny.cc", "which.ii", "which.s", "which.o"}
METRICS = {
    "guest_wall_s": "total_us",
    "guest_user_s": "user_us",
    "guest_system_s": "system_us",
}
NOTES = [
    "Three samples per case are descriptive; ranges are observed minima and maxima, "
    "not confidence intervals or significance tests.",
    "Host wall time is the runner's gate-to-DONE interval; guest wall and CPU times "
    "are separately reported clocks and accounting. CPU controls are not used to normalize results.",
    "User/system times cover GCC and its reaped descendants, excluding the benchmark "
    "parent and independent kernel workers. Wall - (user + system) is not pure I/O wait.",
    "Pipe savings pair the same round within each OS: without-pipe minus with-pipe, "
    "and 100 * (without-pipe - with-pipe) / without-pipe. Negative savings mean slower with -pipe.",
    "Independent stage costs do not sum to a full build. Syntax work overlaps code generation; "
    "the stage commands are not measurements of pure CPU work.",
    "Zero Pedigree fault and context-switch counters are unsupported accounting, not absence "
    "of faults or switches: src/modules/subsys/posix/wait-syscalls.cc:121 initializes rusage "
    "to zero and fills only user/system CPU time.",
    "The historical Linux control has an extra boot disk. Startup is outside the measured "
    "phases; these comparisons do not establish IRQ causality.",
]


def expected_phases():
    phases = ["cpu-before"]
    for prefix, cases in (("warm", CASES), ("r1", CASES),
                          ("r2", list(reversed(CASES))), ("r3", CASES)):
        phases.extend(f"{prefix}-{case}" for case in cases)
    return phases + ["cpu-after"]


def number(value, label, positive=False, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label}: expected a number")
    if integer and not isinstance(value, int):
        raise ValueError(f"{label}: expected an integer")
    if not math.isfinite(value) or value < 0 or (positive and value == 0):
        raise ValueError(f"{label}: invalid value {value!r}")
    return value


def identities(report, label):
    stages = {"before": {}, "after": {}}
    for item in report["identities"]:
        stage = item["stage"]
        if stage == "output":
            continue
        if stage not in stages:
            raise ValueError(f"{label}: unexpected input stage {stage!r}")
        path = item["path"]
        if path not in INPUTS or path in stages[stage]:
            raise ValueError(f"{label}: unexpected or duplicate {stage} input {path!r}")
        size = number(item["bytes"], f"{label}: {path} size", integer=True)
        checksum = item["fnv1a64"]
        if not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-f]{16}", checksum):
            raise ValueError(f"{label}: invalid FNV identity for {path}")
        stages[stage][path] = {"bytes": size, "fnv1a64": checksum}
    if set(stages["before"]) != INPUTS or stages["before"] != stages["after"]:
        raise ValueError(f"{label}: missing or changed before/after input identities")
    return stages["before"]


def load_report(path, os_name):
    report = json.loads(path.read_text())
    if (report.get("result") != "PASS" or report.get("os") != os_name
            or report.get("mode") != "run" or report.get("instrumented") is not False
            or report.get("profile_phase") is not None or report.get("cpus") != 1):
        raise ValueError(f"{path}: expected a PASS, uninstrumented, one-CPU {os_name} run")
    if report.get("configuration") != {"mode": "run", "profile": "none"}:
        raise ValueError(f"{path}: unexpected guest configuration")
    expected = expected_phases()
    phases = report["phases"]
    if (report.get("expected_phases") != expected or len(phases) != 38
            or [phase["phase"] for phase in phases] != expected):
        raise ValueError(f"{path}: incomplete or unexpected phase order")
    for phase in phases:
        label = f"{path}: {phase['phase']}"
        if phase.get("gate_acknowledged") is not True:
            raise ValueError(f"{label}: missing gate acknowledgement")
        number(phase["host_wall_s"], f"{label} host wall", positive=True)
        metric = phase["metric"]
        if number(metric["rc"], f"{label} exit status", integer=True) != 0:
            raise ValueError(f"{label}: nonzero exit status")
        for field in METRICS.values():
            number(metric[field], f"{label} {field}", integer=True)
        number(metric["checksum"], f"{label} checksum", integer=True)
    return report, identities(report, str(path))


def stats(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values),
            "raw": values}


def times(phase):
    return {"host_wall_s": phase["host_wall_s"],
            **{name: phase["metric"][field] / 1_000_000 for name, field in METRICS.items()}}


def summarize(reports, inputs, paths):
    phases = {os_name: {phase["phase"]: phase for phase in report["phases"]}
              for os_name, report in reports.items()}
    controls = []
    for os_name in reports:
        for name in ("cpu-before", "cpu-after"):
            phase = phases[os_name][name]
            controls.append({"os": os_name, "phase": name, **times(phase),
                             "checksum": phase["metric"]["checksum"]})
    if len({control["checksum"] for control in controls}) != 1:
        raise ValueError("CPU before/after checksums differ within or across operating systems")

    cases = []
    for case in CASES:
        row = {"case": case}
        for os_name in reports:
            samples = [times(phases[os_name][f"{round_name}-{case}"]) for round_name in ROUNDS]
            row[os_name] = {name: stats([sample[name] for sample in samples])
                            for name in samples[0]}
        row["pedigree_over_linux_host_wall"] = (
            row["pedigree"]["host_wall_s"]["median"] / row["linux"]["host_wall_s"]["median"])
        cases.append(row)

    pairs = []
    for case in ("tiny", "full"):
        for os_name in reports:
            pair = {"case": case, "os": os_name}
            for metric, prefix in (("host_wall_s", ""), ("guest_wall_s", "guest_wall_")):
                plain = [times(phases[os_name][f"{round_name}-{case}"])[metric]
                         for round_name in ROUNDS]
                piped = [times(phases[os_name][f"{round_name}-{case}-pipe"])[metric]
                         for round_name in ROUNDS]
                if any(before == 0 for before in plain):
                    raise ValueError(f"{os_name}: {case} has zero {metric}; pipe percentage undefined")
                saved = [before - after for before, after in zip(plain, piped)]
                pair.update({f"{prefix}without_pipe_s": plain,
                             f"{prefix}with_pipe_s": piped, f"{prefix}savings_s": stats(saved),
                             f"{prefix}savings_percent": stats([100 * delta / before
                                                               for delta, before in zip(saved, plain)])})
            pairs.append(pair)
    return {"result": "PASS", "rounds": list(ROUNDS), "units": "seconds unless labeled percent",
            "input_identities": inputs, "cases": cases, "cpu_controls": controls,
            "pipe_pairs": pairs, "notes": NOTES,
            "reports": {os_name: {"path": str(paths[os_name].resolve()),
                                   **{key: report.get(key) for key in
                                      ("image", "qemu_version", "source_sha256", "linux_control_note")}}
                        for os_name, report in reports.items()}}


def span(value):
    return f"{value['median']:.3f} [{value['min']:.3f}, {value['max']:.3f}]"


def triple(values):
    return ", ".join(f"{value:.3f}" for value in values)


def markdown(summary):
    lines = ["# Compiler matrix comparison", "",
             "Times are seconds. Host cells show median [minimum, maximum]. "
             "Guest U/S/W cells show median reported user/system/wall time.", "",
             "| Case | Linux host | Pedigree host | Ped/Linux | Linux guest U/S/W | Pedigree guest U/S/W |",
             "|---|---:|---:|---:|---:|---:|"]
    for row in summary["cases"]:
        guests = [" / ".join(f"{row[os_name][name]['median']:.3f}" for name in
                              ("guest_user_s", "guest_system_s", "guest_wall_s"))
                  for os_name in ("linux", "pedigree")]
        lines.append(f"| {row['case']} | {span(row['linux']['host_wall_s'])} | "
                     f"{span(row['pedigree']['host_wall_s'])} | "
                     f"{row['pedigree_over_linux_host_wall']:.3f}x | {guests[0]} | {guests[1]} |")
    lines += ["", "## CPU controls", "",
              "| OS | Control | Host wall | Guest wall | Guest user | Guest system | Checksum |",
              "|---|---|---:|---:|---:|---:|---:|"]
    for row in summary["cpu_controls"]:
        lines.append(f"| {row['os']} | {row['phase']} | {row['host_wall_s']:.3f} | "
                     f"{row['guest_wall_s']:.3f} | {row['guest_user_s']:.3f} | "
                     f"{row['guest_system_s']:.3f} | {row['checksum']} |")
    lines += ["", "## Paired -pipe savings", "",
              "Same-round differences on each clock; positive values favor `-pipe`. "
              "Host wall includes serial gate overhead, which can change the direction of small differences.", "",
              "| Case | OS | Clock | Seconds saved | Percent saved | Seconds r1, r2, r3 | Percent r1, r2, r3 |",
              "|---|---|---|---:|---:|---|---|"]
    for row in summary["pipe_pairs"]:
        for clock, prefix in (("Host wall", ""), ("Guest wall", "guest_wall_")):
            saved = row[f"{prefix}savings_s"]
            percent = row[f"{prefix}savings_percent"]
            lines.append(f"| {row['case']} | {row['os']} | {clock} | {span(saved)} | "
                         f"{span(percent)} | {triple(saved['raw'])} | {triple(percent['raw'])} |")
    lines += ["", "## Raw measured triples", "", "Samples are ordered r1, r2, r3.", "",
              "| Case | OS | Host wall | Guest wall | Guest user | Guest system |",
              "|---|---|---|---|---|---|"]
    for row in summary["cases"]:
        for os_name in ("linux", "pedigree"):
            cells = " | ".join(triple(row[os_name][name]["raw"]) for name in
                               ("host_wall_s", "guest_wall_s", "guest_user_s", "guest_system_s"))
            lines.append(f"| {row['case']} | {os_name} | {cells} |")
    lines += ["", "## Interpretation", ""] + [f"- {note}" for note in NOTES]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--pedigree", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        paths = {"linux": args.linux, "pedigree": args.pedigree}
        reports, fixtures = {}, {}
        for os_name, path in paths.items():
            reports[os_name], fixtures[os_name] = load_report(path, os_name)
        if fixtures["linux"] != fixtures["pedigree"]:
            raise ValueError("Linux and Pedigree input identities do not match")
        summary = summarize(reports, fixtures["linux"], paths)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
        (args.output / "summary.md").write_text(markdown(summary))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"error: {error}\n")
    print(args.output / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
