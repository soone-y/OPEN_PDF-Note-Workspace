# LibreOffice

This directory contains a local LibreOffice administrative image for future
Office document to PDF conversion.

## Version

- Product: LibreOffice 26.2.3.2
- Windows package: `LibreOffice_26.2.3_Win_x86-64.msi`
- Windows MSI URL:
  `https://download.documentfoundation.org/libreoffice/stable/26.2.3/win/x86_64/LibreOffice_26.2.3_Win_x86-64.msi`
- Original MSI SHA256:
  `468D1FB3880AF3BCDDAC002E9054155912C70B45D105BFA1C82036F33456133D`

The downloaded MSI itself is not retained here. `image/` is the extracted
administrative image.


## Source Archives For Custom Build

The custom LibreOffice runtime is built from official LibreOffice 26.2.3.2 source archives, not from the Windows MSI administrative image.

Archive bodies and detached signatures are not tracked in Git because they are
large enough to interfere with normal GitHub synchronization. If the exact
source bundle needs to be republished, attach it as a release asset or publish
it as a separate source bundle instead of committing it to the repository.

The custom build source inputs are identified by these official archive names,
source URLs, and SHA-256 hashes:

The same information is also recorded in `source_archives_26.2.3.2.manifest.tsv` for line-based verification tooling.

The observed external tarball cache from the build workspace backup is recorded in `external_tarballs_26.2.3.2.manifest.tsv` with file names, sizes, and SHA-256 hashes. The tarball bodies are not tracked in Git.

| Archive | Source URL | SHA-256 |
| --- | --- | --- |
| `libreoffice-26.2.3.2.tar.xz` | `https://download.documentfoundation.org/libreoffice/src/26.2.3/libreoffice-26.2.3.2.tar.xz` | `254a641e0eec939364e157e2d9ddf4a55e1a42b5c688c22ce8e4945e97230a31` |
| `libreoffice-dictionaries-26.2.3.2.tar.xz` | `https://download.documentfoundation.org/libreoffice/src/26.2.3/libreoffice-dictionaries-26.2.3.2.tar.xz` | `d70f8d82e1958d901f8e1fcd1c3cfc51db13c4c3e45a7a043f5180798b64b726` |
| `libreoffice-help-26.2.3.2.tar.xz` | `https://download.documentfoundation.org/libreoffice/src/26.2.3/libreoffice-help-26.2.3.2.tar.xz` | `ac8d393005d9c588feb057f3a2e32707e8cd332505aa1a3a7bc1245f56bd5e57` |
| `libreoffice-translations-26.2.3.2.tar.xz` | `https://download.documentfoundation.org/libreoffice/src/26.2.3/libreoffice-translations-26.2.3.2.tar.xz` | `43eee1d52f5310af6156db6e9d05244fd476301911b70ec80868270616ef4b09` |

The reproducible custom build inputs are:

- `custom_build/communication_free_options.input`
- `custom_build/release_reduction_manifest.json`
- `custom_build/patches/*.patch`

The generated source tree, external tarball cache, downloaded source archives,
detached signatures, and built `instdir` remain outside Git unless a later
publication review explicitly changes that policy.

## User Verification Procedure

A user can verify the custom LibreOffice source basis without trusting the
large archive bodies in this repository:

1. Read `source_archives_26.2.3.2.manifest.tsv` and download each official LibreOffice archive,
   `.sha256`, and `.asc` file from the listed official URLs.
2. Check each downloaded archive with SHA-256 and compare it with the
   manifest value. On Windows, `Get-FileHash -Algorithm SHA256 <archive>`
   is sufficient for the hash comparison.
3. Optionally verify each detached signature with GPG after importing the
   relevant LibreOffice release signing key: `gpg --verify <archive>.asc <archive>`.
4. Compare `external_tarballs_26.2.3.2.manifest.tsv` with any retained or
   republished external tarball cache. This confirms the dependency archive
   names, sizes, and SHA-256 hashes observed in the custom build workspace backup.
5. Review `custom_build/communication_free_options.input` and
   `custom_build/release_reduction_manifest.json` and `custom_build/patches/*.patch`
   to inspect every repository-maintained
   change applied to the official source basis.
6. Treat any release runtime as acceptable only after the repository runtime
   gate and focused binary scan pass for the custom `instdir`.

This is a verifiable reference model, not a self-contained source mirror.
If a release needs self-contained source redistribution, publish the archive
bodies and detached signatures as release assets or a separate source bundle.

