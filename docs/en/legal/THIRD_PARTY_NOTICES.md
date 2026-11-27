# Third-party notices

The distribution includes PDFium, MD4C, MicroTeX, XITS Math, zlib, MinGW-w64 runtime, and LibreOffice runtime in the full edition. The original license texts are bundled in `licenses/` and take precedence over this summary.

The full edition includes LibreOffice modification patches, including `0051` for long local paths, in `licenses/libreoffice/custom_build/patches/`. Original MPL/Apache notices are retained.

## MicroTeX and XITS Math

The embedded renderer is a modified copy of MicroTeX openmath, commit `086f4eb740270b28bd0c61a0a359aea9300d61ae`. Its MIT copyright and license text are bundled in `licenses/microtex/LICENSE.txt`. Source provenance and modifications are recorded in `third_party/microtex/README.md` in the source distribution.

`resources/math/xits/XITSMath-Regular.clm2` contains XITS Math outlines and metrics under SIL Open Font License 1.1. The complete license, copyright notices, reserved names, FONTLOG and README are retained in `licenses/xits/`. The font is not sold separately; font derivatives remain under OFL. The application license is unchanged. Japanese text uses fonts installed on Windows; their font files are not redistributed.
