# Build and Verification

This document is for developers building from source. To use a release package, see [Using the Application](Using_the_App.md).

## Prerequisites

- Windows 10 or 11
- PowerShell
- `g++` available on `PATH`
- `third_party/pdfium/lib/pdfium.dll.lib`
- `third_party/pdfium/bin/pdfium.dll`
- The repository version is managed in `REPO_VERSION.txt`.

Dependencies are vendored in the repository. The build and the application must not make external network connections.

## Build

```powershell
./build.ps1
```

`build.ps1` builds the Full edition, Lite edition, and read-only viewer in the Release configuration. To prepare a test environment with sample data, run `./build.ps1 -Run`, then launch `out/run/pdf_note_workspace.exe` (Full) or `out/run_lite/pdf_note_workspace.exe` (Lite). With `-Locale en`, use `out/run_en/` and `out/run_lite_en/` instead. Each folder also contains the read-only viewer.

The first build seeds `sample_workspace/` from the release template for that language and edition, and sets it as the default if no setup file exists. Later builds preserve edits and settings. If a build says the running application needs a binary update, close the application and run the build again.

- `./build.ps1 -Clean` removes the selected artifacts under `out/obj*` and `out/bin*`. Add `-Run` to remove the local runtime binaries as well. Both keep sample data and settings.
- `./build.ps1 -HardClean` does everything `-Clean -Run` does, and deletes the displayed `out/run*/sample_workspace/` directories and any edits they contain. Close any application using the sample first. Settings and other workspaces are kept. The next `./build.ps1 -Run` reinstalls a fresh sample.

Both commands target Full and Lite by default. Use `-Lite` for only the Lite run folder, or `-AllLocales` to include both languages. The `out/run*/` folders are for testing inside the repository. Full can convert Office documents if the verified conversion runtime is present in the repository. Use a release set when checking the distribution itself.

To build individual targets:

```powershell
./scripts/build/build_workspace.ps1 -Edition Full
./scripts/build/build_workspace.ps1 -Edition Lite
./scripts/build/build_readonly_viewer.ps1
./scripts/build/build_workspace.ps1 -Edition Full -Rebuild
```

Full includes Office-to-PDF conversion; Lite does not. The normal application and read-only viewer are written to `out/bin/`; the Lite application is written to `out/bin_lite/`.

## Verification

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_repo_checks.ps1
python tests/python/validate_codebase.py
python -m unittest tests/python/test_python_tools.py
```

List checks with `./tests/scripts/run_repo_checks.ps1 -ListSteps`. Select related steps from PowerShell as shown below. A partial run does not establish a full pass; provide the selected checks' prerequisites and existing artifacts. Optional checks still require their `-Include...` switches. A requested step that is skipped fails the run. The no-argument command above runs the normal full suite; optional checks require separate switches.

```powershell
./tests/scripts/run_repo_checks.ps1 -OnlyStep 'Text Integrity Gate','Safety Scans'
```

Select active-content resistance checks with `./tests/scripts/run_repo_checks.ps1 -OnlyStep 'Script Resistance Tests'`. They cover passive PDFium processing and related static contracts; passing does not establish that the entire application is free of vulnerabilities.

When changing saving behavior, also run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_atomic_write_tests.ps1
```

## Create a release set

```powershell
./release.ps1
```

Do not distribute `out/` directly. Use the Full or Lite folder or ZIP created in a release set. ZIP filenames include locale and edition; their extracted top-level folder is `PDF-Note-Workspace-<version>`. The Full edition includes the verified LibreOffice conversion runtime; Lite does not. To make a short development build of Lite only, use `./build.ps1 -Lite`.

Use `scripts/release/make_release_set.ps1` or `scripts/release/pack_release.ps1` only when adjusting release names, output paths, or bundled contents.

Full groups the LibreOffice conversion runtime and app-private fonts under `lo/`, with fonts at `lo/image/Fonts/`. Lite bundles only the fonts at `libreoffice/image/Fonts/`. Both editions keep the notices at `licenses/libreoffice/`. Font lookup also supports older package layouts; existing release sets retain their original layout.
