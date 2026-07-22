# ライセンス情報

本リポジトリ `PDF Note Workspace` の本体コードおよび別ライセンス表記のない同梱素材は、zlib License で提供します。
`third_party/` 配下の外部依存物、利用者の環境にあるフォント、ローカルのツールチェーンから取得するランタイム DLL は、このライセンスの対象外です。これらは `THIRD_PARTY_NOTICES.md` を参照してください。

release パッケージを受け取った利用者は、まず `docs/LICENSE.md`, `docs/LICENSES_INDEX.md`, `docs/THIRD_PARTY_NOTICES.md`, `licenses/` を参照してください。本書中の `third_party/...` 表記は、リポジトリ上の元配置を示します。

## 1. 本プロジェクトのライセンス

- ライセンス: `zlib License`
- 著作権表示: `Copyright (c) 2026 Soone-Y`
- 対象:
  - `src/`, `scripts/`, `tests/`, `docs/` の本体コードと文書
  - 別ライセンス表記のないリポジトリ同梱素材
- 対象外:
  - `third_party/` 配下の外部依存物とそのライセンス文書
  - 利用者の Windows / ツールチェーン / ローカル環境に属するフォントや DLL

ライセンス本文:

```text
zlib License

Copyright (c) 2026 Soone-Y

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

参考和訳:

以下の日本語訳は理解補助のための参考訳です。解釈に差異がある場合は、上記の英語原文を優先します。

```text
zlib License

Copyright (c) 2026 Soone-Y

このソフトウェアは、明示または黙示を問わず、いかなる保証もなく
「現状のまま」提供されます。
本ソフトウェアの使用によって生じたいかなる損害についても、
作者は一切の責任を負いません。

営利目的を含むあらゆる目的で本ソフトウェアを使用、改変、
再配布することを許可します。以下の制限に従うものとします。

1. 本ソフトウェアの出所を偽ってはなりません。
   あなたが原作者であると主張してはなりません。
   本ソフトウェアを製品で使用する場合、製品文書に謝辞を記載することは
   望ましいですが、必須ではありません。
2. 改変したソース版は、改変版であることを明確に表示しなければならず、
   原版であると偽ってはなりません。
3. この表示は、いかなるソース配布物からも削除または改変してはなりません。
```

## 2. フォントと素材の扱い

### フォント

- 画面表示と通常の描画は、原則として実行環境にインストール済みのフォントをフェイス名で指定して行います（Windows標準フォントなど）。
- 一部、LibreOffice に同梱されている欧文・記号フォントを `third_party/libreoffice/image/Fonts/` に保持し、アプリ起動時にプロセス専用（private font）として読み込んで利用します。このフォントのライセンスは LibreOffice の利用条件に従います。
- 既定の候補名は `src/core/font_list.h` に定義されており、Windows 系フォントや上記 LibreOffice 側フォントに加えて、`Noto Sans JP`、`Source Han Sans JP`、`Hiragino Sans` など「その環境に存在する場合だけ使える」名前も含みます。
- 標準テキスト注釈付き PDF 書き出しでは、書き出し元マシン上のフォントデータを Windows レジストリ/GDI から取得して PDFium に渡し、実フォントとして埋め込める場合があります。失敗時はビットマップ描画へフォールバックします。
- したがって、書き出し PDF に含まれるフォントデータのライセンスは「このリポジトリ」ではなく、「書き出しを行ったマシンにインストールされている各フォント」または「同梱されている LibreOffice フォント」の利用条件に従います。

### 同梱素材

- 別ライセンス表記のないリポジトリ同梱素材は、本プロジェクト素材として、配布上は本体と同じ zlib License で扱います。
- 外部由来の素材を追加する場合は、その時点で出所とライセンスを `THIRD_PARTY_NOTICES.md` か別紙へ追記してください。

## 3. サードパーティ依存の参照先

- release パッケージでの参照先:
  - `docs/THIRD_PARTY_NOTICES.md`
  - `licenses/pdfium/`
  - `licenses/mingw-w64/`
  - `licenses/md4c/`
  - `licenses/zlib/`
  - `licenses/libreoffice/`
- リポジトリ上の元配置:
  - `third_party/pdfium/LICENSE`
  - `third_party/pdfium/licenses/`
  - `third_party/mingw_runtime_licenses/mingw-w64/`
  - `third_party/md4c/LICENSE.md`
  - `third_party/libreoffice/image/license.txt`, `third_party/libreoffice/image/LICENSE.html`, `third_party/libreoffice/image/NOTICE`

## 4. 個人、教育機関、その他団体などにおける利用

- いかなる団体また個人も、本プロジェクトのライセンスに則って特段の許可を得ること無く利用できます。
- ライセンス表記に同意できない場合は利用できません。
- ライセンスの許す限りの資源、例えばライセンス表記自体などを教育目的に利用することもできます。
