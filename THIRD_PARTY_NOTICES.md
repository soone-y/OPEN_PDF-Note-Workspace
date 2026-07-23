# Third-Party Notices

本書は、`PDF Note Workspace` が利用または配布時に同梱する第三者コンポーネントの実態を整理したものです。
プロジェクト本体と、別ライセンス表記のないリポジトリ同梱素材のライセンスは `LICENSE.md` を参照してください。

release パッケージ利用時は、まず `docs/THIRD_PARTY_NOTICES.md` と `licenses/` 配下を参照してください。本書中の `third_party/...` は、リポジトリ上の元配置を示します。

## 1. 同梱/配布する第三者コンポーネント

| コンポーネント | 用途 | 同梱形態 | ライセンス参照 |
| --- | --- | --- | --- |
| PDFium package `145.0.7568.0` | PDF の読み込み、描画、テキスト抽出、書き出し | `third_party/pdfium/` を同梱。release では `pdfium.dll` と対応ライセンス文書を配布 | `third_party/pdfium/LICENSE`, `third_party/pdfium/licenses/` |
| MD4C | Markdown 解析 | `third_party/md4c/` をリポジトリへ配置。導入時は `md4c.c`, `md4c.h`, `LICENSE.md` を参照 | `third_party/md4c/LICENSE.md` |
| LibreOffice `26.2.3.2` | 同梱フォントのアプリ内 private font 利用。docx/pptx から PDF への headless 変換 | `third_party/libreoffice/image/` は比較・フォント・license 参照用。変換 runtime は通信機能を持たないカスタム build を標準 release へ配置する | `third_party/libreoffice/image/license.txt`, `third_party/libreoffice/image/LICENSE.html`, `third_party/libreoffice/image/NOTICE` |
| MinGW-w64 runtime DLLs | `pdf_note_workspace.exe` と `readonly_viewer.exe` 実行に必要な C++/GCC runtime | `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` を `out/bin/` と release へコピー | `third_party/mingw_runtime_licenses/mingw-w64/` |
| zlib runtime | DOCX staging copy の ZIP 展開/検証 | `zlib1.dll` を `out/bin/` と release へコピー | `third_party/pdfium/licenses/zlib.txt` |

## 2. PDFium

### 2.1 実装上の位置づけ

- ビルド時:
  - `third_party/pdfium/include/`
  - `third_party/pdfium/lib/pdfium.dll.lib`
- 実行時:
  - `third_party/pdfium/bin/pdfium.dll`
  - release では `pdfium.dll`
- バージョン:
  - `third_party/pdfium/VERSION`: `145.0.7568.0`
  - `third_party/pdfium/PDFiumConfig.cmake`: `145.0.7568.0`

### 2.2 ライセンス文書

- パッケージ配布物そのもののライセンス:
  - `third_party/pdfium/LICENSE`
  - 内容: MIT ライセンス系の文面
  - 著作権表示: `Copyright 2014-2025 Benoit Blanchon`
- PDFium 本体のライセンス:
  - `third_party/pdfium/licenses/pdfium.txt`
- PDFium 同梱コンポーネントのライセンス文書:
  - `third_party/pdfium/licenses/abseil.txt` - Apache License 2.0
  - `third_party/pdfium/licenses/agg23.txt` - Anti-Grain Geometry permissive notice
  - `third_party/pdfium/licenses/fast_float.txt` - MIT License
  - `third_party/pdfium/licenses/freetype.txt` - FreeType Project License
  - `third_party/pdfium/licenses/icu.txt` - Unicode License V3 と ICU 内包 notices
  - `third_party/pdfium/licenses/lcms.txt` - MIT License
  - `third_party/pdfium/licenses/libjpeg_turbo.md` / `libjpeg_turbo.ijg` - libjpeg-turbo / IJG notices
  - `third_party/pdfium/licenses/libopenjpeg.txt` - BSD 2-Clause
  - `third_party/pdfium/licenses/libpng.txt` - PNG Reference Library License
  - `third_party/pdfium/licenses/libtiff.txt` - libtiff permissive license
  - `third_party/pdfium/licenses/llvm-libc.txt` - Apache License 2.0 with LLVM Exceptions
  - `third_party/pdfium/licenses/zlib.txt` - zlib License

