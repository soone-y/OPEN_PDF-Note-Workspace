from __future__ import annotations

import copy
import json
import tempfile
import tarfile
import hashlib
import io
import shutil
import subprocess
import os
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from tools.metrics.public_statistics import cli, model
from tools.metrics.public_statistics.collect import aggregate
from tools.metrics.public_statistics.packages import measure
from tools.metrics.public_statistics.render import render
from tools.metrics.backfill_public_statistics import backfill
from tools.release_checks import prepared_release_packages as preparation
from tools.release_checks import public_statistics_gate
from tools.release_checks.public_snapshot_inventory import file_records, read_inventory, tree_sha256, write_new_json

REPO_ROOT = Path(__file__).resolve().parents[2]


class StatisticsFixture(unittest.TestCase):
    def setUp(self):
        base = REPO_ROOT / "out/tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="public_stats_", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.snapshot = self.root / "snapshot"
        self.snapshot.mkdir()
        files = {"REPO_VERSION.txt": "1.2.3\n", "README.md": "説明\n", "src/main.cpp": '\ufeff// comment\nconst char* private_symbol = R"x(非公開の具体的な本文)x";\n\nint main(){ return 12; }\n',
                 "tests/unit/check.cpp": "// test\nint test(){return 0;}\n", "docs/ja/Guide.md": "自然言語の本文\n",
                 "locales/ja.json": '{"a":"日本語","b":"未翻訳"}', "locales/en.json": '{"a":"English"}'}
        for name, value in files.items():
            path = self.snapshot / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value, encoding="utf-8", newline="")
        self.seed = self.root / "seed.json"
        self.document = {"schema_version": 1, "profiles": {}, "current": None, "history": []}
        write_new_json(self.seed, self.document)
        cli.write_views(self.snapshot, self.document, exclusive=True)
        self.receipt_path = self.root / "inventory.json"
        self.receipt = self.inventory()
        write_new_json(self.receipt_path, self.receipt)
        self.packages = []
        self.package_paths = []
        for locale in ("ja", "en"):
            for edition in ("full", "lite"):
                path = self.root / f"pdf_note_workspace_1.2.3_{locale}_{edition}.zip"
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr("PDF-Note-Workspace-1.2.3/pdf_note_workspace.exe.buildinfo.txt", f"version\t1.2.3\nlocale\t{locale}\nedition\t{edition}\n")
                    archive.writestr("PDF-Note-Workspace-1.2.3/app.bin", b"payload")
                self.packages.append(measure(path, locale, edition, "1.2.3"))
                self.package_paths.append(path)
        self.packages.sort(key=lambda row: (row["locale"], row["edition"]))

    def inventory(self):
        rows = file_records(self.snapshot)
        return {"schema_version": 1, "allowlist_sha256": "a" * 64, "files": rows, "tree_sha256": tree_sha256(rows)}

    def record(self, kind="public-snapshot"):
        return aggregate(self.snapshot, self.inventory(), version="1.2.3", measured_at="2026-10-27T00:00:00+00:00", source_kind=kind)

    def generated(self):
        return cli.generate(self.snapshot, self.receipt_path, self.seed, self.packages, ["ja", "en"], "2026-10-27T00:00:00+00:00")


