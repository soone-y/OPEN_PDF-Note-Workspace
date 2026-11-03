# 統計履歴 / Statistics history

数値は公開対象相当の固定入力についての計測値です。品質・安全性の総合評価ではありません。
Values describe fixed public-equivalent inputs; they are not a quality or security score.

統計文書と生成Pagesを除外します。code/comment行は対応sourceの簡易分類です。
Statistics and generated Pages are excluded. Code/comment lines are approximate classifications of supported source files.

private-history-allowlisted は非公開履歴から許可範囲相当だけを計測した値です。公開済みsnapshot自体の値とは区別します。
private-history-allowlisted denotes aggregate measurements of an allowlisted subset of private history, not the published snapshot itself.

## 開発規模の推移 / Development scale

開発履歴の固定観測点から、allowlist相当の範囲だけを再集計し、一つの日付順の表で示します。
Fixed observations of allowlisted development history are presented in one chronological table.

表中の日付に固定した観測点を示します。全コミットの列挙や未掲載期間の補間は行いません。
The table lists observations fixed to the dates shown, without enumerating every commit or interpolating missing periods.

Gitで追跡した許可範囲だけが対象です。内部文書・履歴の説明・個別ファイル名・非公開の版番号やcommit IDは掲載しません。
Only tracked, allowlisted inputs are measured. Internal documents, change descriptions, individual filenames, private versions and commit IDs are omitted.

### ソース領域の物理サイズ / Source-tree size

src相当の許可範囲を同じ責務で選び、資源を含む実byte数を示します。実装ソースのbyte数とC/C++字句量は、以下の別表に示します。
A consistent allowlisted source-tree role covers raw file bytes including resources. Application-source bytes and C/C++ lexical measurements are listed separately below.

| 日付 / Date | Files | Bytes | Δ Bytes |
| --- | ---: | ---: | ---: |
| 2025-12-06 | 6 | 30,669 | UNMEASURED |
| 2025-12-29 | 69 | 1,249,946 | +1,219,277 |
| 2025-12-30 | 69 | 1,256,541 | +6,595 |
| 2025-12-31 | 67 | 1,161,123 | -95,418 |
| 2026-01-31 | 71 | 1,719,539 | +558,416 |
| 2026-02-28 | 87 | 2,555,437 | +835,898 |
| 2026-03-31 | 76 | 3,092,274 | +536,837 |
| 2026-04-08 | 78 | 3,364,363 | +272,089 |
| 2026-04-09 | 76 | 3,059,416 | -304,947 |
| 2026-04-30 | 92 | 3,445,307 | +385,891 |
| 2026-05-31 | 134 | 4,084,062 | +638,755 |
| 2026-06-30 | 166 | 4,942,433 | +858,371 |
| 2026-07-31 | 205 | 5,685,753 | +743,320 |
| 2026-08-31 | 210 | 5,947,523 | +261,770 |
| 2026-09-30 | 269 | 7,328,547 | +1,381,024 |
| 2026-10-05 | 275 | 7,734,982 | +406,435 |

### アプリ実装 / Application source

| 日付 / Date | Files | Lines | Code | Comment | Blank | Bytes | Δ Lines |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2025-12-06 | 6 | 961 | 714 | 68 | 179 | 30,669 | UNMEASURED |
| 2025-12-29 | 69 | 30,667 | 21,752 | 6,740 | 2,175 | 1,249,946 | +29,706 |
| 2025-12-30 | 69 | 30,880 | 21,959 | 6,740 | 2,181 | 1,256,541 | +213 |
| 2025-12-31 | 67 | 28,224 | 19,410 | 6,757 | 2,057 | 1,161,123 | -2,656 |
| 2026-01-31 | 71 | 41,584 | 31,868 | 6,824 | 2,892 | 1,719,539 | +13,360 |
| 2026-02-28 | 87 | 61,291 | 49,582 | 7,184 | 4,525 | 2,555,437 | +19,707 |
| 2026-03-31 | 70 | 69,583 | 63,192 | 1,444 | 4,947 | 2,938,684 | +8,292 |
| 2026-04-08 | 72 | 75,876 | 69,148 | 1,440 | 5,288 | 3,210,773 | +6,293 |
| 2026-04-09 | 70 | 69,191 | 62,932 | 1,406 | 4,853 | 2,905,826 | -6,685 |
| 2026-04-30 | 86 | 78,699 | 71,609 | 1,423 | 5,667 | 3,291,227 | +9,508 |
| 2026-05-31 | 127 | 92,125 | 83,857 | 1,557 | 6,711 | 3,856,770 | +13,426 |
| 2026-06-30 | 159 | 111,996 | 102,116 | 1,636 | 8,244 | 4,715,141 | +19,871 |
| 2026-07-31 | 197 | 127,835 | 116,182 | 2,229 | 9,424 | 5,427,242 | +15,839 |
| 2026-08-31 | 202 | 133,633 | 121,068 | 2,738 | 9,827 | 5,689,012 | +5,798 |
| 2026-09-30 | 260 | 162,501 | 146,151 | 4,656 | 11,694 | 7,069,614 | +28,868 |
| 2026-10-05 | 266 | 169,514 | 152,160 | 5,488 | 11,866 | 7,476,049 | +7,013 |