### 2.3 配布時の扱い

- `scripts/release/pack_release.ps1` は `third_party/pdfium/LICENSE` と `third_party/pdfium/licenses/` 配下の文書を release の `licenses/pdfium/` へコピーします。
- `pdfium.dll` を配布する場合は、上記ライセンス文書群も一緒に配布してください。


### 2.4 ローカル方針

- コンパイル済みの公式バイナリパッケージを直接配置して利用しており、本リポジトリ上でのソースビルドは行わないため、以下の標準ビルドツール用設定ファイルや過去のゴミファイルは削除済みです。
  - `args.gn`
  - `PDFiumConfig.cmake`
  - `include/fpdfview.h.orig`
## 3. MinGW-w64 runtime

### 3.1 実装上の位置づけ

- `full_build.ps1` は `scripts/build/build_workspace.ps1` 経由で、`g++` のあるツールチェーンから次の DLL を `out/bin/` にコピーします。
  - `libstdc++-6.dll`
  - `libgcc_s_seh-1.dll`
  - `libwinpthread-1.dll`
  - `zlib1.dll`
- `scripts/release/pack_release.ps1` は上記 DLL を release ルートへ配置します。

### 3.2 ライセンス文書

- GCC runtime:
  - `third_party/mingw_runtime_licenses/mingw-w64/gcc/COPYING3.txt` - GPLv3
  - `third_party/mingw_runtime_licenses/mingw-w64/gcc/COPYING.RUNTIME.txt` - GCC Runtime Library Exception
- winpthreads:
  - `third_party/mingw_runtime_licenses/mingw-w64/winpthreads/COPYING.txt` - MIT + BSD 系 notice
- zlib:
  - `third_party/pdfium/licenses/zlib.txt` - zlib License

### 3.3 配布時の扱い

- `scripts/release/pack_release.ps1` は上記ライセンス文書を release の `licenses/mingw-w64/` へコピーします。
- 上記 DLL を同梱して配布する場合は、対応するライセンス文書も同梱してください。

## 4. MD4C

### 4.1 実装上の位置づけ

- `third_party/md4c/` には vendor 化した `MD4C` を配置します。
- 保持対象は `src/`, `LICENSE.md`, `README.md`, `CHANGELOG.md` です。
- Markdown 解析移行では、現在次を build へ組み込み済みです。
  - `third_party/md4c/src/md4c.c`
  - `third_party/md4c/src/md4c.h`
- `scripts/build/build_workspace.ps1` は `third_party/md4c/src/md4c.c` を直接コンパイルします。
- `md4c-html.*` は現時点では使用前提にしません。
- `MD4C_USE_UTF16` を MinGW で使うため、`third_party/md4c/src/md4c.c` に wide literal 化の局所パッチを 1 箇所適用しています。

### 4.2 ライセンス文書

- `third_party/md4c/LICENSE.md`
  - MIT License
  - Copyright © 2016-2024 Martin Mitáš

### 4.3 配布時の扱い

- `MD4C` をビルドまたは release に含める場合は、対応するライセンス文書も一緒に保持・同梱してください。
- `scripts/release/pack_release.ps1` は `third_party/md4c/LICENSE.md` を release の `licenses/md4c/LICENSE.md` へコピーします。
- 現時点では source build と release パッケージの両方に反映済みです。


### 4.4 ローカル方針

- 当プロジェクトでは Markdown から AST への解析機能のみを利用し、HTML形式への出力機能やLinuxパッケージ管理、CMake構成は使用しないため、以下のファイルは削除済みです。
  - `src/CMakeLists.txt`
  - `src/md4c-html.c`, `src/md4c-html.h`
  - `src/entity.c`, `src/entity.h`
  - `src/md4c-html.pc.in`, `src/md4c.pc.in`