## Layout

- `image/program/soffice.com`
  - Console entry point to use from conversion code.
- `image/program/soffice.exe`
  - GUI-capable entry point. Do not use from this app.
- `image/license.txt`, `image/LICENSE.html`, `image/NOTICE`
  - LibreOffice and bundled component license notices.

## Local Policy Changes

The following files were removed from the administrative image because this app
must not include update or send-mail entry points:

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

The following non-conversion launchers/resources were also removed because this
app uses LibreOffice only through `soffice.com` for local headless Office-to-PDF
conversion:

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

The following GUI-only resources and bundled scripts/macros were removed
because headless conversion does not expose LibreOffice UI or execute bundled
macro/script samples:

- `image/share/gallery/`
- `image/share/tipoftheday/`
- `image/program/wizards/`
- `image/share/wizards/`
- `image/share/Scripts/`
- `image/share/basic/`

The following templates were removed because this app does not create new
LibreOffice documents and only converts existing Office files to PDF:

- `image/share/template/`

The following Japanese/English-only localization reduction was applied because
this app no longer targets general multilingual LibreOffice UI support:

- `image/share/registry/res/fcfg_langpack_*.xcd`, except `ja`, `en-US`, `en-GB`, and `en-ZA`
- `image/share/registry/res/registry_*.xcd`, except `ja`, `en-GB`, and `en-ZA`
- `image/program/resource/`, except `common`, `ja`, `en_GB`, `en_ZA`, and `en_US`

The following document authoring helpers and stale registry fragments were
removed because this app does not create or edit LibreOffice documents and the
referenced features are outside the conversion responsibility:

- `image/share/autotext/`
- `image/share/autocorr/`
- `image/share/labels/`
- `image/share/classification/`
- `image/share/wordbook/`
- `image/share/config/wizard/`
- `image/share/registry/pdfimport.xcd`
- `image/share/registry/librelogo.xcd`
- `image/share/registry/oo-ldap.xcd.sample`
- `image/share/registry/oo-ad-ldap.xcd.sample`

The following dictionary reduction was applied because this app targets
Japanese and English conversion. No Japanese dictionary extension is present in
this image, so only the English dictionary extension is retained:

- Removed `image/share/extensions/dict-*`, except `image/share/extensions/dict-en/`
- Retained `image/share/extensions/dict-en/`

The following Python/UNO scripting runtime files were removed because this app
does not execute LibreOffice macros, Python scripts, UNO scripting extensions,
ScriptForge, or Access2Base:

- `image/program/python-core-3.12.13/`
- `image/program/python312.dll`
- `image/program/python3.dll`
- `image/program/python.exe`
- `image/program/pyuno.pyd`
- `image/program/pythonloaderlo.dll`
- `image/program/pythonloader.py`
- `image/program/pythonloader.uno.ini`
- `image/program/pythonscript.py`
- `image/program/uno.py`
- `image/program/unohelper.py`
- `image/program/officehelper.py`
- `image/program/msgbox.py`
- `image/program/scriptforge.py`
- `image/program/scriptforge.pyi`
- `image/program/access2base.py`
- `image/program/desktophelper.txt`

The following UI icon theme archives were removed because this app only uses
headless conversion and does not expose the LibreOffice GUI:

- `image/share/config/images_*.zip`

The following Base/database/reportbuilder and Java runtime files were removed
because this app does not open Base databases, use external data sources, build
reports, or execute Java UNO extensions during Word/PowerPoint conversion:

- `image/program/classes/`
- `image/program/abplo.dll`
- `image/program/dbplo.dll`
- `image/program/hsqldb.dll`
- `image/program/jdbclo.dll`
- `image/program/postgresql-sdbc-impllo.dll`
- `image/program/libpq.dll`
- `image/program/java_uno.dll`
- `image/program/javaloaderlo.dll`
- `image/program/javavendors.xml`
- `image/program/javavmlo.dll`
- `image/program/jvmfwk3.ini`
- `image/share/firebird/`
- `image/share/config/soffice.cfg/dbaccess/`
- `image/share/config/soffice.cfg/modules/dbapp/`
- `image/share/config/soffice.cfg/modules/dbbrowser/`
- `image/share/config/soffice.cfg/modules/dbquery/`
- `image/share/config/soffice.cfg/modules/dbrelation/`
- `image/share/config/soffice.cfg/modules/dbreport/`
- `image/share/config/soffice.cfg/modules/dbtable/`
- `image/share/config/soffice.cfg/modules/dbtdata/`
- `image/share/config/soffice.cfg/modules/sabpilot/`
- `image/share/config/soffice.cfg/modules/sbibliography/`
- `image/share/config/soffice.cfg/modules/swreport/`
- `image/share/registry/base.xcd`
- `image/share/registry/postgresql.xcd`
- `image/share/registry/reportbuilder.xcd`

