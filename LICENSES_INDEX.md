# PDF Note Workspace ライセンス整理表

本書は、`PDF Note Workspace` の本体ライセンス、第三者コンポーネント、ランタイム DLL、LibreOffice 同梱物、フォント関連のライセンスを漏らさないための整理表です。適宜更新します。

- 本体コードのライセンス本文は `LICENSE.md` を正とします。
- 第三者コンポーネントの配布・同梱・参照情報は `THIRD_PARTY_NOTICES.md` を正とします。
- 各ライセンスの法的に有効な本文は、各 `licenses/` 配下または各第三者配布物に含まれる原文を正とします。
- 本書は配布物に入れるべきライセンス文書の索引・確認表であり、各ライセンス本文の代替ではありません。

release パッケージを受け取った利用者は、`docs/` と `licenses/` 配下だけを見れば追跡できるようにしてあります。本書中の `third_party/...` は、リポジトリ上の元配置確認用です。

## 1. 本体ライセンス

| 対象 | ライセンス | 著作権表示 | 対象範囲 | 対象外 | 配布時の扱い |
| --- | --- | --- | --- | --- | --- |
| `PDF Note Workspace` 本体 | zlib License | `Copyright (c) 2026 Soone-Y` | `src/`, `scripts/`, `tests/`, `docs/` の本体コードと文書、および別ライセンス表記のない同梱素材 | `third_party/` 配下、利用者環境のフォント、ローカルツールチェーン由来 DLL | `docs/LICENSE.md` を release に同梱する |

## 2. 配布物に含めるライセンス文書一覧

| 区分 | 配布先の例 | 含めるファイル | 用途 |
| --- | --- | --- | --- |
| 本体 | `docs/LICENSE.md` | `LICENSE.md` | 本体 zlib License |
| ライセンス索引 | `docs/LICENSES_INDEX.md` | `LICENSES_INDEX.md` | 配布物と license 文書の対応表 |
| 第三者通知 | `docs/THIRD_PARTY_NOTICES.md` | `THIRD_PARTY_NOTICES.md` | 第三者コンポーネントの一覧と扱い |
| PDFium パッケージ | `licenses/pdfium/LICENSE` | `third_party/pdfium/LICENSE` | PDFium 配布パッケージ自体の MIT 系ライセンス |
| PDFium 本体・依存 | `licenses/pdfium/` | `third_party/pdfium/licenses/*.txt`, `*.md`, `*.ijg` | PDFium と PDFium 同梱コンポーネントのライセンス原文 |
| MinGW-w64 runtime | `licenses/mingw-w64/gcc/` | `COPYING3.txt`, `COPYING.RUNTIME.txt` | GCC runtime / GPLv3 / GCC Runtime Library Exception |
| MinGW-w64 winpthreads | `licenses/mingw-w64/winpthreads/` | `COPYING.txt` | winpthreads の MIT + BSD 系 notice |
| zlib runtime | `licenses/zlib/zlib.txt` | `zlib.txt` | zlib1.dll の zlib License |
| MD4C | `licenses/md4c/LICENSE.md` | `third_party/md4c/LICENSE.md` | MD4C の MIT License |
| LibreOffice | `licenses/libreoffice/` | `license.txt`, `LICENSE.html`, `NOTICE` | LibreOffice 本体、第三者コンポーネント、Apache NOTICE、フォント notice |

## 3. 第三者コンポーネント一覧

