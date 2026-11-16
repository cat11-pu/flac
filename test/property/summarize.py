#!/usr/bin/env python3
"""Write the machine-readable and Markdown summaries for the property run."""

import argparse
import csv
import json
import shutil
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path


OK_STATUSES = {"accepted", "recovered", "rejected"}


def read_tsv(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def read_key_value_tsv(path):
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if separator:
            values[key] = value
    return values


def as_int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def copy_counterexamples(report, malformed, malformed_rows):
    target = report / "counterexamples"
    rejected_target = target / "rejected"
    failures_target = target / "failures"
    if target.exists():
        shutil.rmtree(target)
    rejected_target.mkdir(parents=True)
    index_rows = []

    failures = [row for row in malformed_rows if row["status"] not in OK_STATUSES]
    for row in failures:
        failures_target.mkdir(exist_ok=True)
        source = Path(row["path"])
        destination = failures_target / source.name
        shutil.copy2(source, destination)
        index_rows.append(
            {
                "kind": "failure",
                "category": row["category"],
                "variant": row["variant"],
                "file": str(destination.relative_to(report)),
                "size": row["size"],
                "offset": row.get("reported_offset", ""),
                "detail": row.get("failure_kind", ""),
            }
        )

    if not failures:
        selected = {}
        fallback = {}
        for row in malformed_rows:
            fallback.setdefault(row["category"], row)
            if row["status"] == "rejected":
                selected.setdefault(row["category"], row)
        for category, row in fallback.items():
            selected.setdefault(category, row)
        for category, row in sorted(selected.items()):
            source = Path(row["path"])
            destination = rejected_target / f"{category}_{row['variant']}_{source.name}"
            shutil.copy2(source, destination)
            if row["status"] in ("rejected", "recovered"):
                kind = "representative_rejection"
                detail = (
                    f"{row['status']}; state={row.get('state', '-')}; "
                    f"first_error={row.get('first_error', '-')}; "
                    f"offset={row.get('reported_offset', '-')}"
                )
            else:
                kind = "representative_semantic_illegal"
                detail = (
                    f"accepted by stream decoder; state={row.get('state', '-')}; "
                    f"semantic_illegal={row.get('semantic_illegal', 'NA')}"
                )
            index_rows.append(
                {
                    "kind": kind,
                    "category": category,
                    "variant": row["variant"],
                    "file": str(destination.relative_to(report)),
                    "size": row["size"],
                    "offset": row.get("reported_offset", ""),
                    "detail": detail,
                }
            )

    with (target / "index.tsv").open("w", newline="", encoding="utf-8") as handle:
        fields = ("kind", "category", "variant", "file", "size", "offset", "detail")
        writer = csv.DictWriter(handle, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(index_rows)
    return index_rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    args.report.mkdir(parents=True, exist_ok=True)
    malformed_rows = read_tsv(args.work / "malformed_results.tsv")
    valid_rows = read_tsv(args.work / "valid_results.tsv")
    valid_manifest = read_tsv(args.work / "valid" / "manifest.tsv")
    metadata = read_key_value_tsv(args.report / "metadata.tsv")

    okay_rows = [row for row in malformed_rows if row["status"] in OK_STATUSES]
    failures = [row for row in malformed_rows if row["status"] not in OK_STATUSES]
    outcome_counts = Counter(row["status"] for row in malformed_rows)
    category_counts = Counter(row["category"] for row in malformed_rows)
    category_outcomes = defaultdict(Counter)
    category_semantic_illegal = Counter()
    for row in malformed_rows:
        category_outcomes[row["category"]][row["status"]] += 1
        if as_int(row.get("semantic_illegal")) > 0:
            category_semantic_illegal[row["category"]] += 1

    failure_kinds = Counter(row.get("failure_kind", "") for row in failures)
    sanitizer_failures = {
        kind: failure_kinds.get(kind, 0)
        for kind in ("oob", "leak", "ub", "crash")
    }

    offset_breakdown = {}
    for category in sorted(category_counts):
        offsets = [
            as_int(row.get("reported_offset"))
            for row in malformed_rows
            if row["category"] == category
            and row["status"] in ("rejected", "recovered")
            and row.get("reported_offset", "").isdigit()
        ]
        offset_breakdown[category] = {
            "observations": len(offsets),
            "min": min(offsets) if offsets else None,
            "max": max(offsets) if offsets else None,
        }

    valid_pass = [row for row in valid_rows if row["status"] == "pass"]
    valid_fail = [row for row in valid_rows if row["status"] != "pass"]
    frames_compared = sum(as_int(row.get("samples")) for row in valid_pass)
    samples_compared = sum(
        as_int(row.get("samples")) * as_int(row.get("channels"), 1) for row in valid_pass
    )
    bps_coverage = Counter(row["bps"] for row in valid_manifest)
    channel_coverage = Counter(row["channels"] for row in valid_manifest)
    rate_coverage = Counter(row["rate"] for row in valid_manifest)

    counterexamples = copy_counterexamples(args.report, args.work / "malformed", malformed_rows)

    malformed_pass = len(okay_rows) == len(malformed_rows)
    valid_all_pass = len(valid_pass) == len(valid_rows)
    overall_pass = malformed_pass and valid_all_pass and not failures

    summary = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "overall_pass": overall_pass,
        "sanitizer": metadata.get("sanitizer", "unknown"),
        "ffmpeg_version": metadata.get("ffmpeg_version", "unknown"),
        "git_commit": metadata.get("git_commit", "unknown"),
        "malformed": {
            "total": len(malformed_rows),
            "completed": len(okay_rows),
            "failures": len(failures),
            "outcomes": dict(sorted(outcome_counts.items())),
            "category_counts": dict(sorted(category_counts.items())),
            "category_outcomes": {
                category: dict(sorted(counts.items()))
                for category, counts in sorted(category_outcomes.items())
            },
            "sanitizer_failures": sanitizer_failures,
            "timeouts": outcome_counts.get("timeout", 0),
            "invariants": outcome_counts.get("invariant", 0),
            "offset_breakdown": offset_breakdown,
        },
        "valid": {
            "total": len(valid_rows),
            "pass": len(valid_pass),
            "fail": len(valid_fail),
            "frames_compared": frames_compared,
            "interleaved_samples_compared": samples_compared,
            "bps_coverage": dict(sorted(bps_coverage.items())),
            "channel_coverage": dict(sorted(channel_coverage.items())),
            "rate_coverage": dict(sorted(rate_coverage.items())),
        },
        "counterexamples": counterexamples,
    }

    (args.report / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    lines = [
        "# FLAC decoder property test",
        "",
        f"- Generated: `{summary['generated_at']}`",
        f"- Git commit: `{summary['git_commit']}`",
        f"- Sanitizers: `{summary['sanitizer']}`",
        f"- Reference decoder: `{summary['ffmpeg_version']}`",
        f"- Overall: **{'PASS' if overall_pass else 'FAIL'}**",
        "",
        "## Malformed corpus",
        "",
        f"- Cases: **{len(malformed_rows)}**",
        f"- Crashes: **{sanitizer_failures['crash']}**",
        f"- Out-of-bounds: **{sanitizer_failures['oob']}**",
        f"- Leaks: **{sanitizer_failures['leak']}**",
        f"- Undefined behavior: **{sanitizer_failures['ub']}**",
        f"- Timeouts: **{outcome_counts.get('timeout', 0)}**",
        f"- Harness invariant failures: **{outcome_counts.get('invariant', 0)}**",
        f"- Other harness failures: **{outcome_counts.get('harness_failure', 0)}**",
        "",
        "| Category | Cases | Rejected | Recovered | Accepted | Semantic illegal | Offset observations | Offset min | Offset max |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for category in sorted(category_counts):
        outcomes = category_outcomes[category]
        offsets = offset_breakdown[category]
        lines.append(
            f"| {category} | {category_counts[category]} | {outcomes.get('rejected', 0)} | "
            f"{outcomes.get('recovered', 0)} | {outcomes.get('accepted', 0)} | "
            f"{category_semantic_illegal.get(category, 0)} | "
            f"{offsets['observations']} | {offsets['min'] if offsets['min'] is not None else '-'} | "
            f"{offsets['max'] if offsets['max'] is not None else '-'} |"
        )

    lines.extend(
        [
            "",
            "`rejected` means the stream decoder returned false. `recovered` means it "
            "reported at least one recoverable error and reached end-of-stream. "
            "`accepted` means the container decoded; semantic metadata checks can "
            "still mark it illegal.",
            "",
            "## Valid differential corpus",
            "",
            f"- Files: **{len(valid_rows)}**",
            f"- Exact sample matches: **{len(valid_pass)}/{len(valid_rows)}**",
            f"- Frames compared: **{frames_compared}**",
            f"- Interleaved samples compared: **{samples_compared}**",
            f"- Bit depths: `{dict(sorted(bps_coverage.items()))}`",
            f"- Channel counts: `{dict(sorted(channel_coverage.items()))}`",
            f"- Sample rates: `{dict(sorted(rate_coverage.items()))}`",
            "",
            "## Counterexamples",
            "",
        ]
    )
    if counterexamples:
        lines.append("| Kind | Category | Variant | File | Offset | Detail |")
        lines.append("|---|---|---|---|---:|---|")
        for row in counterexamples:
            lines.append(
                f"| {row['kind']} | {row['category']} | {row['variant']} | "
                f"`{row['file']}` | {row['offset']} | {row['detail']} |"
            )
    else:
        lines.append("No failures; no counterexamples were emitted.")

    lines.extend(
        [
            "",
            "The full generated corpus and per-case logs are under the work directory. "
            "Offset values are byte offsets from the start of the FLAC stream and come "
            "from `FLAC__stream_decoder_get_decode_position()` at the first error, or "
            "from the sanitizer death callback.",
            "",
        ]
    )
    (args.report / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")

    print(f"report: {args.report / 'SUMMARY.md'}")
    print(f"summary: {args.report / 'summary.json'}")
    return 0 if overall_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
