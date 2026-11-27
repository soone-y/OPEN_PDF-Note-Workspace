# 第三者コンポーネント

配布物には PDFium（package `160.0.8179.0`）、MD4C（`0.6.0+git.f9e5eb2`）、MicroTeX（openmath `086f4eb`）、XITS Math、zlib（`1.4`）、MinGW-w64 runtime、および LibreOffice 由来の部品を含みます。LibreOffice は、通常版では変換用の custom runtime `26.2.7.3` を `lo/` に、Lite版では同梱フォントだけを `libreoffice/image/Fonts/` に含みます。同梱フォントとライセンス/NOTICE 文書（`licenses/libreoffice/`）は、どちらの版も LibreOffice `26.2.3.2` の管理者展開物から取り出したものです。各ライセンスの原文は配布物の `licenses/` に同梱します。ここでの説明より各原文を優先してください。

通常版のLibreOffice改変は、長いlocal path対応の `0051` を含むpatch一式として `licenses/libreoffice/custom_build/patches/` に同梱します。元のMPL/Apache表示を保持します。

## MicroTeX と XITS Math

数式組版には MicroTeX の openmath 系統、固定コミット `086f4eb740270b28bd0c61a0a359aea9300d61ae` を改変して組み込みます。MIT ライセンスの原文と著作権表示を `licenses/microtex/LICENSE.txt` に同梱します。変更内容と出典はソースの `third_party/microtex/README.md` に記録します。

数式フォントは XITS Math のアウトラインとメトリクスを含む `resources/math/xits/XITSMath-Regular.clm2` です。SIL Open Font License 1.1 の原文、著作権表示、予約名、FONTLOG、README を `licenses/xits/` に保持します。フォント単独での販売を行わず、フォントの派生物には OFL を維持します。アプリ本体のライセンスは変更されません。Windows の日本語フォントは端末上のものを利用し、フォントファイルを再配布しません。