class PublicStatisticsTests(StatisticsFixture):
    def test_raw_bytes_lines_literal_partition_and_no_source_excerpts(self):
        record, active = self.record("private-history-allowlisted")
        document = {**self.document, "profiles": {model.digest(active): active}, "history": [record]}
        model.validate_document(document)
        self.assertEqual(record["source_scale"]["application"]["lines"], 4)
        self.assertEqual(record["inventory"]["groups"]["documents"]["files"], 2)
        self.assertEqual(record["localization"]["en_missing_ids"], 1)
        self.assertEqual(sum(record["lexical"]["bytes"].values()), (self.snapshot / "src/main.cpp").stat().st_size)
        self.assertGreater(record["lexical"]["literal_bytes"]["string"], 0)
        public = json.dumps(document, ensure_ascii=False) + "".join(render(document))
        self.assertNotIn("非公開の具体的な本文", public)
        self.assertNotIn("private_symbol", public)
        self.assertNotIn("src/main.cpp", public)
        self.assertNotIn(str(self.root), public)

    def test_generation_verification_is_read_only_and_excludes_own_outputs(self):
        document = self.generated()
        before = file_records(self.snapshot)
        cli.verify(self.snapshot, self.packages, ["ja", "en"])
        self.assertEqual(before, file_records(self.snapshot))
        self.assertEqual(document["current"]["inventory"]["files"], 7)
        with self.assertRaisesRegex(ValueError, "history seed identity"):
            cli.generate(self.snapshot, self.receipt_path, self.seed, self.packages, ["ja", "en"], "2026-10-27T00:00:00+00:00")

    def test_candidate_input_changes_are_rejected(self):
        for name, content in (("src/main.cpp", "changed"), ("src/extra.h", "new")):
            with self.subTest(name=name):
                path = self.snapshot / name
                old = path.read_bytes() if path.exists() else None
                path.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "differ"):
                    cli.generate(self.snapshot, self.receipt_path, self.seed, self.packages, ["ja", "en"], "2026-10-27T00:00:00+00:00")
                if old is None: path.unlink()
                else: path.write_bytes(old)

    def test_missing_extra_packages_and_frozen_or_live_roots_fail(self):
        with self.assertRaisesRegex(ValueError, "variants"):
            cli.generate(self.snapshot, self.receipt_path, self.seed, self.packages[:2], ["ja", "en"], "2026-10-27T00:00:00+00:00")
        for path in (self.snapshot / ".git", self.root / "public_snapshot_manifest.json"):
            path.write_text("frozen", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unfrozen"):
                self.generated()
            path.unlink()

    def test_closed_schema_rejects_private_content_even_with_recomputed_identity(self):
        document = self.generated()
        for mutate in (lambda r: r["inventory"].update(source_text="private"), lambda r: r["release"].update(private_commit="1" * 40),
                       lambda r: r["verification"].update(reason="C:/" + "Users/" + "private/log"), lambda r: r["source_scale"]["application"].update(symbol="private_function")):
            with self.subTest(mutate=mutate):
                value = copy.deepcopy(document)
                record = value["current"]
                mutate(record)
                record["record_id"] = model.digest({k: v for k, v in record.items() if k not in {"record_id", "comparison"}})
                with self.assertRaises(ValueError): model.validate_document(value)

    def test_tampered_markdown_and_aggregates_fail(self):
        document = self.generated()
        path = self.snapshot / "STATISTICS.md"
        original = path.read_bytes()
        path.write_bytes(original + b"tamper")
        with self.assertRaisesRegex(ValueError, "Markdown"):
            cli.verify(self.snapshot, self.packages, ["ja", "en"])
        path.write_bytes(original)
        record = document["current"]
        record["inventory"]["files"] += 1
        record["record_id"] = model.digest({k: v for k, v in record.items() if k not in {"record_id", "comparison"}})
        with self.assertRaisesRegex(ValueError, "total mismatch"):
            model.validate_document(document)

    def test_method_changes_are_separate_and_candidate_not_promoted_to_history(self):
        current, active = self.record("release-candidate")
        previous, _ = self.record()
        self.assertEqual(model.compare(current, previous)["status"], "COMPLETE")
        previous["measurement"]["profile_key"] = "a" * 64
        self.assertEqual(model.compare(current, previous)["status"], "NOT_APPLICABLE")
        value = {**self.document, "profiles": {model.digest(active): active}, "current": current}
        path = self.root / "candidate.json"
        write_new_json(path, value)
        with self.assertRaisesRegex(ValueError, "current=null"): model.read_seed(path)

    def test_duplicate_json_keys_unknown_paths_links_and_broken_text_fail(self):
        path = self.root / "duplicate.json"
        path.write_text('{"schema_version":1,"schema_version":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"): model.read_json(path)
        with self.assertRaisesRegex(ValueError, "unclassified"): model.group("unreviewed.data")
        with mock.patch("tools.release_checks.public_snapshot_inventory.is_link", side_effect=lambda p: p == self.snapshot / "src"):
            with self.assertRaisesRegex(ValueError, "link"): file_records(self.snapshot)
        (self.snapshot / "src/main.cpp").write_bytes(b"\xff")
        with self.assertRaisesRegex(ValueError, "encoding"): self.record()

    def test_export_receipt_rejects_malformed_paths_and_digests(self):
        for path in ("../outside", "D:/secret", "a\\b", "tab\tfile"):
            value = copy.deepcopy(self.receipt)
            value["files"][0]["path"] = path
            value["tree_sha256"] = tree_sha256(value["files"])
            location = self.root / "bad_inventory.json"
            location.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(path=path), self.assertRaises(ValueError): read_inventory(location)

    def test_archive_path_variant_and_duplicate_members_fail(self):
        path = self.root / "wrong.zip"
        with zipfile.ZipFile(path, "w") as archive: archive.writestr("../escape", b"x")
        with self.assertRaises(ValueError): measure(path, "ja", "full")
        with self.assertRaisesRegex(ValueError, "variant"): measure(self.package_paths[0], "en", "full", "1.2.3")

    def test_bom_declared_historical_test_encoding_keeps_raw_bytes_and_lines(self):
        path = self.snapshot / "tests/unit/utf16.cpp"
        path.write_text('// 注釈\nint test2(){return 0;}\n', encoding="utf-16")
        record, _ = self.record()
        self.assertEqual(record["inventory"]["legacy_text_files"], 1)
        self.assertEqual(record["source_scale"]["tests_source"]["lines"], 4)
        self.assertEqual(record["inventory"]["groups"]["tests_source"]["size_bytes"],
                         path.stat().st_size + (self.snapshot / "tests/unit/check.cpp").stat().st_size)

    def test_resources_and_auxiliary_tools_are_separate_from_application(self):
        (self.snapshot / "src/icon.ico").write_bytes(b"resource")
        (self.snapshot / "src/build.ps1").write_text("# tool\n", encoding="utf-8")
        record, _ = self.record()
        self.assertEqual(record["source_scale"]["application"]["files"], 1)
        self.assertEqual(record["inventory"]["groups"]["resources"]["files"], 3)
        self.assertEqual(record["source_scale"]["tools"]["files"], 1)
        self.assertEqual(record["source_scale"]["src_tree"]["files"], 3)
        self.assertEqual(record["source_scale"]["src_tree"]["size_bytes"], sum(p.stat().st_size for p in (self.snapshot / "src").iterdir()))
        with self.assertRaisesRegex(ValueError, "unclassified"): model.group("src/unknown.data")

    def test_method_changes_during_measurement_fail(self):
        model.profile()
        with mock.patch.object(model, "_toolset_at_start", "0" * 64):
            with self.assertRaisesRegex(ValueError, "toolset changed"): self.record()


class PreparedStatisticsTests(StatisticsFixture):
    def prepared(self):
        root = self.root / "_prepared_ja"
        root.mkdir()
        components = {"release": "full", "release_lite": "lite", "release_zip": "full.zip", "release_lite_zip": "lite.zip"}
        for name in ("full", "lite"):
            (root / name).mkdir()
            (root / name / "content").write_bytes(b"owned package")
            (root / (name + ".zip")).write_bytes(b"owned archive")
        identity = self.inventory()
        write_new_json(root / preparation.SOURCE_RECEIPT, identity)
        with mock.patch.object(preparation, "source_identity", return_value=identity):
            preparation.seal(root, self.snapshot, "ja", "1.2.3", components)
        return root, identity

    def test_prepared_packages_are_not_candidates_and_adopt_without_data_loss(self):
        root, identity = self.prepared()
        before = file_records(root)
        destination = self.root / "ja"
        destination.mkdir()
        self.assertFalse((root / "release_set_manifest.json").exists())
        with mock.patch.object(preparation, "source_identity", return_value=identity):
            preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertFalse(root.exists())
        self.assertEqual(before, file_records(destination))
        self.assertFalse((destination / "release_set_manifest.json").exists())

    def test_input_or_package_change_prevents_adoption_and_preserves_both_roots(self):
        root, identity = self.prepared()
        destination = self.root / "ja"
        destination.mkdir()
        with mock.patch.object(preparation, "source_identity", return_value={**identity, "changed": True}):
            with self.assertRaisesRegex(ValueError, "source inputs changed"):
                preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertTrue(root.exists())
        (root / "full.zip").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "files changed"):
            preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertTrue(root.exists())
        self.assertEqual(list(destination.iterdir()), [])

    def test_existing_target_wrong_locale_and_non_sibling_moves_are_rejected(self):
        root, identity = self.prepared()
        destination = self.root / "ja"
        destination.mkdir()
        sentinel = destination / "sentinel"
        sentinel.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "empty"):
            preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertEqual(sentinel.read_bytes(), b"keep")
        sentinel.unlink()
        with self.assertRaisesRegex(ValueError, "mismatch"):
            preparation.adopt(root, destination, self.snapshot, "en", "1.2.3")
        nested = self.root / "elsewhere" / "ja"
        nested.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "sibling"):
            preparation.adopt(root, nested, self.snapshot, "ja", "1.2.3")
        self.assertTrue(root.exists())

    def test_windows_rename_retry_is_bounded_and_preserves_prepared_files(self):
        root, identity = self.prepared()
        destination = self.root / "ja"
        destination.mkdir()
        denied = PermissionError("sharing denied")
        denied.winerror = 5
        rename = Path.rename
        calls = []
        def transient(path, target):
            calls.append(target)
            if len(calls) == 1: raise denied
            return rename(path, target)
        before = file_records(root)
        with mock.patch.object(Path, "rename", transient), mock.patch.object(preparation, "source_identity", return_value=identity), mock.patch.object(preparation.time, "sleep"):
            preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertEqual(len(calls), 2)
        self.assertEqual(before, file_records(destination))
        root, identity = self.prepared()
        destination = self.root / "denied"
        destination.mkdir()
        before = file_records(root)
        with mock.patch.object(Path, "rename", side_effect=denied) as failed, mock.patch.object(preparation, "source_identity", return_value=identity), mock.patch.object(preparation.time, "sleep"):
            with self.assertRaises(PermissionError): preparation.adopt(root, destination, self.snapshot, "ja", "1.2.3")
        self.assertEqual(failed.call_count, 3)
        self.assertEqual(before, file_records(root))


