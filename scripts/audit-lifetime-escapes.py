#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path


CLASSIFICATIONS = {
    "api-declaration",
    "api-definition",
    "async-transfer",
    "kernel-admitted-wrapper",
    "kernel-root-thread",
    "legacy-async-transfer",
    "lexical-migration-debt",
    "lexical-owner",
    "mixed-reviewed",
    "module-thread-publication",
    "non-thread-api",
    "thread-publication",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Reject growth in inventoried manual lifetime boundaries."
    )
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    return parser.parse_args()


def matching_files(root, rule):
    suffixes = set(rule["suffixes"])
    exclusions = tuple(rule.get("exclude_substrings", []))
    for source_root in rule["roots"]:
        for path in (root / source_root).rglob("*"):
            if path.suffix not in suffixes:
                continue
            relative = path.relative_to(root).as_posix()
            if any(exclusion in relative for exclusion in exclusions):
                continue
            yield relative, path


def validate_inventory(root, inventory):
    failures = []
    if not isinstance(inventory, dict):
        return ["inventory must be an object"]
    if inventory.get("version") != 1:
        failures.append("inventory version must be 1")

    rules = inventory.get("rules")
    if not isinstance(rules, list) or not rules:
        return [*failures, "inventory must contain a non-empty rules list"]

    identifiers = set()
    for rule in rules:
        if not isinstance(rule, dict):
            failures.append("every rule must be an object")
            continue
        identifier = rule.get("id")
        if not isinstance(identifier, str) or not identifier:
            failures.append("every rule needs a non-empty id")
            continue
        if identifier in identifiers:
            failures.append(f"duplicate rule id: {identifier}")
        identifiers.add(identifier)

        pattern = rule.get("pattern")
        try:
            if not isinstance(pattern, str):
                raise TypeError("pattern must be a string")
            re.compile(pattern)
        except (TypeError, re.error) as error:
            failures.append(f"{identifier}: invalid pattern: {error}")

        roots = rule.get("roots")
        suffixes = rule.get("suffixes")
        allowances = rule.get("allowances")
        if not isinstance(roots, list) or not roots:
            failures.append(f"{identifier}: roots must be a non-empty list")
            continue
        if (not isinstance(suffixes, list) or not suffixes or
                any(not isinstance(suffix, str) or not suffix for suffix in suffixes)):
            failures.append(f"{identifier}: suffixes must be a non-empty list")
            continue
        if not isinstance(allowances, dict):
            failures.append(f"{identifier}: allowances must be an object")
            continue

        exclusion_values = rule.get("exclude_substrings", [])
        if (not isinstance(exclusion_values, list) or
                any(not isinstance(value, str) for value in exclusion_values)):
            failures.append(f"{identifier}: exclude_substrings must be a list of strings")
            continue
        exclusions = tuple(exclusion_values)
        for source_root in roots:
            if not isinstance(source_root, str) or not source_root:
                failures.append(f"{identifier}: every source root must be a string")
                continue
            if not (root / source_root).is_dir():
                failures.append(f"{identifier}: missing source root: {source_root}")

        for relative, allowance in allowances.items():
            if not isinstance(relative, str) or not relative:
                failures.append(f"{identifier}: allowance paths must be strings")
                continue
            if not isinstance(allowance, dict):
                failures.append(f"{identifier}: allowance must be an object: {relative}")
                continue
            path = root / relative
            if not path.is_file():
                failures.append(f"{identifier}: stale path: {relative}")
            if path.suffix not in suffixes:
                failures.append(f"{identifier}: disallowed suffix: {relative}")
            if not any(relative == source_root or relative.startswith(source_root + "/")
                       for source_root in roots):
                failures.append(f"{identifier}: path is outside rule roots: {relative}")
            if any(exclusion in relative for exclusion in exclusions):
                failures.append(f"{identifier}: allowance is excluded from scan: {relative}")

            expected = allowance.get("expected")
            if not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0:
                failures.append(f"{identifier}: invalid expected count for {relative}")
            classification = allowance.get("classification")
            if classification not in CLASSIFICATIONS:
                failures.append(
                    f"{identifier}: invalid classification for {relative}: {classification}"
                )
            owner = allowance.get("owner")
            if owner is not None and (not isinstance(owner, str) or not owner):
                failures.append(f"{identifier}: invalid owner for {relative}")

    return failures


def audit_rule(root, rule):
    pattern = re.compile(rule["pattern"])
    allowances = rule["allowances"]
    actual = {}
    failures = []

    for relative, path in matching_files(root, rule):
        count = len(pattern.findall(path.read_text(encoding="latin-1")))
        if count:
            actual[relative] = count

    for relative in sorted(set(actual) | set(allowances)):
        count = actual.get(relative, 0)
        allowance = allowances.get(relative)
        if allowance is None:
            failures.append(f"new site: {relative} ({count})")
            continue
        expected = allowance["expected"]
        if count != expected:
            failures.append(f"count changed: {relative} ({count} != {expected})")

    budget = sum(entry["expected"] for entry in allowances.values())
    print(f"{rule['id']}: {sum(actual.values())}/{budget}")
    return failures


def main():
    args = parse_args()
    root = args.root.resolve()
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))

    failures = validate_inventory(root, inventory)
    if failures:
        print("Invalid lifetime escape inventory:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    for rule in inventory["rules"]:
        rule_failures = audit_rule(root, rule)
        failures.extend(f"{rule['id']}: {failure}" for failure in rule_failures)

    if failures:
        print("Lifetime escape inventory changed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print(
            "Wrap or transfer the resource, or add a reviewed inventory entry.",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
