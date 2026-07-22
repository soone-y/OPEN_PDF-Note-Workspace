# third_party

このディレクトリは、PDF Note Workspace が利用する第三者成果物を置く場所です。単なる一時置き場ではなく、各依存の役割、保持理由、配布時の扱い、安全上の制約を明文化して管理します。

このプロジェクトでは、第三者成果物についても次を最優先します。

- 外部通信、自動更新、テレメトリ、クラッシュ送信、広告 SDK を入れない
- ユーザーの原本ファイルを直接破壊しない
- 音を鳴らす API や OS 既定の警告音に依存しない
- 生成物、ログ、キャッシュ、ユーザーデータを `third_party/` に書き込まない
- release に入れるものと、repo に保持するものを分けて判断する

## 役割一覧

| 依存 | 主な役割 | repo での保持対象 | release での扱い |
| --- | --- | --- | --- |
| `pdfium/` | PDF の読み込み、描画、テキスト抽出、PDF 書き出し | SDK ヘッダ、import lib、`pdfium.dll`、ライセンス文書 | `pdfium.dll` と PDFium 関連ライセンスを同梱 |
| `md4c/` | ノート本文の Markdown 解析 | `md4c.c`, `md4c.h` を含む vendor ソースとライセンス文書 | バイナリには静的に組み込まれ、ライセンス文書を同梱 |
| `libreoffice/` | docx/pptx など Office 文書から PDF へのローカル変換、同梱フォントのアプリ内 private font 利用 | LibreOffice 管理者展開イメージ、実行条件メモ、ライセンス文書 | `image/Fonts/` の選択 subset と対応ライセンス/NOTICE を同梱。Office 変換エンジン本体は明示指定時のみ同梱 |
| `mingw_runtime_licenses/` | MinGW-w64 runtime DLL のライセンス証跡 | `libstdc++`, `libgcc`, `winpthread`, `zlib` 向けライセンス文書 | runtime DLL と対応ライセンスを同梱 |

## 共通ルール

### 置いてよいもの

- アプリ本体のビルド、実行、配布、ライセンス遵守に必要な第三者成果物
- 取得元、バージョン、ハッシュ、ローカル変更点を説明する README / VERSION / NOTICE 類
- 配布対象バイナリに対応するライセンス文書

### 置かないもの

- ダウンロードキャッシュ、未確定の検証ファイル、一時ファイル
- `.git`, CI 設定、テストスイート、サンプル、ビルド中間物など、利用に不要な upstream 付属物
- ユーザーの文書、変換結果、ログ、クラッシュダンプ、Python bytecode、profile/cache
- 外部通信、自動更新、送信、広告、計測を目的とする実行ファイルや設定

### 変更時に確認するもの

- `THIRD_PARTY_NOTICES.md`
  - 配置場所、用途、ライセンス文書、release 同梱方法が実態と一致していること
- `README.md`
  - リポジトリ構成、ビルド、release フォルダ説明が実態と一致していること
- `scripts/build/build_workspace.ps1` / `scripts/build/build_sources.json`
  - build に必要な include / source / import lib が実態と一致していること
- `scripts/release/pack_release.ps1`
  - release に入れるバイナリとライセンス文書が実態と一致していること
- `.gitignore`
  - 大きな実体や生成物を不用意に Git 追跡せず、必要な README / LICENSE / NOTICE / VERSION は追跡できること
- `docs/internal/reports/license_棚卸し調査レポート_2026-04-09.md`
  - ライセンス整理メモが最新状態に追随していること

## `pdfium/`

### 役割

PDFium は PDF 処理の中核依存です。本アプリでは、PDF の読み込み、ページ描画、テキスト抽出、ページサイズ取得、PDF 出力、注釈を反映した PDF 生成などに使います。

### 保持対象

- `include/`
  - C/C++ から PDFium API を呼ぶためのヘッダ群
- `lib/pdfium.dll.lib`
  - MinGW link に使う import library
- `bin/pdfium.dll`
  - 開発実行時および release 用の実行時 DLL
- `LICENSE`
  - PDFium package 側のライセンス
- `licenses/`
  - PDFium 本体および PDFium に含まれる第三者コンポーネントのライセンス群
- `VERSION`, `PDFiumConfig.cmake`, `args.gn`
  - バージョンと取得内容を確認するための証跡

公開 snapshot では、ライセンス確認済みの PDFium 配布物だけを対象とし、`.orig`、`.rej`、`.bak` などの作業用バックアップは除外します。

### ビルド/実行

- `scripts/build/build_workspace.ps1` は `third_party/pdfium/include` を include path に入れます
- link では `third_party/pdfium/lib/pdfium.dll.lib` を使います
- build 後、`third_party/pdfium/bin/pdfium.dll` を `out/bin/pdfium.dll` へコピーします

### 配布

- `scripts/release/pack_release.ps1` は `pdfium.dll` を release ルートへコピーします
- `third_party/pdfium/LICENSE` と `third_party/pdfium/licenses/` は release の `licenses/pdfium/` へコピーします
- `pdfium.dll` を配る場合は、対応するライセンス文書群も必ず配ります

### 安全上の扱い

- PDFium は PDF の解析と描画に限定して使い、ネットワーク取得や外部 URL 起動の入口にしません
- PDF 内の JavaScript、リンク、外部参照に関わる機能は、アプリ側で安全に抑制または通知する前提です
- PDFium 更新は手動作業とし、自動更新機構は入れません

## `md4c/`

### 役割

MD4C はノート本文の Markdown 解析依存です。本アプリでは Markdown を内部の note document/model へ変換するために使います。HTML 生成器としては使いません。

### 保持対象

- `src/md4c.c`
  - build に直接組み込む C 実装
