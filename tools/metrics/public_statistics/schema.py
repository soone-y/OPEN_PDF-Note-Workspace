"""Closed public projection: no source excerpts, symbols, paths or raw logs."""
from __future__ import annotations

import re
from datetime import date, datetime


def fields(value, names):
    if not isinstance(value, dict) or set(value) != set(names.split()):
        raise ValueError("unrecognized public statistics fields")


def integer(value, nullable=False):
    if nullable and value is None:
        return
    if type(value) is not int or value < 0:
        raise ValueError("invalid public aggregate count")


def sha(value, nullable=False):
    if nullable and value is None:
        return
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("invalid public aggregate identity")


def stamp(value, nullable=False):
    if nullable and value is None:
        return
    if not isinstance(value, str) or datetime.fromisoformat(value.replace("Z", "+00:00")).tzinfo is None:
        raise ValueError("statistics timestamp must have a timezone")


def validate_record_fields(record):
    release = record["release"]
    fields(release, "version published_at public_url tag")
    stamp(release["published_at"], True)
    if release["tag"] is not None and not re.fullmatch(r"v[0-9]+(?:\.[0-9A-Za-z_-]+)+", release["tag"]):
        raise ValueError("invalid public tag")
    if release["public_url"] is not None and not re.fullmatch(r"https://github\.com/soone-y/OPEN_PDF-Note-Workspace/releases/tag/v[0-9A-Za-z._-]+", release["public_url"]):
        raise ValueError("unreviewed public release URL")
    measurement = record["measurement"]
    development = "observed_on" in measurement
    fields(measurement, "profile_key measured_at source_kind input_sha256 allowlist_sha256" + (" observed_on" if development else ""))
    if development:
        day = measurement["observed_on"]
        if not isinstance(day, str) or date.fromisoformat(day).isoformat() != day:
            raise ValueError("invalid development observation date")
        if measurement["source_kind"] != "private-history-allowlisted" or measurement["allowlist_sha256"] is None:
            raise ValueError("development history requires allowlisted private input")
        if any(value is not None for value in release.values()) or record["packages"]:
            raise ValueError("development history must not expose private versions or release metadata")
    stamp(measurement["measured_at"])
    for name in ("profile_key", "input_sha256", "allowlist_sha256"):
        sha(measurement[name], name == "allowlist_sha256")
    if measurement["source_kind"] not in {"public-snapshot", "private-history-allowlisted", "release-candidate"}:
        raise ValueError("unreviewed statistics source kind")
    inv = record["inventory"]
    fields(inv, "status files size_bytes text_files binary_files lines blank_lines legacy_text_files groups extensions")
    if inv["status"] != "COMPLETE":
        raise ValueError("physical inventory must complete")
    for name in ("files", "size_bytes", "text_files", "binary_files", "lines", "blank_lines", "legacy_text_files"):
        integer(inv[name])
    if inv["text_files"] + inv["binary_files"] != inv["files"] or inv["blank_lines"] > inv["lines"]:
        raise ValueError("invalid text inventory denominator")
    for value in inv["groups"].values():
        fields(value, "files size_bytes")
    for extension, value in inv["extensions"].items():
        if not re.fullmatch(r"\.[a-z0-9_+-]{1,30}|<none>", extension):
            raise ValueError("invalid extension aggregate")
        fields(value, "files size_bytes")
        integer(value["files"]); integer(value["size_bytes"])
    for name in ("files", "size_bytes"):
        if sum(value[name] for value in inv["extensions"].values()) != inv[name]:
            raise ValueError("extension aggregate denominator mismatch")
    scale = record["source_scale"]
    fields(scale, "status application tests_source tools" + (" src_tree" if "src_tree" in scale else ""))
    if development and "src_tree" not in scale:
        raise ValueError("development history requires src tree size")
    if "src_tree" in scale:
        fields(scale["src_tree"], "files size_bytes")
        for value in scale["src_tree"].values(): integer(value)
        if any(scale["src_tree"][key] > inv[key] for key in ("files", "size_bytes")):
            raise ValueError("src tree exceeds inventory denominator")
    if scale["status"] != "COMPLETE":
        raise ValueError("source measurement must complete")
    for category in ("application", "tests_source", "tools"):
        fields(scale[category], "files text_files size_bytes lines blank_lines code_lines comment_lines classified_files")
        for value in scale[category].values():
            integer(value)
        if scale[category]["classified_files"] > scale[category]["text_files"] or scale[category]["text_files"] > scale[category]["files"]:
            raise ValueError("invalid source classification coverage")
        if sum(scale[category][key] for key in ("code_lines", "comment_lines", "blank_lines")) > scale[category]["lines"]:
            raise ValueError("invalid source line partition")
    lex = record["lexical"]
    fields(lex, "status files source_bytes bytes literal_bytes literal_counts lexical_items string_payload_source_bytes string_purpose")
    if lex["status"] not in {"COMPLETE", "NOT_APPLICABLE"} or lex["string_purpose"] != "UNCLASSIFIED":
        raise ValueError("invalid lexical scope")
    for name in ("files", "source_bytes", "lexical_items", "string_payload_source_bytes"):
        integer(lex[name])
    for name, keys in (("bytes", "code literal comment whitespace"), ("literal_bytes", "string character number other"), ("literal_counts", "string character number other")):
        fields(lex[name], keys)
        for value in lex[name].values(): integer(value)
    if sum(lex["bytes"].values()) != lex["source_bytes"] or sum(lex["literal_bytes"].values()) != lex["bytes"]["literal"] or lex["string_payload_source_bytes"] > lex["literal_bytes"]["string"]:
        raise ValueError("invalid literal byte partition")
    loc = record["localization"]
    fields(loc, "status ja_ids en_ids en_missing_ids")
    if loc["status"] not in {"COMPLETE", "UNMEASURED"}: raise ValueError("invalid locale status")
    for name in ("ja_ids", "en_ids", "en_missing_ids"): integer(loc[name], loc["status"] == "UNMEASURED")
    if loc["status"] == "COMPLETE" and loc["en_missing_ids"] > loc["ja_ids"]: raise ValueError("invalid locale denominator")
    seen = set()
    for package in record["packages"]:
        fields(package, "status locale edition sha256 zip_bytes unpacked_bytes file_count basis")
        key = (package["locale"], package["edition"])
        if key in seen or key[0] not in {"ja", "en"} or key[1] not in {"full", "lite"}: raise ValueError("duplicate or invalid package variant")
        seen.add(key)
        if package["status"] not in {"COMPLETE", "PARTIAL"} or package["basis"] not in {"archive-bytes", "published-asset-metadata"}: raise ValueError("invalid package measurement basis")
        sha(package["sha256"]); integer(package["zip_bytes"])
        integer(package["unpacked_bytes"], package["status"] == "PARTIAL")
        integer(package["file_count"], package["status"] == "PARTIAL")
    fields(record["verification"], "status reason suites")
    if record["verification"] != {"status": "UNMEASURED", "reason": "no-bound-public-verification-receipt", "suites": []}:
        raise ValueError("verification evidence requires a supported bound receipt adapter")
    if record["supplemental"] != {"status": "UNMEASURED", "reason": "not-selected"}:
        raise ValueError("supplemental analysis requires a supported receipt adapter")
    comparison = record["comparison"]
    if set(comparison) not in ({"status", "reason", "baseline_record_id"}, {"status", "reason", "baseline_record_id", "application_lines_delta"}): raise ValueError("invalid comparison fields")
    if comparison["status"] not in {"COMPLETE", "UNMEASURED", "NOT_APPLICABLE"} or comparison["reason"] not in {None, "no-compatible-baseline", "profile-or-source-kind-changed"}: raise ValueError("invalid comparison status")
    sha(comparison["baseline_record_id"], True)
    if comparison.get("application_lines_delta") is not None and type(comparison["application_lines_delta"]) is not int: raise ValueError("invalid comparison delta")
