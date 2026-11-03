"""Deterministic bilingual views, containing aggregates rather than source excerpts."""
from __future__ import annotations

from tools.metrics.public_statistics.model import scope_kind, validate_document


def history_rows(document: dict, records: list[dict]) -> list[str]:
    rows = ["| 版 / Version | 入力 / Input | アプリsource行 / Lines | ファイル / Files | Byte | 比較 / Comparison |",
            "| --- | --- | ---: | ---: | ---: | --- |"]
    for record in records:
        rows.append(f"| {record['release']['version']} | {record['measurement']['source_kind']} | {record['source_scale']['application']['lines']:,} | "
                    f"{record['inventory']['files']:,} | {record['inventory']['size_bytes']:,} | {record['comparison']['status']} |")
    return rows


def development_tables(records: list[dict]) -> list[str]:
    lines = ["## 開発規模の推移 / Development scale", "",
             "開発履歴の固定観測点から、allowlist相当の範囲だけを再集計し、一つの日付順の表で示します。",
             "Fixed observations of allowlisted development history are presented in one chronological table.", "",
             "表中の日付に固定した観測点を示します。全コミットの列挙や未掲載期間の補間は行いません。",
             "The table lists observations fixed to the dates shown, without enumerating every commit or interpolating missing periods.", "",
             "Gitで追跡した許可範囲だけが対象です。内部文書・履歴の説明・個別ファイル名・非公開の版番号やcommit IDは掲載しません。",
             "Only tracked, allowlisted inputs are measured. Internal documents, change descriptions, individual filenames, private versions and commit IDs are omitted.", "",
             "### ソース領域の物理サイズ / Source-tree size", "",
             "src相当の許可範囲を同じ責務で選び、資源を含む実byte数を示します。実装ソースのbyte数とC/C++字句量は、以下の別表に示します。",
             "A consistent allowlisted source-tree role covers raw file bytes including resources. Application-source bytes and C/C++ lexical measurements are listed separately below.", "",
             "| 日付 / Date | Files | Bytes | Δ Bytes |",
             "| --- | ---: | ---: | ---: |"]
    previous = None
    for record in records:
        tree = record["source_scale"]["src_tree"]
        compatible = previous is not None and record["comparison"]["status"] == "COMPLETE" and record["comparison"]["baseline_record_id"] == previous["record_id"]
        change = f"{tree['size_bytes'] - previous['source_scale']['src_tree']['size_bytes']:+,}" if compatible else "UNMEASURED"
        lines.append(f"| {record['measurement']['observed_on']} | {tree['files']:,} | {tree['size_bytes']:,} | {change} |")
        previous = record
    lines += ["", "### アプリ実装 / Application source", "",
             "| 日付 / Date | Files | Lines | Code | Comment | Blank | Bytes | Δ Lines |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for record in records:
        app = record["source_scale"]["application"]
        delta = record["comparison"].get("application_lines_delta")
        values = " | ".join(f"{app[key]:,}" for key in ("files", "lines", "code_lines", "comment_lines", "blank_lines", "size_bytes"))
        change = f"{delta:+,}" if delta is not None else "UNMEASURED"
        lines.append(f"| {record['measurement']['observed_on']} | {values} | {change} |")
    lines += ["", "### 検査・開発基盤 / Tests and development tools", "",
              "| 日付 / Date | Test files | Test lines | Tool files | Tool lines | 文書file / Document files | 全file / All files | 全byte / All bytes |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for record in records:
        tests, tools, inv = record["source_scale"]["tests_source"], record["source_scale"]["tools"], record["inventory"]
        values = (tests["files"], tests["lines"], tools["files"], tools["lines"], inv["groups"]["documents"]["files"], inv["files"], inv["size_bytes"])
        lines.append(f"| {record['measurement']['observed_on']} | " + " | ".join(f"{value:,}" for value in values) + " |")
    lines += ["", "### C/C++の物理byte内訳 / C/C++ physical byte partition", "",
              "| 日付 / Date | Source bytes | Literal | Comment | Whitespace | Remaining source | Lexical items |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for record in records:
        lex = record["lexical"]
        values = (lex["source_bytes"], lex["bytes"]["literal"], lex["bytes"]["comment"], lex["bytes"]["whitespace"], lex["bytes"]["code"], lex["lexical_items"])
        lines.append(f"| {record['measurement']['observed_on']} | " + " | ".join(f"{value:,}" for value in values) + " |")
    lines += ["", "### Literalの内訳 / Literal byte composition", "",
              "| 日付 / Date | String bytes | Character bytes | Number bytes | Other literal bytes |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for record in records:
        values = record["lexical"]["literal_bytes"]
        lines.append(f"| {record['measurement']['observed_on']} | " + " | ".join(f"{values[key]:,}" for key in ("string", "character", "number", "other")) + " |")
    lines += ["", "字句itemはClang token・関数数ではありません。文字列の用途は未分類です。Remaining sourceには宣言・識別子・記号を含み、ロジック量とは呼びません。",
              "Lexical items are not Clang tokens or function counts. String purposes are unclassified; remaining source includes declarations, identifiers and punctuation, and is not a logic measure.", "",
              "### 文言カタログ / Localization catalogs", "",
              "| 日付 / Date | JA IDs | EN IDs | EN missing IDs |",
              "| --- | ---: | ---: | ---: |"]
    for record in records:
        loc = record["localization"]
        values = [f"{loc[key]:,}" if loc[key] is not None else "UNMEASURED" for key in ("ja_ids", "en_ids", "en_missing_ids")]
        lines.append(f"| {record['measurement']['observed_on']} | " + " | ".join(values) + " |")
    return lines + ["", "検査コードの量は試験の十分性・カバレッジを示しません。過去時点の試験結果・AST指標は未測定です。",
                    "Test-source size is not test adequacy or coverage. Historical test results and AST metrics are unmeasured.", ""]


def render(document: dict) -> tuple[str, str]:
    validate_document(document)
    current = document["current"]
    history = list(reversed(document["history"]))
    shared = ["数値は公開対象相当の固定入力についての計測値です。品質・安全性の総合評価ではありません。",
              "Values describe fixed public-equivalent inputs; they are not a quality or security score.", "",
              "統計文書と生成Pagesを除外します。code/comment行は対応sourceの簡易分類です。",
              "Statistics and generated Pages are excluded. Code/comment lines are approximate classifications of supported source files.", "",
              "private-history-allowlisted は非公開履歴から許可範囲相当だけを計測した値です。公開済みsnapshot自体の値とは区別します。",
              "private-history-allowlisted denotes aggregate measurements of an allowlisted subset of private history, not the published snapshot itself.", ""]
    lines = ["# 規模・検証状況 / Project statistics", "", *shared]
    if current is None:
        lines += ["現在の対象版は未確定です。過去版の固定入力による計測だけを掲載しています。",
                  "The current release is not finalized. Only fixed historical measurements are listed.", ""]
    else:
        inventory = current["inventory"]
        app = current["source_scale"]["application"]
        lexical = current["lexical"]
        lines += [f"対象版 / Candidate version: **{current['release']['version']}**", "",
                  f"計測日時 / Measured at: {current['measurement']['measured_at']}", "",
                  "| 指標 / Metric | 値 / Value |", "| --- | ---: |",
                  f"| 計測対象file / Measured files | {inventory['files']:,} |",
                  f"| 計測対象byte / Measured bytes | {inventory['size_bytes']:,} |",
                  f"| アプリsource file / Application source files | {app['files']:,} |",
                  f"| アプリsource行 / Application source lines | {app['lines']:,} |",
                  f"| code / comment lines（対応source） | {app['code_lines']:,} / {app['comment_lines']:,} |",
                  f"| 分類対応file / Classified source files | {app['classified_files']:,} / {app['text_files']:,} |",
                  f"| 空行 / Application text blank lines | {app['blank_lines']:,} |",
                  f"| 字句計測対象 / Lexically measured C/C++ files | {lexical['files']:,} |",
                  f"| C/C++ literal source bytes | {lexical['bytes']['literal']:,} |",
                  f"| 検証証跡 / Verification evidence | {current['verification']['status']} |", "",
                  "## 配布物 / Packages", "", "| Locale | Edition | ZIP byte | 非圧縮byte / Unpacked byte | 状態 / Status |",
                  "| --- | --- | ---: | ---: | --- |"]
        for package in current["packages"]:
            size = package['unpacked_bytes']
            lines.append(f"| {package['locale']} | {package['edition']} | {package['zip_bytes']:,} | {size if size is not None else 'UNMEASURED'} | {package['status']} |")
        lines += ["", f"Input SHA-256: `{current['measurement']['input_sha256']}`", "",
                  f"Profile: `{current['measurement']['profile_key']}`", ""]
    compatible = [row for row in history if current is None or (row["measurement"]["profile_key"] == current["measurement"]["profile_key"] and scope_kind(row) == scope_kind(current))]
    lines += ["## 過去版 / History", "", *history_rows(document, compatible[:5]), "",
              "[全履歴 / Full history](docs/statistics/HISTORY.md) · [JSON](docs/statistics/metrics.json)", ""]
    all_history = ["# 統計履歴 / Statistics history", "", *shared, *history_rows(document, history), "",
                   "欠落値は未測定であり、0を意味しません。差分は方式と入力範囲が一致する観測点について計算します。",
                   "Missing values are unmeasured, not zero. Differences are calculated between observations with matching methods and input scopes.", "",
                   "各版の計測日時・方式・入力hash・配布物・適用範囲は [JSON](metrics.json) を参照してください。",
                   "See the [JSON](metrics.json) for measurement dates, methods, input hashes, packages and scope of every record.", "",
                   "[主要統計 / Statistics](../../STATISTICS.md)", ""]
    if document.get("development_history"):
        all_history = [*all_history[:2], *shared, *development_tables(document["development_history"]),
                       "## 公開版の記録 / Published releases", "", *all_history[2 + len(shared):]]
    return "\n".join(lines), "\n".join(all_history)