- `src/md4c.h`
  - アプリ側 adapter が参照する公開ヘッダ
- `LICENSE.md`
  - MD4C の MIT ライセンス文書
- `README.md`, `CHANGELOG.md`
  - 取得元・仕様・履歴確認用の補助文書
- `src/md4c-html.*`, `src/entity.*`, `src/CMakeLists.txt`, `*.pc.in`
  - 現時点では実行には使わない upstream 由来ファイルです

### ビルド/実行

- `scripts/build/build_sources.json` は `third_party/md4c/src/md4c.c` を build source に含めます
- `scripts/build/build_workspace.ps1` は `third_party/md4c/src` を include path に入れます
- `md4c.c` は `MD4C_USE_UTF16` を付けてコンパイルします
- MinGW で UTF-16 経路を使うため、`src/md4c.c` には局所パッチがあります

### 配布

- MD4C はアプリ本体バイナリに組み込まれます
- `scripts/release/pack_release.ps1` は `third_party/md4c/LICENSE.md` を release の `licenses/md4c/LICENSE.md` へコピーします

### 安全上の扱い

- MD4C はローカル文字列解析だけに使います
- Markdown 内 URL を解析しても、アプリ側で外部通信や外部起動へつなげません
- 解析失敗や未対応構文は、保存や原本変更に進めず、静かな視覚通知または内部ログで扱います

## `libreoffice/`

### 役割

LibreOffice は Office 文書を PDF に変換するためのローカル変換エンジンです。対象は docx / pptx など、PDFium が直接読めない Office 系入力を、アプリ管理下の一時コピーから PDF へ変換する経路です。また、`image/Fonts/` 配下から選んだ欧文・記号フォントはアプリ起動時に Win32 の private font としてプロセス内に読み込みます。

### 保持対象

- `VERSION`
  - LibreOffice のバージョン、取得元 MSI、ハッシュなどの証跡
- `README.md`
  - headless 実行条件、削除済みファイル、使用時の安全条件
- `image/`
  - LibreOffice Windows x86_64 管理者展開イメージ
- `image/program/soffice.com`
  - アプリから呼ぶ想定の headless 変換入口
- `image/program/soffice.exe`
  - GUI-capable entry point。アプリからは使わない方針
- `image/license.txt`, `image/LICENSE.html`, `image/NOTICE`
  - LibreOffice 本体と同梱コンポーネントのライセンス/通知

### ローカル変更

外部通信禁止と送信機能排除のため、管理者展開イメージから更新/送信系ファイルを削除済みです。削除対象を増減した場合は、この一覧、`third_party/libreoffice/README.md`、`THIRD_PARTY_NOTICES.md` を同じ作業で更新します。

### 実行条件

LibreOffice をアプリから呼ぶ場合は、次を必須条件にします。

- ユーザー原本を直接変換対象にせず、アプリ管理下のコピーだけを入力にする
- 出力先はアプリ管理下の一時フォルダに限定する
- `--headless --nologo --nodefault --nolockcheck --nofirststartwizard --norestore` を使う
- `-env:UserInstallation=file:///...` で専用 profile を指定する
- `PYTHONDONTWRITEBYTECODE=1` を設定する
- `PYTHONPYCACHEPREFIX` は Git 追跡外のアプリ管理フォルダに向ける
- 変換後 PDF は PDFium で検証してからアプリ側に渡す
- 失敗、タイムアウト、検証失敗では「保存しない」で終了し、原本を保持する

### 配布

- `scripts/release/pack_release.ps1` は `image/Fonts/` の選択 subset を `libreoffice/image/Fonts/` へコピーします
- 対応する `license.txt`, `LICENSE.html`, `NOTICE` は `licenses/libreoffice/` へコピーします
- `-IncludeLibreOfficeRuntime` を指定した配布物だけが、`libreoffice/custom_runtime/instdir/` に Office 変換 runtime を含みます
- release 同梱前に、外部通信、自動更新、クラッシュ送信、外部リンク参照、マクロ、音の発生がないことを実機で確認します

### 安全上の扱い

- LibreOffice はローカル headless 変換専用です
- 通常ユーザーの LibreOffice profile は使いません
- マクロ実行、外部リンク更新、テンプレート取得、オンラインヘルプ、更新確認は使いません
- 変換プロセスが固まる可能性を前提に、タイムアウトと後始末を実装します

## `mingw_runtime_licenses/`

### 役割

MinGW-w64 runtime DLL を release に同梱するためのライセンス証跡です。DLL 本体はこのディレクトリには置かず、ビルド時にローカル toolchain から取得します。

### 保持対象

- `mingw-w64/gcc/COPYING3.txt`
  - GCC runtime に関わる GPLv3 文書
- `mingw-w64/gcc/COPYING.RUNTIME.txt`
  - GCC Runtime Library Exception
- `mingw-w64/winpthreads/COPYING.txt`
  - winpthreads のライセンス文書

### ビルド/実行

- `scripts/build/build_workspace.ps1` は `g++` のある toolchain directory から runtime DLL を `out/bin/` へコピーします

### 配布

- `scripts/release/pack_release.ps1` は runtime DLL を release ルートへコピーします
- 対応するライセンス文書は `licenses/mingw-w64/` と `licenses/zlib/` へコピーします

## 判断メモ

- `third_party/` のサイズ削減は目的ではなく、説明可能性と安全性を優先します
- release に入るものは、実行に必要な最小構成とライセンス文書に絞ります
- repo に保持するものは、再現性、監査、ライセンス確認、ローカルパッチ確認に必要な最小構成に絞ります
- 大きな依存を追加する場合は、`.gitignore` と配布スクリプトを先に確認し、追跡対象と release 対象を混同しないようにします