| コンポーネント | 主な用途 | 同梱/利用形態 | 主なライセンス | 注意度 | 必要な対応 |
| --- | --- | --- | --- | --- | --- |
| PDFium package `145.0.7568.0` | PDFium バイナリ/ヘッダのパッケージ | `third_party/pdfium/`、release では `pdfium.dll` | MIT 系 | 低 | `third_party/pdfium/LICENSE` を同梱 |
| PDFium 本体 | PDF 読み込み、描画、テキスト抽出、書き出し | `pdfium.dll` | BSD 3-Clause + Apache 2.0 系 | 中 | `pdfium.txt` と関連 license 群を同梱 |
| Abseil | PDFium 依存 | PDFium 同梱 | Apache License 2.0 | 中 | LICENSE、NOTICE がある場合は NOTICE、改変表示 |
| Anti-Grain Geometry / AGG 2.3 | PDFium 依存 | PDFium 同梱 | permissive notice | 低 | 著作権表示を保持 |
| fast_float | PDFium 依存 | PDFium 同梱 | MIT License | 低 | 著作権表示とライセンス文を保持 |
| FreeType | フォント描画関連 | PDFium / LibreOffice 依存で含まれる可能性 | FreeType Project License | 中 | FreeType 利用クレジットを保持 |
| ICU | Unicode / 国際化関連 | PDFium / LibreOffice 依存で含まれる可能性 | Unicode License V3 / ICU License + third-party notices | 中 | ICU ライセンスと同梱 notice を保持 |
| Little CMS / lcms2 | 色管理 | PDFium / LibreOffice 依存で含まれる可能性 | MIT 系 | 低 | 著作権表示とライセンス文を保持 |
| libjpeg-turbo / IJG | JPEG 画像処理 | PDFium / LibreOffice 依存で含まれる可能性 | IJG License + Modified BSD 3-Clause + zlib | 中 | IJG 表示文、BSD 文、改変時表示を保持 |
| libopenjpeg | JPEG 2000 関連 | PDFium 依存 | BSD 2-Clause | 低 | 著作権表示と免責文を保持 |
| libpng | PNG 画像処理 | PDFium / LibreOffice 依存で含まれる可能性 | PNG Reference Library License / zlib 系 | 低 | 出所詐称禁止、改変表示、通知保持 |
| libtiff | TIFF 画像処理 | PDFium / LibreOffice 依存で含まれる可能性 | permissive / BSD 風 | 低 | 著作権表示、名称の広告利用禁止 |
| LLVM libc | PDFium 依存 | PDFium 同梱 | Apache 2.0 with LLVM Exceptions | 中 | Apache 文、LLVM 例外、NOTICE がある場合は保持 |
| zlib | 圧縮 | PDFium 依存等 | zlib License | 低 | 出所詐称禁止、改変表示、通知保持 |
| MinGW-w64 runtime DLLs | C++/GCC runtime | `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` | GPLv3 + GCC Runtime Library Exception / MIT + BSD 系 | 中 | `COPYING3.txt`, `COPYING.RUNTIME.txt`, `COPYING.txt` を同梱 |
| zlib runtime | DOCX staging copy の ZIP 展開/検証 | `zlib1.dll` | zlib License | 低 | `zlib.txt` を同梱 |
| MD4C | Markdown 解析 | `third_party/md4c/src/md4c.c`, `md4c.h` をビルドに使用 | MIT License | 低 | `LICENSE.md` を同梱。局所パッチを管理 |
| LibreOffice `26.2.3.2` | 同梱フォントの private font 利用。docx/pptx から PDF への headless 変換 | `third_party/libreoffice/image/` は比較・フォント・license 参照用。変換エンジン本体は検証済みカスタム runtime を release 配置または環境変数で指定 | MPL 2.0 中心 + LGPL/GPL/MPL/Apache 等多数 | 中〜高 | `license.txt`, `LICENSE.html`, `NOTICE` を同梱。runtime を含める場合は同梱 runtime 自身の文書を使い、改変 patch を提示可能にする |
| LibreOffice 同梱フォント | 英語・記号向けフォント候補 | `FR_PRIVATE` でプロセス内読み込み | LibreOffice の各フォント notice に従う | 中 | `license.txt`, `LICENSE.html`, `NOTICE` を同梱。再配布範囲を選択フォントに限定 |
| Windows / OS フォント | 日本語表示、注釈、PDF 書き出し | 利用者環境にインストール済みのフォントをフェイス名で利用 | 各 OS / 各フォントのライセンス | 中 | フォントファイルを本リポジトリの権利として扱わない。再配布権を与えない |

## 4. PDFium 関連ライセンス原文チェックリスト

PDFium を配布する場合、少なくとも次を release の `licenses/pdfium/` に含めます。

