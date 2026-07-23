これは、LibreOffice runtime による Office-to-PDF 変換結果の公開サンプルです。

- `PPTX_図形・画像・SmartArt総合検証_変換結果.pdf`: 図形、画像、SmartArt、WordArt、グループ化、グラデーションを含む PowerPoint 入力の変換結果です（11ページ）。
- `Word_埋め込みオブジェクト総合検証_変換結果.pdf`: Word のグラフ、埋め込みExcel、SmartArt、WordArt などを含む Word 入力の変換結果です（7ページ）。

どちらも `tests/fixtures/office_conversion` にある検証用 Office ファイルを用いた変換品質検証で確認・編纂した確定PDFです。Office の元ファイルは配布せず、PDF のみを収録しています。

検証用に作成した合成資料であり、個人情報、実在の講義資料、外部由来の文章・画像は含めていません。これらは配布用の確定サンプルであり、以後の変換品質向上に伴う積極的な差し替えは行いません。

This folder contains two finalized public PDFs curated through conversion-quality validation: one from PowerPoint and one from Word. The source Office files remain in `tests/fixtures/office_conversion`; only the PDFs are distributed here. These are fixed distribution samples and will not be proactively replaced as conversion quality improves.
