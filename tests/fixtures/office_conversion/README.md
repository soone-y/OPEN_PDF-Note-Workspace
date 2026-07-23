# Office conversion fixtures

This directory contains the tracked Office conversion fixture set used to validate
the reduced LibreOffice runtime. It includes four `.docx` and four `.pptx` files
covering charts, tables, fonts, shapes, images, SmartArt, equations, multilingual
text, fields, content controls, and embedded objects.

Keep the source Office files unchanged. Generated PDFs remain local-only and are
written under `.local\repo_resource\tmp`.

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\scripts\run_office_conversion_fixture_tests.ps1
```

The test converts all `.docx` and `.pptx` files under this directory with the bundled local LibreOffice image and verifies that each output is a PDF. Generated outputs are written under `.local\repo_resource\tmp` and removed by default.

Use `-Keep` to inspect the converted PDFs:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\scripts\run_office_conversion_fixture_tests.ps1 -Keep
```

Kept output directories include `conversion_manifest.json`, which records the source file, output PDF path, PDF size, SHA-256, OOXML font hints, Word layout hints such as `w:docGrid`, and embedded PDF font names. This helps track layout-quality issues such as Japanese auto line-wrap differences.

The app conversion path now stages DOCX/PPTX packages byte-for-byte. It does not rewrite font names, font sizes, or document text before LibreOffice opens the staged copy. LibreOffice can still substitute a font at render time when the requested family is unavailable, so line wrapping should be checked visually for important documents.

For DOCX samples where LibreOffice splits Japanese words immediately after an ideographic space, compare this experimental conversion:

```powershell
python tools\libreoffice\libreoffice_smoke_test.py --input-dir tests\fixtures\office_conversion --require-docx --require-pptx --docx-space-protection word-joiner-token-after-space --keep
```

This modifies only the temporary conversion copy by inserting zero-width `U+2060 WORD JOINER` characters inside tokens that follow `U+3000` ideographic spaces. Use it only as a layout probe until the result is validated for the target documents.

Do not put confidential or copyrighted samples here unless they are safe to keep locally.