The following Calc/Excel/spreadsheet files were removed because this app does
not convert spreadsheet files:

- `image/program/calclo.dll`
- `image/program/scdlo.dll`
- `image/program/scfiltlo.dll`
- `image/program/sclo.dll`
- `image/program/scnlo.dll`
- `image/program/scuilo.dll`
- `image/program/vbaobjlo.dll`
- `image/share/calc/`
- `image/share/config/soffice.cfg/modules/scalc/`
- `image/share/registry/calc.xcd`
- `image/share/xslt/export/spreadsheetml/`
- `image/share/xslt/import/spreadsheetml/`
- `image/share/xslt/export/uof/odf2uof_spreadsheet.xsl`
- `image/share/xslt/import/uof/uof2odf_spreadsheet.xsl`

The following non-conversion leftovers were removed because their primary
features are outside the app responsibility:

- `image/program/pdfimportlo.dll`
- `image/program/PresentationMinimizerlo.dll`
- `image/program/Engine12.dll`
- `image/program/intl/fbintl.dll`
- `image/program/intl/fbintl.conf`
- `image/share/config/soffice.cfg/writerperfect/`

`image/program/libcurl.dll` is retained for now because `mergedlo.dll` and
`LanguageToollo.dll` import it. Removing it makes `soffice.com` fail before
conversion starts.

`image/program/jvmfwklo.dll` and `image/program/jvmaccesslo.dll` are retained
because `mergedlo.dll` imports them at process startup. Removing either one
makes `soffice.com` fail before conversion starts.

`image/program/orcus.dll`, `image/program/CoinMP.dll`,
`image/program/lpsolve55.dll`, `image/program/ifbclient.dll`, and older
external-format filter DLLs are retained because `mergedlo.dll` imports them at
process startup.

## Required Invocation Rules

When this dependency is wired into the app, use it only for local, headless
conversion of staged copies. Never pass the user's original file as the direct
conversion target.

Required process settings:

- Set `PYTHONDONTWRITEBYTECODE=1` defensively.
- Set `PYTHONPYCACHEPREFIX` to an app-managed ignored directory such as
  `__resource__/__libreoffice_pycache` defensively if the bundled image ever
  regains Python support.
- Clean any LibreOffice-image `__pycache__` directories before and after
  conversion.
- Use a per-conversion or app-managed user profile via
  `-env:UserInstallation=file:///...`.
- Use `--headless --nologo --nodefault --nolockcheck --nofirststartwizard --norestore`.
- Convert only a copied input file in an app-controlled temporary directory.
- Validate the generated PDF with PDFium before exposing it to the app.
- Treat conversion failure as "do not save" and leave the source file unchanged.

Current status: this administrative image is retained for conversion comparison
and reduction investigation only. Static scanning on 2026-05-28 confirmed that
it still includes network-capable code through `libcurl.dll`, `WINHTTP.dll`,
`WS2_32.dll`, and WebDAV/cURL markers. It is not eligible for app use or
release packaging. The app keeps Office conversion disabled until a
communication-free custom runtime passes `tools/release_checks/libreoffice_runtime_gate.py`.

## Custom Build Track

B-path reduction of the existing administrative image is mostly complete for
low-risk non-conversion resources. The remaining simple-delete blockers are
startup imports from `mergedlo.dll` and related libraries, especially
`libcurl.dll`, `jvmfwklo.dll`, `jvmaccesslo.dll`, `ifbclient.dll`,
`orcus.dll`, `CoinMP.dll`, and `lpsolve55.dll`.

The A-path custom build plan is tracked in
`docs/internal/plans/libreoffice_カスタムビルド実装計画_2026-05-04.md`. Use
`tools/libreoffice/libreoffice_build_env_check.py` first; it is read-only and does not
download, install, extract, or build anything.

The fixed first-pass exclusion options for that build are recorded in
`custom_build/communication_free_options.input`. A candidate runtime must pass
`tools/release_checks/libreoffice_runtime_gate.py` before any release packaging or app
enablement decision.