class HistoricalStatisticsTests(StatisticsFixture):
    def catalog(self, kind="private-history-allowlisted"):
        private = self.snapshot / "docs/internal/secret.md"
        private.parent.mkdir(parents=True)
        private.write_text("INTERNAL-NOT-FOR-PUBLIC", encoding="utf-8")
        archive = self.root / "source.tar"
        with tarfile.open(archive, "w") as target:
            for path in self.snapshot.rglob("*"):
                if path.is_file(): target.add(path, arcname=path.relative_to(self.snapshot).as_posix())
        entry = {"version": "1.2.3", "source_kind": kind, "archive": str(archive),
                 "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                 "allowlist": ["src/", "tests/", "docs/", "README.md", "REPO_VERSION.txt", "locales/"]}
        location = self.root / "catalog.json"
        write_new_json(location, {"schema_version": 1, "releases": [entry]})
        return location

    def test_private_history_selects_allowlist_and_publishes_only_aggregates(self):
        catalog = self.catalog()
        output = self.root / "history"
        document = backfill(catalog, output, None, "2026-10-27T00:00:00+00:00")
        self.assertIsNone(document["current"])
        self.assertFalse((output / "inputs/0/docs/internal/secret.md").exists())
        raw = json.dumps(document, ensure_ascii=False)
        for private in ("INTERNAL-NOT-FOR-PUBLIC", "非公開の具体的な本文", "private_symbol", str(self.root), "src/main.cpp"):
            self.assertNotIn(private, raw)
        self.assertEqual(document["history"][0]["measurement"]["source_kind"], "private-history-allowlisted")

    def test_historical_hash_failure_and_missing_private_allowlist_fail_closed(self):
        location = self.catalog()
        catalog = model.read_json(location)
        catalog["releases"][0]["archive_sha256"] = "0" * 64
        location.write_text(json.dumps(catalog), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            backfill(location, self.root / "bad_hash", None, "2026-10-27T00:00:00+00:00")
        catalog["releases"][0]["archive_sha256"] = hashlib.sha256(Path(catalog["releases"][0]["archive"]).read_bytes()).hexdigest()
        catalog["releases"][0].pop("allowlist")
        location.write_text(json.dumps(catalog), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "explicit allowlist"):
            backfill(location, self.root / "bad_scope", None, "2026-10-27T00:00:00+00:00")

    def test_repeated_backfill_preserves_original_record_and_time(self):
        location = self.catalog()
        first = backfill(location, self.root / "first", None, "2026-10-27T00:00:00+00:00")
        second = backfill(location, self.root / "second", self.root / "first/metrics.json", "2026-10-28T00:00:00+00:00")
        self.assertEqual(first, second)

    def development_catalog(self):
        location = self.catalog()
        value = model.read_json(location)
        entry = value["releases"][0]
        entry.pop("version")
        entry["observed_on"] = "2026-04-30"
        value["releases"].append({**entry, "observed_on": "2026-05-31"})
        location.write_text(json.dumps(value), encoding="utf-8")
        return location

    def test_development_time_series_reuses_aggregates_without_private_metadata(self):
        location = self.development_catalog()
        result = backfill(location, self.root / "development", None, "2026-10-27T00:00:00+00:00")
        self.assertEqual(result["history"], [])
        self.assertEqual(len(result["development_history"]), 2)
        self.assertIsNone(result["current"])
        latest = result["development_history"][-1]
        self.assertEqual(latest["comparison"]["application_lines_delta"], 0)
        self.assertTrue(all(value is None for value in latest["release"].values()))
        summary, history = render(result)
        for heading in ("Development scale", "Source-tree size", "Tests and development tools", "C/C++ physical byte partition", "Literal byte composition", "Localization catalogs"):
            self.assertIn(heading, history)
        public = json.dumps(result, ensure_ascii=False) + history + summary
        for secret in ("INTERNAL-NOT-FOR-PUBLIC", "非公開の具体的な本文", "private_symbol", "src/main.cpp", str(self.root)):
            self.assertNotIn(secret, public)
        again = backfill(location, self.root / "development_again", self.root / "development/metrics.json", "2026-10-28T00:00:00+00:00")
        self.assertEqual(result, again)

    def test_development_versions_commits_and_duplicate_dates_are_rejected(self):
        location = self.development_catalog()
        result = backfill(location, self.root / "development", None, "2026-10-27T00:00:00+00:00")
        for change in (lambda r: r["release"].update(version="1.2.3"), lambda r: r["measurement"].update(private_commit="a" * 40),
                       lambda r: r["measurement"].update(repository="private"),
                       lambda r: r["source_scale"].pop("src_tree")):
            value = copy.deepcopy(result)
            row = value["development_history"][0]
            change(row)
            row["record_id"] = model.digest({k: v for k, v in row.items() if k not in {"record_id", "comparison"}})
            with self.assertRaises(ValueError): model.validate_document(value)
        result["development_history"].append(result["development_history"][0])
        with self.assertRaisesRegex(ValueError, "unique and ordered"): model.validate_document(result)
        catalog = model.read_json(location)
        catalog["releases"][0]["version"] = "1.2.3"
        location.write_text(json.dumps(catalog), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "dates and aggregates only"):
            backfill(location, self.root / "bad_development", None, "2026-10-27T00:00:00+00:00")

    def mapped_catalog(self, *, collision=False):
        location = self.development_catalog()
        value = model.read_json(location)
        entry = value["releases"][0]
        archive = Path(entry["archive"])
        old_cpp = "private_old_layout/" + ("main.cpp" if collision else "early.cpp")
        old_python = "private_old_python/main.py"
        with tarfile.open(archive, "a") as target:
            for name, raw in ((old_cpp, b"int earlier(){return 1;}\n"), (old_python, b"print('local fixture text')\n")):
                info = tarfile.TarInfo(name)
                info.size = len(raw)
                target.addfile(info, io.BytesIO(raw))
        entry["archive_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        entry["allowlist"] += [old_cpp, old_python]
        entry["path_map"] = {"private_old_layout/": "src/", "private_old_python/": "src/python/"}
        value["releases"] = [entry]
        location.write_text(json.dumps(value), encoding="utf-8")
        return location

    def test_private_role_mapping_keeps_one_source_scope_without_exposing_names(self):
        location = self.mapped_catalog()
        output = self.root / "mapped_history"
        value = backfill(location, output, None, "2026-10-27T00:00:00+00:00")
        record = value["development_history"][0]
        self.assertEqual(record["source_scale"]["application"]["files"], 3)
        self.assertEqual(record["source_scale"]["src_tree"]["files"], 3)
        self.assertEqual(record["lexical"]["files"], 2)
        self.assertTrue((output / "inputs/0/src/python/main.py").is_file())
        raw = json.dumps(value) + "".join(render(value))
        for private in ("private_old_layout", "private_old_python", "early.cpp", "local fixture text", "private_symbol"):
            self.assertNotIn(private, raw)

    def test_role_mapping_rejects_collisions_and_unreviewed_targets(self):
        location = self.mapped_catalog(collision=True)
        with self.assertRaisesRegex(ValueError, "mapping collision"):
            backfill(location, self.root / "collision", None, "2026-10-27T00:00:00+00:00")
        value = model.read_json(location)
        for target in ("../escape/", "docs/internal/", "other/"):
            value["releases"][0]["path_map"] = {"private_old_layout/": target}
            location.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(target=target), self.assertRaises(ValueError):
                backfill(location, self.root / ("bad_mapping_" + str(len(list(self.root.iterdir())))), None, "2026-10-27T00:00:00+00:00")


class StatisticsPackagingContractTests(unittest.TestCase):
    def test_distribution_statistics_link_uses_the_public_portal(self):
        package = (REPO_ROOT / "scripts/release/pack_release.ps1").read_text(encoding="utf-8-sig")
        copy_top = package.split("function Copy-ReleaseTopDocuments(", 1)[1].split("function Write-ReleaseSetupJson(", 1)[0]
        portal = "https://soone-y.github.io/OPEN_PDF-Note-Workspace/STATISTICS.html"
        self.assertIn(f'$content = $content.Replace("](STATISTICS.md)", "]({portal})")', copy_top)
        allowlist = json.loads((REPO_ROOT / "site/github/documentation_portal_allowlist.json").read_text(encoding="utf-8"))
        self.assertIn({"source": "STATISTICS.md", "destination": "STATISTICS.md"}, allowlist["documentation_portal"]["files"])
        # Repository/snapshot readers retain the local, versioned statistics.
        self.assertIn("](STATISTICS.md)", (REPO_ROOT / "README.md").read_text(encoding="utf-8"))


@unittest.skipUnless(shutil.which("pwsh") and shutil.which("git"), "requires PowerShell and Git")
class StatisticsPipelineTests(StatisticsFixture):
    def make_repository(self):
        repository = self.root / "repository"
        shutil.copytree(self.snapshot, repository)
        observation, active = self.record("private-history-allowlisted")
        observation["release"] = dict.fromkeys(observation["release"])
        observation["measurement"]["observed_on"] = "2026-04-30"
        observation["record_id"] = model.digest({k: v for k, v in observation.items() if k not in {"record_id", "comparison"}})
        cli.write_views(repository, {**self.document, "profiles": {model.digest(active): active}, "development_history": [observation]}, exclusive=False)
        for relative in ("release.ps1", "publish.ps1", "scripts/release/release_version.ps1", "scripts/release/make_release_set.ps1", "scripts/release/export_public_snapshot.ps1",
                         "tools/dev/export_public_snapshot.py", "tools/metrics/code_metrics/analyze_repo.py",
                         "tools/metrics/distribution_size_report.py", "tools/metrics/generate_public_statistics.py", "tools/metrics/backfill_public_statistics.py",
                         "tools/release_checks/prepared_release_packages.py", "tools/release_checks/public_snapshot_inventory.py",
                         "tools/release_checks/public_statistics_gate.py", "tools/release_checks/release_set_integrity_gate.py",
                         "tools/release_checks/public_snapshot_content_gate.py"):
            path = repository / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / relative, path)
        for relative in ("tools/metrics/public_statistics", "tools/metrics/code_structure"):
            shutil.copytree(REPO_ROOT / relative, repository / relative, ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copyfile(REPO_ROOT / "tests/fixtures/public_statistics/pack.py", repository / "tools/fixture_pack.py")
        (repository / "build.ps1").write_text('param([string]$Locale,[switch]$Rebuild,[switch]$VerboseOutput)\nWrite-Output "fixture build $Locale"\n', encoding="utf-8")
        (repository / "scripts/release/pack_release.ps1").write_text(r'''
param([string]$OutBaseDir,[string]$NamePrefix,[switch]$Checksums,[switch]$Zip,[string]$Locale,[switch]$Lite)
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$edition=if($Lite){'lite'}else{'full'}
& python -B (Join-Path $repoRoot 'tools/fixture_pack.py') --out $OutBaseDir --locale $Locale --edition $edition
if($LASTEXITCODE -ne 0){throw 'fixture packaging failed'}
''', encoding="utf-8")
        site = repository / "site/github/scripts"
        site.mkdir(parents=True)
        (site / "build_public_site.py").write_text("from pathlib import Path\np=Path(__file__).resolve().parent.parent/'output/public'\np.mkdir(parents=True)\n(p/'index.html').write_text('<html>fixture</html>',encoding='utf-8')\n", encoding="utf-8")
        (site / "validate_public_site.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
        policy = repository / "docs/internal"
        policy.mkdir()
        (policy / "public_repo_release_allowlist_2026-08-24.txt").write_text("README.md\nREPO_VERSION.txt\nSTATISTICS.md\n.gitignore\ndocs/statistics/\ndocs/ja/\nsrc/\nlocales/\ntools/\nscripts/\nsite/\nthird_party/\nbuild.ps1\nrelease.ps1\n", encoding="utf-8")
        (repository / "third_party").mkdir()
        (repository / "third_party/vendor.bin").write_bytes(b"reviewed fixture artifact")
        (repository / ".gitignore").write_text("third_party/vendor.bin\n__pycache__/\n", encoding="utf-8")
        vendor_hash = hashlib.sha256((repository / "third_party/vendor.bin").read_bytes()).hexdigest()
        (policy / "public_repo_release_artifact_manifest_2026-08-12.tsv").write_text(vendor_hash + "\tthird_party/vendor.bin\n", encoding="utf-8")
        (policy / "public_repo_gitignoreテンプレート_2026-07-02.gitignore").write_text("__pycache__/\n", encoding="utf-8")
        subprocess.run(["git", "init", "--quiet", str(repository)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(repository), "add", "."], check=True, capture_output=True)
        return repository

    def test_two_phase_pair_statistics_integrity_and_read_only_validation(self):
        repository = self.make_repository()
        environment = os.environ.copy()
        environment["TEMP"] = environment["TMP"] = str(self.root)
        log = self.root / "pipeline.log"
        wrapper = self.root / "invoke_release.ps1"
        wrapper.write_text("param([string]$Entry,[string]$OutputRoot)\n[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n& $Entry -AllLocales -DeferPostCreationValidation -ReleaseSetBaseDir $OutputRoot\nexit $LASTEXITCODE\n", encoding="utf-8")
        with log.open("wb") as output:
            result = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-File", str(wrapper), "-Entry", str(repository / "release.ps1"),
                                     "-OutputRoot", str(self.root / "sets")],
                                    cwd=repository, env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=180)
        evidence = REPO_ROOT / "out/tests/public_statistics_pipeline"
        evidence.mkdir(parents=True, exist_ok=True)
        evidence_log = evidence / (self.root.name + ".log")
        shutil.copyfile(log, evidence_log)
        if result.returncode:
            raise AssertionError(f"Release control-flow fixture failed, exit={result.returncode}; full log: {evidence_log}")
        pairs = list((self.root / "sets").iterdir())
        self.assertEqual(len(pairs), 1)
        ja, en = pairs[0] / "ja", pairs[0] / "en"
        self.assertFalse((pairs[0] / "prepared_ja").exists())
        self.assertFalse((pairs[0] / "prepared_en").exists())
        before = (file_records(ja), file_records(en))
        self.assertEqual(public_statistics_gate.validate_pair(ja, en), [])
        self.assertEqual(before, (file_records(ja), file_records(en)))
        from tools.release_checks.release_set_integrity_gate import validate_release_set
        self.assertEqual(validate_release_set(ja), [])
        self.assertEqual(validate_release_set(en), [])
        document = model.read_json(ja / "public_snapshot/docs/statistics/metrics.json")
        self.assertEqual(len(document["current"]["packages"]), 4)
        self.assertEqual(len(document["development_history"]), 1)
        self.assertEqual((ja / "public_snapshot/STATISTICS.md").read_bytes(), (en / "public_snapshot/STATISTICS.md").read_bytes())
        publish_wrapper = repository / "check_publish_pair.ps1"
        publish_wrapper.write_text(r'''
param([string]$PairRoot)
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'publish.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'publish parse failed'}
$functions=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-UnifiedReleasePair'},$true))
if($functions.Count -ne 1){throw 'publish pair boundary missing'}
$functionPath=Join-Path $PSScriptRoot 'publish_pair_function.ps1'
Set-Content -LiteralPath $functionPath -Value $functions[0].Extent.Text -Encoding UTF8
. $functionPath
$items=@(foreach($locale in @('ja','en')){
    $root=Join-Path $PairRoot $locale
    $manifest=Get-Content -LiteralPath (Join-Path $root 'release_set_manifest.json') -Raw | ConvertFrom-Json
    [pscustomobject]@{SetRoot=$root;Result=[pscustomobject]@{Manifest=$manifest;SnapshotTreeHash='same';SnapshotAllowlistHash='same';SnapshotArtifactManifestHash='same'}}
})
Assert-UnifiedReleasePair -Items $items
''', encoding="utf-8")
        def publish_check(expected):
            with log.open("ab") as output:
                checked = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-File", str(publish_wrapper), "-PairRoot", str(pairs[0])],
                                         cwd=repository, env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=60)
            shutil.copyfile(log, evidence_log)
            self.assertEqual(checked.returncode == 0, expected, f"Publish pair boundary result; full log: {evidence_log}")
        publish_check(True)
        self.assertEqual(before, (file_records(ja), file_records(en)))
        for root in (ja, en):
            manifest_path = root / "release_set_manifest.json"
            original_manifest = manifest_path.read_bytes()
            manifest = json.loads(original_manifest)
            manifest.pop("statistics_schema_version")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            self.assertTrue(public_statistics_gate.validate_set(root))
            self.assertTrue(public_statistics_gate.validate_pair(ja, en))
            manifest_path.write_bytes(original_manifest)
        archive = next(en.glob("*en_full.zip"))
        original = archive.read_bytes()
        archive.write_bytes(original + b"modified")
        self.assertTrue(public_statistics_gate.validate_pair(ja, en))
        publish_check(False)


if __name__ == "__main__":
    unittest.main()