### 検査・開発基盤 / Tests and development tools

| 日付 / Date | Test files | Test lines | Tool files | Tool lines | 文書file / Document files | 全file / All files | 全byte / All bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2025-12-06 | 0 | 0 | 0 | 0 | 1 | 7 | 30,697 |
| 2025-12-29 | 0 | 0 | 1 | 75 | 1 | 71 | 1,258,571 |
| 2025-12-30 | 0 | 0 | 1 | 75 | 0 | 70 | 1,258,643 |
| 2025-12-31 | 0 | 0 | 1 | 74 | 0 | 68 | 1,163,182 |
| 2026-01-31 | 0 | 0 | 1 | 319 | 2 | 74 | 1,737,469 |
| 2026-02-28 | 3 | 300 | 1 | 17 | 3 | 95 | 2,584,648 |
| 2026-03-31 | 21 | 2,438 | 1 | 24 | 4 | 150 | 9,728,332 |
| 2026-04-08 | 23 | 2,759 | 1 | 24 | 4 | 154 | 10,015,190 |
| 2026-04-09 | 12 | 1,374 | 1 | 24 | 4 | 140 | 9,598,885 |
| 2026-04-30 | 14 | 2,202 | 1 | 24 | 4 | 136 | 3,759,192 |
| 2026-05-31 | 22 | 4,216 | 1 | 24 | 4 | 192 | 5,282,041 |
| 2026-06-30 | 25 | 6,993 | 1 | 47 | 5 | 257 | 5,564,610 |
| 2026-07-31 | 33 | 10,567 | 46 | 15,832 | 5 | 391 | 10,643,750 |
| 2026-08-31 | 42 | 13,155 | 60 | 19,613 | 46 | 506 | 12,172,900 |
| 2026-09-30 | 44 | 19,827 | 65 | 20,918 | 56 | 581 | 12,690,090 |
| 2026-10-05 | 54 | 23,268 | 65 | 20,992 | 56 | 603 | 13,385,474 |

### C/C++の物理byte内訳 / C/C++ physical byte partition

| 日付 / Date | Source bytes | Literal | Comment | Whitespace | Remaining source | Lexical items |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 2025-12-06 | 12,732 | 418 | 1,593 | 3,637 | 7,084 | 2,262 |
| 2025-12-29 | 1,249,946 | 37,613 | 311,835 | 301,879 | 598,619 | 189,054 |
| 2025-12-30 | 1,256,541 | 37,916 | 311,806 | 302,219 | 604,600 | 190,748 |
| 2025-12-31 | 1,161,123 | 37,329 | 312,915 | 269,125 | 541,754 | 168,837 |
| 2026-01-31 | 1,719,539 | 60,279 | 318,471 | 429,875 | 910,914 | 284,295 |
| 2026-02-28 | 2,555,437 | 115,523 | 347,049 | 672,341 | 1,420,524 | 432,770 |
| 2026-03-31 | 2,938,684 | 162,673 | 61,569 | 860,448 | 1,853,994 | 564,884 |
| 2026-04-08 | 3,210,773 | 193,904 | 61,927 | 942,659 | 2,012,283 | 611,460 |
| 2026-04-09 | 2,905,826 | 171,784 | 58,799 | 854,783 | 1,820,460 | 557,291 |
| 2026-04-30 | 3,291,227 | 200,116 | 59,454 | 961,527 | 2,070,130 | 623,079 |
| 2026-05-31 | 3,856,770 | 237,924 | 67,721 | 1,113,633 | 2,437,492 | 727,394 |
| 2026-06-30 | 4,715,141 | 305,728 | 73,294 | 1,343,824 | 2,992,295 | 878,795 |
| 2026-07-31 | 5,427,242 | 381,032 | 97,559 | 1,520,455 | 3,428,196 | 1,001,052 |
| 2026-08-31 | 5,689,012 | 333,105 | 120,298 | 1,572,553 | 3,663,056 | 1,062,351 |
| 2026-09-30 | 7,069,614 | 374,832 | 244,500 | 1,913,301 | 4,536,981 | 1,275,618 |
| 2026-10-05 | 7,476,049 | 385,791 | 302,621 | 2,004,443 | 4,783,194 | 1,335,645 |