As of 2026-06-02, the current A-path build output is produced outside the
repository at `<LO_WORKDIR>\src\libreoffice-26.2.3.2\instdir`.
That output has passed the runtime gate and the focused binary scan for the
network/audio indicators used by this repository, and DOCX/PPTX smoke
conversion has succeeded. Do not rebuild it for every verification run; reuse
the existing gated `instdir` unless the LibreOffice source, custom build
options, custom patches, or toolchain changes.

As of 2026-06-08, `0024-use-localappdata-home-on-windows.patch` stops the
Windows custom runtime from treating the protected Documents folder as
LibreOffice's generic home directory during headless conversion startup. The
repository-local `third_party/libreoffice/custom_runtime/instdir` has been
updated from a rebuilt external `instdir`; runtime gate, binary scan, Office
conversion fixtures, DOCX space-protection conversion, and Defender event
checks passed with zero new `soffice.bin` Controlled Folder Access events.

As of 2026-07-15, `0025-improve-docx-table-layout-fidelity.patch` prefers
complete `w:tcW` cell widths when an autofit table has a materially stale
`w:tblGrid`, and uses tighter Word-compatible line metrics for Yu Gothic. The
long-table regression fixture improved from five pages to the Microsoft 365
reference count of three pages with 100% page-level extracted-text similarity.
The rebuilt Writer DLLs passed the runtime gate, prohibited-indicator scan,
DOCX/PPTX smoke conversion, and the four-document visual quality comparison
before being copied into the repository-local custom runtime.

As of 2026-07-16, `0032-refine-small-yu-gothic-table-line-height.patch`
further tightens line height only for Yu Gothic at 7.5 pt or below inside
Word-compatible table layout. Normal paragraphs, larger table text, font
ascent, and PPTX layout remain unchanged. All three long-table pages improved
against the Microsoft 365 reference while the other tested pages were either
unchanged or improved. The rebuilt `swlo.dll` passed the runtime gate, the
four-document smoke conversion, and the 26-page quality comparison before
being copied into the repository-local custom runtime.

As of 2026-07-17, `0033-improve-pptx-japanese-shape-font-linking.patch`
matches PowerPoint's Japanese font linking for DrawingML text that has no
direct East Asian font. Untagged classic-theme Japanese uses MS Gothic,
explicitly Japanese runs retain the theme's MS PGothic supplemental font, and
Japanese text linked from Cambria Math or Courier New uses MS Mincho. Explicit
East Asian fonts and non-Japanese text are unchanged. The rebuilt `ooxlo.dll`
passed the runtime gate and the eight-document, 63-page quality comparison
before being copied into the repository-local custom runtime.

As of 2026-07-17, `0034-use-chart-language-for-japanese-fonts.patch` retains
the OOXML chart language and uses the theme's Japanese supplemental font for
automatic chart text when no Japanese chart title is available for the older
heuristic. This changes only the previously mislinked series name in the
63-page comparison, from Noto Sans JP to the reference MS PGothic, while the
other 62 pages remain unchanged. The rebuilt `ooxlo.dll` passed the runtime
gate before being copied into the repository-local custom runtime.

As of 2026-07-17, `0035-complete-classic-japanese-font-linking.patch` adds the
Windows Office Japanese font links that were still missing from DrawingML:
Calibri to MS Gothic and Times New Roman to MS Mincho. It removes the remaining
Noto Sans JP use from the PPTX table/font fixture. The change applies only to
Japanese text whose resolved East Asian font is one of those Latin-only
typefaces; explicit Japanese-capable fonts remain unchanged.

As of 2026-07-17, `0036-preserve-japanese-chart-theme-fallback.patch` prevents
an empty `+mn-ea` chart theme reference from overwriting the Japanese
supplemental typeface selected from `c:lang`. The Word embedded-chart fixture
now uses the reference MS Mincho for its title, category labels, and legend.
The rebuilt `ooxlo.dll` SHA-256 is
`EE6170CFF3D938710D1656D54196F4DA187B874E732D0FA9AFB54E4CA193EBFF`.
The communication gate, focused binary scan, patch replay check, fixture smoke
test, and eight-document 63-page comparison passed.

