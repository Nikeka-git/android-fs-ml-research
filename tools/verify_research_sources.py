"""Verify methodology artifacts and, optionally, pinned public parser texts.

Python 3.12+, standard library only. Input texts are never executed or saved.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DATA_MANIFEST = ROOT / "data/diagnostic-texts/manifest.json"
ARTIFACT_MANIFEST = ROOT / "evidence/methodology/assignment3-manifest.json"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verify_bytes(data, expected, label):
    require(len(data) == expected["bytes"], f"Size mismatch: {label}")
    require(sha256(data) == expected["sha256"], f"SHA-256 mismatch: {label}")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def verify(fetch=False):
    manifest = read_json(DATA_MANIFEST)
    artifacts = read_json(ARTIFACT_MANIFEST)
    require(manifest["schema_version"] == 1, "Unsupported data schema")
    require(artifacts["schema_version"] == 1, "Unsupported artifact schema")
    revision = manifest["upstream_revision"]
    require(re.fullmatch(r"[0-9a-f]{40}", revision), "Invalid pinned revision")
    require(manifest["upstream_repository"] == "google/syzkaller", "Unexpected upstream")
    base = f"https://raw.githubusercontent.com/google/syzkaller/{revision}/pkg/report/testdata/linux/report/"
    records = manifest["records"]
    require(bool(records), "Empty source manifest")
    ids = [record["report_id"] for record in records]
    require(all(type(report_id) is int and report_id >= 0 for report_id in ids), "Invalid report ID")
    require(len(ids) == len(set(ids)), "Duplicate report ID")
    labels = Counter()
    for record in records:
        report_id = record["report_id"]
        require(record["source_url"] == base + str(report_id), f"Unpinned URL: {report_id}")
        require(type(record["bytes"]) is int and record["bytes"] > 0, f"Invalid size: {report_id}")
        require(re.fullmatch(r"[0-9a-f]{64}", record["sha256"]), f"Invalid hash: {report_id}")
        require(record["reference_label"] in {"ext4", "F2FS", "none"}, f"Invalid label: {report_id}")
        require(record["partition"] is None, "Partition needs a separately reviewed manifest")
        require(record["annotation_status"] == "reported_not_independently_reviewed", "Unexpected annotation status")
        require(record["evidence_level"] == "public_parser_fixture", "Unexpected evidence level")
        labels[record["reference_label"]] += 1
    summary = manifest["summary"]
    require(len(records) == summary["file_count"], "Incorrect file count")
    require(sum(record["bytes"] for record in records) == summary["total_bytes"], "Incorrect byte total")
    require(dict(labels) == summary["reference_label_counts"], "Incorrect label counts")
    require(manifest["annotation_source"] == artifacts["artifacts"][0]["repo_path"], "Annotation source mismatch")

    for artifact in artifacts["artifacts"]:
        path = (ROOT / artifact["repo_path"]).resolve()
        require(path.is_relative_to(ROOT), "Artifact path outside repository")
        verify_bytes(path.read_bytes(), artifact, artifact["repo_path"])
    duplicates = sum(count - 1 for count in Counter(record["sha256"] for record in records).values())
    print(f"Offline checks passed: {len(records)} source records; {len(artifacts['artifacts'])} local artifacts; {duplicates} exact duplicates.")
    if fetch:
        for record in records:
            request = Request(record["source_url"], headers={"User-Agent": "research-source-verifier/1"})
            with urlopen(request, timeout=30) as response:
                require(response.status == 200, f"HTTP failure: {record['report_id']}")
                require(response.geturl() == record["source_url"], "Unexpected source redirect")
                data = response.read(record["bytes"] + 1)
            verify_bytes(data, record, f"report {record['report_id']}")
        print(f"Upstream checks passed: {len(records)}/{len(records)} files; {summary['total_bytes']} bytes; all SHA-256 values matched.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="Also verify public source bytes over HTTPS")
    args = parser.parse_args()
    try:
        verify(fetch=args.fetch)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