### Literalの内訳 / Literal byte composition

| 日付 / Date | String bytes | Character bytes | Number bytes | Other literal bytes |
| --- | ---: | ---: | ---: | ---: |
| 2025-12-06 | 330 | 0 | 88 | 0 |
| 2025-12-29 | 26,381 | 2,084 | 9,148 | 0 |
| 2025-12-30 | 26,570 | 2,098 | 9,248 | 0 |
| 2025-12-31 | 27,087 | 1,610 | 8,632 | 0 |
| 2026-01-31 | 43,504 | 2,439 | 14,336 | 0 |
| 2026-02-28 | 91,644 | 3,831 | 20,048 | 0 |
| 2026-03-31 | 132,966 | 5,369 | 24,338 | 0 |
| 2026-04-08 | 162,400 | 5,774 | 25,730 | 0 |
| 2026-04-09 | 142,794 | 5,499 | 23,491 | 0 |
| 2026-04-30 | 168,737 | 6,401 | 24,978 | 0 |
| 2026-05-31 | 201,331 | 7,013 | 29,580 | 0 |
| 2026-06-30 | 261,629 | 8,939 | 35,160 | 0 |
| 2026-07-31 | 330,003 | 10,277 | 40,752 | 0 |
| 2026-08-31 | 279,734 | 10,697 | 42,674 | 0 |
| 2026-09-30 | 315,004 | 11,950 | 47,878 | 0 |
| 2026-10-05 | 324,575 | 12,229 | 48,987 | 0 |

字句itemはClang token・関数数ではありません。文字列の用途は未分類です。Remaining sourceには宣言・識別子・記号を含み、ロジック量とは呼びません。
Lexical items are not Clang tokens or function counts. String purposes are unclassified; remaining source includes declarations, identifiers and punctuation, and is not a logic measure.

### 文言カタログ / Localization catalogs

| 日付 / Date | JA IDs | EN IDs | EN missing IDs |
| --- | ---: | ---: | ---: |
| 2025-12-06 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2025-12-29 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2025-12-30 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2025-12-31 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-01-31 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-02-28 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-03-31 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-04-08 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-04-09 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-04-30 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-05-31 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-06-30 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-07-31 | UNMEASURED | UNMEASURED | UNMEASURED |
| 2026-08-31 | 1,986 | 1,986 | 0 |
| 2026-09-30 | 2,228 | 2,228 | 0 |
| 2026-10-05 | 2,313 | 2,313 | 0 |

検査コードの量は試験の十分性・カバレッジを示しません。過去時点の試験結果・AST指標は未測定です。
Test-source size is not test adequacy or coverage. Historical test results and AST metrics are unmeasured.

## 公開版の記録 / Published releases

| 版 / Version | 入力 / Input | アプリsource行 / Lines | ファイル / Files | Byte | 比較 / Comparison |
| --- | --- | ---: | ---: | ---: | --- |
| 0.9.520 | public-snapshot | 170,588 | 638 | 21,752,865 | COMPLETE |
| 0.9.511 | public-snapshot | 142,957 | 549 | 19,502,481 | COMPLETE |
| 0.9.504 | public-snapshot | 133,314 | 535 | 20,257,916 | COMPLETE |
| 0.9.503 | public-snapshot | 133,182 | 533 | 20,237,275 | COMPLETE |
| 0.9.502 | public-snapshot | 133,149 | 533 | 20,219,752 | COMPLETE |
| 0.9.015 | public-snapshot | 132,065 | 485 | 19,630,795 | COMPLETE |
| 0.9.014 | public-snapshot | 131,948 | 485 | 19,622,916 | COMPLETE |
| 0.9.008 | public-snapshot | 127,337 | 443 | 17,291,187 | UNMEASURED |

欠落値は未測定であり、0を意味しません。差分は方式と入力範囲が一致する観測点について計算します。
Missing values are unmeasured, not zero. Differences are calculated between observations with matching methods and input scopes.

各版の計測日時・方式・入力hash・配布物・適用範囲は [JSON](metrics.json) を参照してください。
See the [JSON](metrics.json) for measurement dates, methods, input hashes, packages and scope of every record.

[主要統計 / Statistics](../../STATISTICS.md)