| ファイル | 対象 | ライセンス/内容 |
| --- | --- | --- |
| `LICENSE` | PDFium package | MIT 系、`Copyright 2014-2025 Benoit Blanchon` |
| `pdfium.txt` | PDFium 本体 | BSD 3-Clause + Apache 2.0 |
| `abseil.txt` | Abseil | Apache License 2.0 |
| `agg23.txt` | Anti-Grain Geometry | permissive notice |
| `fast_float.txt` | fast_float | MIT License |
| `freetype.txt` | FreeType | FreeType Project License |
| `icu.txt` | ICU | Unicode License V3、ICU License、同梱 third-party notices |
| `lcms.txt` | Little CMS | MIT License |
| `libjpeg_turbo.md` | libjpeg-turbo | IJG / Modified BSD 3-Clause / zlib の整理 |
| `libjpeg_turbo.ijg` | IJG JPEG | IJG notice |
| `libopenjpeg.txt` | libopenjpeg | BSD 2-Clause |
| `libpng.txt` | PNG | PNG Reference Library License |
| `libtiff.txt` | TIFF | permissive license |
| `llvm-libc.txt` | LLVM libc | Apache 2.0 with LLVM Exceptions |
| `zlib.txt` | zlib | zlib License |

## 5. MinGW-w64 runtime チェックリスト

MinGW-w64 の DLL を release に含める場合、次を同梱します。

| DLL / 対象 | ライセンス文書 | 内容 | 実務上の判断 |
| --- | --- | --- | --- |
| `libstdc++-6.dll` | `COPYING3.txt`, `COPYING.RUNTIME.txt` | GPLv3 + GCC Runtime Library Exception | 通常の GCC コンパイル成果物としての利用なら、自作アプリ全体を GPL にする趣旨ではない |
| `libgcc_s_seh-1.dll` | `COPYING3.txt`, `COPYING.RUNTIME.txt` | GPLv3 + GCC Runtime Library Exception | 同上 |
| `libwinpthread-1.dll` | `COPYING.txt` | MIT + BSD 系 notice | 著作権表示・免責文を保持 |
| `zlib1.dll` | `zlib.txt` | zlib License | DOCX staging copy の ZIP 展開/検証で利用 |

## 6. MD4C チェックリスト

| 項目 | 内容 |
| --- | --- |
| 使用箇所 | Markdown 解析 |
| 使用ファイル | `third_party/md4c/src/md4c.c`, `third_party/md4c/src/md4c.h` |
| 不使用方針 | `md4c-html.*` は現時点では使用前提にしない |
| ローカル変更 | `MD4C_USE_UTF16` を MinGW で使うため、wide literal 化の局所パッチを 1 箇所適用 |
| ライセンス | MIT License、`Copyright © 2016-2024 Martin Mitáš` |
| 配布対応 | `licenses/md4c/LICENSE.md` を同梱 |

## 7. LibreOffice チェックリスト

| 項目 | 内容 |
| --- | --- |
| バージョン | LibreOffice `26.2.3.2` Windows x86_64 管理者展開イメージ、および A経路カスタム build |
| 取得元 MSI | `LibreOffice_26.2.3_Win_x86-64.msi` |
| SHA256 | `468D1FB3880AF3BCDDAC002E9054155912C70B45D105BFA1C82036F33456133D` |
| 主用途 | 同梱フォントの private font 利用。docx/pptx から PDF への headless 変換 |
| 主ライセンス | MPL 2.0 ベース。第三者コンポーネントとして LGPL/GPL/MPL/Apache/SIL OFL/LPPL/CC-BY-SA 等を含む |
| 必須同梱文書 | `license.txt`, `LICENSE.html`, `NOTICE` |
| release 現状 | 標準 release は選択フォント、検証済みカスタム runtime、runtime 側 license/NOTICE、custom build 入力を同梱。変換機能を含めない軽量配布は `-Lite` を指定する。runtime は配布前に SDK、ローカルビルドパス、更新 URL を除去 |

### 7.0.1 LibreOffice license/notice の採用基準