## 5. LibreOffice

### 5.1 実装上の位置づけ

- `third_party/libreoffice/image/` には LibreOffice 26.2.3.2 の Windows x86_64 管理者展開イメージを配置しています。ただし、この管理者展開イメージは外部通信可能な import/marker が残るため、Office 変換 runtime として release 採用しません。
- 取得元 MSI:
  - `LibreOffice_26.2.3_Win_x86-64.msi`
  - SHA256: `468D1FB3880AF3BCDDAC002E9054155912C70B45D105BFA1C82036F33456133D`
- ファイル取り込み時の docx/pptx 変換は、通信機能を持たないカスタム build runtime が見つかった場合に有効です。
- `image/Fonts/` 配下から選んだ欧文・記号フォントは、アプリ起動時に `AddFontResourceExW(..., FR_PRIVATE, ...)` でプロセス private font として読み込み、システムへインストールしません。
- headless 変換専用とし、ユーザー原本ではなくアプリ管理下のコピーを入力にします。
- release パッケージでは、アプリ側 private font 利用のため選択したフォントファイルを `libreoffice/image/Fonts/` に置き、検証済みカスタム runtime を `libreoffice/custom_runtime/instdir/` 配置で標準同梱し、LibreOffice ライセンス/NOTICE 文書を同梱します。変換機能を含めない軽量配布は `-Lite` を指定します。
- カスタム Office 変換 runtime を release に含める場合は、`scripts/release/pack_release.ps1` が同梱 runtime 自身の `license.txt`, `LICENSE.html`, `NOTICE` を `licenses/libreoffice/` へコピーし、`third_party/libreoffice/custom_build/communication_free_options.input`、`release_reduction_manifest.json`、`patches/*.patch` も `licenses/libreoffice/custom_build/` へコピーします。

### 5.2 ローカル方針

- 外部通信禁止方針に合わせ、次の更新/送信系ファイルは管理者展開イメージから削除済みです。
  - `image/LibreOffice_26.2.3_Win_x86-64.msi`
  - `image/program/updater.exe`
  - `image/program/update_service.exe`
  - `image/program/updatecheckuilo.dll`
  - `image/program/updater.ini`
  - `image/program/senddoc.exe`
  - `image/update-settings.ini`
  - `image/share/registry/onlineupdate.xcd`
  - `image/program/updchklo.dll`
  - `image/program/minidump_upload.exe`
  - `image/program/mar.exe`
  - `image/program/quickstart.exe`
  - `image/program/sweb.exe`
  - `image/program/mailmerge.py`
  - `image/program/classes/java_websocket.jar`
  - `image/**/__pycache__/`
- LibreOffice を Word/PowerPoint から PDF への headless 変換専用にするため、次の PDF import / publish / solver / standalone launcher 類も削除済みです。
  - `image/program/xpdfimport.exe`
  - `image/share/xpdfimport/`
  - `image/share/extensions/wiki-publisher/`
  - `image/share/extensions/nlpsolver/`
  - `image/program/sbase.exe`
  - `image/program/scalc.exe`
  - `image/program/sdraw.exe`
  - `image/program/simpress.exe`
  - `image/program/smath.exe`
  - `image/program/swriter.exe`
  - `image/program/soffice_safe.exe`
  - `image/program/gengal.exe`
  - `image/program/twain32shim.exe`
  - `image/program/unopkg.com`
  - `image/program/unopkg.exe`
  - `image/program/uno.exe`
  - `image/program/unoinfo.exe`
  - `image/program/regview.exe`
  - `image/program/odbcconfig.exe`
  - `image/program/opencltest.exe`
  - `image/program/spsupp_helper.exe`
  - `image/program/gpgme-w32spawn.exe`
- headless 変換で LibreOffice UI や bundled macro/script samples を使わないため、次の GUI/resource/script 類も削除済みです。
  - `image/share/gallery/`
  - `image/share/tipoftheday/`
  - `image/program/wizards/`
  - `image/share/wizards/`
  - `image/share/Scripts/`
  - `image/share/basic/`
