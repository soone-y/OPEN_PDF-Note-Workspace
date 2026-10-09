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

`build.ps1` builds the Full edition, Lite edition, and read-only viewer in the Release configuration and writes ready-to-package executables below `out/`.

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