- 選択フォントだけを release に含める場合でも、LibreOffice の `license.txt`, `LICENSE.html`, `NOTICE` は保持する。
- 公式管理者展開イメージ `third_party/libreoffice/image` は外部通信可能な import/marker が残るため、Office 変換 runtime として採用しない。
- 標準 release は検証済みカスタム runtime を `libreoffice/custom_runtime/instdir/` へコピーし、その `instdir` に存在する `license.txt`, `LICENSE.html`, `NOTICE` を `licenses/libreoffice/` へコピーする。変換機能を含めない軽量配布は `-Lite` を指定する。`-NoLibreOfficeRuntime` は `-Lite` と併用する変換なし配布用の指定であり、標準 release では使用できない。
- release にコピーした runtime は、`tools/release_checks/sanitize_libreoffice_runtime_release.py` により `sdk/` を除外し、ローカル絶対パス/ビルドユーザー名を等長置換し、`program/version.ini` の更新 URL を空にする。
- カスタム build は LibreOffice source に patch を当てているため、`third_party/libreoffice/custom_build/patches/*.patch` と `communication_free_options.input` を、改変内容と build 条件を示す再現入力として保持する。
- `scripts/release/pack_release.ps1` は、release 内に `soffice.com` または `mergedlo.dll` を検出した場合、bundled runtime 側の LibreOffice license 文書を `licenses/libreoffice/` へコピーし、custom build options/patches も `licenses/libreoffice/custom_build/` へコピーする。
- release に含めない Help、辞書、Basic script、UI license dialog 等の個別 license 表示はコピーしない。対応する実体ファイルを同梱する場合だけ、該当 license/notice を追加する。
- 不要 license の削除は、対応する実体が release に含まれないことを確認してから行う。現時点で Git 管理している LibreOffice license 文書3件は削除しない。

### 7.1 LibreOffice 管理者展開イメージから削除済みの主な系統

`THIRD_PARTY_NOTICES.md` の方針に従い、外部通信・更新・送信・クラッシュ報告・不要 UI・不要 launcher・不要 script/template 類を削除済みとして管理します。

| 系統 | 削除例 |
| --- | --- |
| 更新/送信/クラッシュ報告 | `updater.exe`, `update_service.exe`, `updatecheckuilo.dll`, `senddoc.exe`, `minidump_upload.exe`, `mar.exe`, `onlineupdate.xcd` など |
| PDF import / publish / solver | `xpdfimport.exe`, `share/xpdfimport/`, `wiki-publisher/`, `nlpsolver/` |
| standalone launcher | `sbase.exe`, `scalc.exe`, `sdraw.exe`, `simpress.exe`, `smath.exe`, `swriter.exe`, `soffice_safe.exe`, `unopkg.exe`, `uno.exe`, `unoinfo.exe` など |
| GUI/resource/script/template | `share/gallery/`, `share/tipoftheday/`, `program/wizards/`, `share/wizards/`, `share/Scripts/`, `share/basic/`, `share/template/` |

### 7.2 LibreOffice 運用上の注意

- `image/program/libcurl.dll` は `mergedlo.dll` と `LanguageToollo.dll` の import 依存があるため保持します。
- 起動時は `PYTHONDONTWRITEBYTECODE=1` と `PYTHONPYCACHEPREFIX` を設定し、`third_party/` 配下に Python bytecode を生成させません。
- 変換前後に LibreOffice image 配下の `__pycache__` を削除し、変換生成物を `third_party/` に残しません。
- `-env:UserInstallation=file:///...` で専用 profile を指定し、利用者の通常 LibreOffice profile を使いません。
- release 同梱前に、外部通信経路、マクロ、外部リンク/リモート画像参照、クラッシュレポート、更新確認が実行されないことを実機確認します。

## 8. フォント整理

| フォント区分 | 利用方法 | ライセンス上の扱い | 注意 |
| --- | --- | --- | --- |
| Windows 標準フォント | `Meiryo`, `Meiryo UI` 等をフェイス名で利用 | 利用者の Windows ライセンスに従う | フォントファイルを本アプリの同梱物として再配布しない |
| LibreOffice 同梱フォント | `FR_PRIVATE` でプロセス内読み込み | LibreOffice の `license.txt` / `LICENSE.html` / `NOTICE` に従う | release では選択フォントと対応ライセンス文書を同梱 |
| 環境に存在すれば使う候補 | `Noto Sans JP`, `Source Han Sans JP`, `Hiragino Sans` 等 | 各利用者環境のフォントライセンスに従う | 本リポジトリが再配布権を与えるものではない |
| PDF 書き出し時に埋め込まれる可能性のあるフォント | GDI / Windows レジストリから取得し PDFium へ渡す場合あり | 書き出し元マシンの各フォントライセンスに従う | 埋め込み可否はフォントごとの条件に従う。失敗時はビットマップ描画へフォールバック |