- 新規 LibreOffice 文書作成は行わず、既存 Office ファイルの PDF 変換だけを行うため、次の template 類も削除済みです。
  - `image/share/template/`
- `image/program/libcurl.dll` は `mergedlo.dll` と `LanguageToollo.dll` の import 依存があるため保持します。
- 起動時は `PYTHONDONTWRITEBYTECODE=1` と `PYTHONPYCACHEPREFIX` を設定し、third_party 配下に Python bytecode を生成させないでください。
- 変換前後に LibreOffice image 配下の `__pycache__` を削除し、変換生成物を `third_party/` に残さないでください。
- `-env:UserInstallation=file:///...` で専用 profile を指定し、利用者の通常 LibreOffice profile を使わないでください。

### 5.3 ライセンス文書

- `third_party/libreoffice/image/license.txt`
- `third_party/libreoffice/image/LICENSE.html`
- `third_party/libreoffice/image/NOTICE`
- LibreOffice 本体は MPL 2.0 ベースの OSS で、同梱コンポーネントの個別 notice は上記文書に従います。
- `third_party/libreoffice/image/help/**`, `share/extensions/dict-en/**`, `share/basic/**` などに含まれる個別 license 表示は、それらの実体を release へ配布しない限り release へコピーしません。必要になった場合だけ、対象ファイルと一緒に対応する license/notice を追加します。

### 5.4 配布時の扱い

- `scripts/release/pack_release.ps1` は `third_party/libreoffice/image/Fonts/` から英語・記号向けの選択フォントだけを release の `libreoffice/image/Fonts/` へコピーし、上記ライセンス/NOTICE 文書を release の `licenses/libreoffice/` へコピーします。
- 標準 release には LibreOffice runtime 本体、runtime 側 license/NOTICE、custom build 入力を含めます。変換機能を含めない軽量配布は `scripts/release/pack_release.ps1 -Lite` を指定してください。`-NoLibreOfficeRuntime` は `-Lite` と併用する変換なし配布用の指定であり、標準 release では使用できません。
- release 同梱前に、外部通信経路、マクロ、外部リンク/リモート画像参照、クラッシュレポート、更新確認が実行されないことを実機で再確認してください。
- 不要になった license/notice を削る判断は、対応する実体ファイルを release から除外済みであることを確認してから行います。現時点で Git 管理している LibreOffice license 文書3件は、選択フォントの配布に対応するため保持します。

## 6. フォントとその他の素材

### 6.1 フォント

- LibreOffice 管理者展開イメージ内の `image/Fonts/` は、LibreOffice 変換品質維持とアプリ内表示候補のため保持します。
- 標準運用の日本語フォントは Windows 標準の `Meiryo` / `Meiryo UI` を使います。
- アプリは LibreOffice 同梱フォントを `FR_PRIVATE` でプロセス内に読み込み、英語・記号向けの選択候補として `Liberation Sans` / `Liberation Serif` / `Liberation Mono` / `Carlito` / `Caladea` / `OpenSymbol` を含めます。
- LibreOffice 側フォントは日本語既定フォントには使わず、日本語候補の後、Windows の Latin 候補より前に置きます。
- LibreOffice `license.txt` 内に同梱フォントの notice が含まれることを確認済みです。release では `licenses/libreoffice/` に `license.txt` / `LICENSE.html` / `NOTICE` を同梱します。
- 標準テキスト注釈付き PDF 書き出しでは、書き出し元マシン上のフォントデータを取得して PDF 内に使う場合があります。
- そのため、フォントの利用条件は各 OS / 各フォントライセンスに従います。本リポジトリが第三者フォントの再配布権を与えるものではありません。

### 6.2 リポジトリ同梱素材

本リポジトリのうち、別個の第三者ライセンス文書が付いていない同梱素材は、現行文書ではプロジェクト素材として `LICENSE.md` の対象に含めています。外部由来素材を追加する場合は、その時点で出所と条件を明記してください。
