# Embedded MicroTeX

This directory contains the offline subset of MicroTeX openmath from
<https://github.com/NanoMichael/MicroTeX>, commit
`086f4eb740270b28bd0c61a0a359aea9300d61ae`.
`UPSTREAM.json` records the source archive digest and original file digests.
The original library license is retained in `LICENSE` (MIT).

The application builds the C++17 library and Windows GDI+ backend directly as
independent translation units. It does not download dependencies, discover
font files, launch TeX processes, or enable privileged math commands.
`lib/microtexconfig.h` is a local configuration header; auto font discovery and
logging are disabled. The glyph backend uses embedded CLM2 outlines rather than
separate OTF or TTF files. Windows system fonts supply ordinary text fallback.

The only bundled math font is `res/xits/XITSMath-Regular.clm2` (995,497 bytes,
SHA-256 `1bf9cbe66a4357d63600e3f8e8415321d9d2f87f1476cd4b7e36bed6534e3042`).
It is covered by SIL OFL 1.1; retain the complete `OFL.txt`, `FONTLOG.txt` and
`README.txt` beside it in source distributions. Binary distributions put these
texts in `licenses/xits/` and the font in `resources/math/xits/`.
The font license does not change the application's license.

Local adaptations implement optional row spacing, the hbar symbol, Windows
UTF-8 conversion, finite integer metrics, bounded parsing, locale-independent
numeric conversion, and deterministic GDI resource cleanup. User-defined macros
and environments are disabled because upstream definitions change process-wide
state. Unsupported input remains editable as source; PDF export reports failure. Outline drawing and
application lifetime/cache ownership live in `src/math/` and `src/note/`.
Local patches are recorded in `patches/`; retain them when updating this copy.
The application verifies the exact font size and digest before parsing it.
The legacy MicroTeX master fonts and special.ttf are not included.

`scripts/build/sync_microtex_assets.ps1` is the common resource-copy contract for
builds, tests and packaging. It verifies original notice bytes and font bytes.