## 9. ライセンスリスク整理

| リスク | 該当候補 | 方針 |
| --- | --- | --- |
| GPL の直接リンクによる影響 | poppler 等、LibreOffice 内の GPL 系部品 | 自作アプリ本体へ直接リンクしない。LibreOffice は外部/headless 変換器として扱う |
| LGPL のリンク条件 | LibreOffice 内部ライブラリ、GPGME 等 | 直接リンクを避ける。必要な場合は動的リンク・再リンク可能性・改変ソース提供条件を個別確認 |
| MPL のファイル単位 copyleft | LibreOffice 本体、MPL 系 import ライブラリ | 改造配布時は改変ファイルの扱いを確認。未改造外部利用なら影響を限定しやすい |
| Apache NOTICE 漏れ | Abseil、LibreOffice NOTICE、OpenSSL 等 | NOTICE を削除せず、`licenses/` に同梱 |
| フォント再配布 | Windows フォント、環境依存フォント | フォントファイルを勝手に同梱しない。LibreOffice 同梱フォントは対応 notice とセットで扱う |
| 変換時の外部通信 | LibreOffice 更新確認、クラッシュ報告、外部リンク等 | 不要ファイル削除、専用 profile、実機確認 |

## 10. 標準 release パッケージの構成

以下は Office 変換 runtime を含む標準 release の構成です。`-Lite`、`-NoSampleWorkspace`、`-NoSetupJson`、`-IncludeWorkspace` などの梱包オプションを指定する場合は、一部の項目が省略または置換されます。

```text
PDF Note Workspace release/
  readonly_viewer.exe
  readonly_viewer.exe.buildinfo.txt
  pdf_note_workspace.exe
  pdf_note_workspace.exe.buildinfo.txt
  pdf_workspace_setup.json
  pdfium.dll
  libstdc++-6.dll
  libgcc_s_seh-1.dll
  libwinpthread-1.dll
  zlib1.dll
  libreoffice/
    image/
      Fonts/
        # release で同梱する選択フォントのみ
    custom_runtime/
      instdir/
        # 標準 release で同梱する検証済み Office 変換 runtime
  sample_workspace/
  docs/
    README.md
    LICENSE.md
    LICENSES_INDEX.md
    THIRD_PARTY_NOTICES.md
    CONTENTS.txt
  licenses/
    pdfium/
      LICENSE
      abseil.txt
      agg23.txt
      fast_float.txt
      freetype.txt
      icu.txt
      lcms.txt
      libjpeg_turbo.md
      libjpeg_turbo.ijg
      libopenjpeg.txt
      libpng.txt
      libtiff.txt
      llvm-libc.txt
      pdfium.txt
      zlib.txt
    mingw-w64/
      gcc/
        COPYING3.txt
        COPYING.RUNTIME.txt
      winpthreads/
        COPYING.txt
    md4c/
      LICENSE.md
    zlib/
      zlib.txt
    libreoffice/
      license.txt
      LICENSE.html
      NOTICE
      custom_build/
        communication_free_options.input
        release_reduction_manifest.json
        patches/
          # カスタム build に適用した patch 一式
  manifest.json
  checksums.sha256
```

標準 release は `libreoffice/custom_runtime/instdir/` を同梱し、runtime 側の `license.txt`, `LICENSE.html`, `NOTICE`、`communication_free_options.input`、`release_reduction_manifest.json`、custom build の patch 一式も必ず含めます。配布 runtime は SDK とローカルビルド情報を含めず、更新 URL を空にします。変換機能を含めない軽量配布は `-Lite` を指定します。`-NoLibreOfficeRuntime` は `-Lite` と併用する変換なし配布用の指定であり、標準 release では使用できません。

## 11. 追加時ルール

新しい外部依存物、フォント、画像、辞書、サンプル、生成物を追加するときは、次を同時に更新してください。

1. `THIRD_PARTY_NOTICES.md` のコンポーネント表。
2. `licenses/<component>/` への原文ライセンス配置。
3. `scripts/release/pack_release.ps1` のコピー対象。
4. 本書の該当表。
5. 外部由来素材の場合、出所 URL、取得日、バージョン、ハッシュ、改変有無。
6. 配布物の中身と表記を一致。