The normal bundled runtime must retain the Math and Calc execution closure even
though standalone Calc conversion is not exposed by the application. Word
documents may contain OMML and embedded Excel objects. Removing `smlo.dll`, the
required Calc DLLs, `share/calc`, Calc configuration, or `calc.xcd` loses those
objects or changes following layout. The corrected reduced runtime was
subsequently checked for files outside the DOCX/PPTX headless-conversion path.
Isolated removal trials first removed 596 files and 15,698,131 bytes while
retaining the Math and Calc execution closure. A second, conversion-only pass
then removed GUI themes, palettes, toolbars, fingerprints, and the Writer,
Calc, and Draw UI definitions. The current runtime contains 1,050 files and
291,100,469 bytes: 1,301 files and 35,051,848 bytes below the corrected
2,351-file baseline. All eight documents and 63 pages still convert with no
quality-report difference, structural issue, or new semantic issue.

The accepted removal includes unreferenced conversion/UI DLLs and unused
`soffice.cfg` subtrees for unsupported modules. Removing the complete
`soffice.cfg` tree, the common GUI configuration as a whole, or additional
small common UI directories prevented source documents from loading and was
rejected. `program/scdlo.dll`, `program/scuilo.dll`, and
`share/config/soffice.cfg/modules/scalc` remain protected even though the
current fixture run did not load them, because they belong to the embedded
Excel conversion closure.

`presets/` is retained because deleting it prevents a new LibreOffice user
profile from being initialized. The Impress UI definitions are retained
because PPTX conversion fails without them. Removing all bundled fonts, or
even the Caladea/Carlito/Gentium/Noto group, made the full conversion run fail
to finish within the allowed timeout; fonts therefore remain outside this
reduction pass.

For app use, copy the gated custom runtime into this ignored repository-local
layout:

- `third_party/libreoffice/custom_runtime/instdir/program/soffice.com`

The app does not use LibreOffice runtime environment-variable overrides and
does not search for generic or official LibreOffice installations. It only
uses the communication-gated custom runtime layout above, or the corresponding
release layout beside the executable:

- `libreoffice/custom_runtime/instdir/program/soffice.com`

The repository tracks only the reproducible inputs for the custom build:

- `custom_build/communication_free_options.input`
- `custom_build/release_reduction_manifest.json`
- `custom_build/patches/*.patch`
- build/apply helper scripts and documentation

Generated runtimes, temporary conversion outputs, visual-check PNGs, and smoke
test kept directories belong under ignored locations such as
`third_party/libreoffice/custom_runtime/`, `.local/repo_resource/tmp/`,
`out/`, or the external `<LO_WORKDIR>\...` build tree. They must not be
committed.

Release packaging applies `custom_build/release_reduction_manifest.json` only
to the copied release runtime. It removes the verified headless-conversion
extras before the runtime gate runs. The source `instdir` is not modified by
packaging, and every manifest path is constrained to that copied runtime.

License handling is tied to what is actually shipped:

- The tracked `image/license.txt`, `image/LICENSE.html`, and `image/NOTICE`
  are still required while release packages include selected LibreOffice
  private fonts.
- The existing `image/` administrative runtime is not eligible for Office
  conversion release use because it still contains communication-capable
  indicators; do not use its runtime files as the app conversion engine.
- If the A-path custom `instdir` is later copied into a release,
  `pack_release.ps1` copies that runtime's own `license.txt`, `LICENSE.html`,
  and `NOTICE` into `licenses/libreoffice/`, and also copies the custom build
  options/patches into `licenses/libreoffice/custom_build/`.
- Do not copy licenses for Help, dictionaries, Basic scripts, UI license
  dialogs, or other LibreOffice subtrees unless the corresponding runtime files
  are also shipped.

The app conversion path now applies the DOCX Japanese spacing protection in the
staging copy only: `word/document.xml` text runs receive `U+2060 WORD JOINER`
inside tokens following `U+3000` ideographic spaces before LibreOffice sees the
copy. `tests/scripts/run_docx_space_protection_tests.ps1` exercises the same C++
staging implementation and verifies that the source DOCX is unchanged. With
`-Soffice <custom-soffice.com>`, the same test passes that app-staged DOCX to
LibreOffice with `--docx-space-protection off`, so Python does not perform a
second DOCX rewrite.

The staging ZIP support is intentionally narrow: normal single-disk DOCX ZIP
files using stored or deflated `word/document.xml` entries are accepted.
Malformed ZIPs and ZIP64-style central directory markers fail before output is
adopted. Unsupported inputs must fail without modifying the source DOCX.

The older `tools/libreoffice/libreoffice_smoke_test.py --docx-space-protection
word-joiner-token-after-space` mode remains a comparison/probe path, not the
primary release gate.
