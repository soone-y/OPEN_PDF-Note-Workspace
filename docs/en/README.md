# PDF Note Workspace (English)

This directory contains the guides for using the English release. Begin with [Getting Started](Getting_Started.md) when installing the application; after that, choose a guide by the task you want to perform.

- Start: `pdf_note_workspace.exe`
- Read-only viewer: `readonly_viewer.exe`
- Documentation and licenses: this `docs/` directory and the release `licenses/` directory

Package folders differ slightly by edition: Full groups LibreOffice components under `lo/`; Lite keeps only the bundled fonts under `libreoffice/`. [Getting Started](Getting_Started.md#full-and-lite-editions) explains how this layout reduces extraction-path length problems and where to extract the package.

[Using the Application](Using_the_App.md) is the everyday starting point: open a PDF, add notes or annotations, then save or export your work. Guides for formats, recovery, and troubleshooting are for the point at which you need them.

## Choose a guide

| Guide | Role |
| --- | --- |
| [Using the Application](Using_the_App.md) | Follow a complete first task from workspace preparation to sharing |
| [Help Reference](Help_Reference.md) | Look up a feature's location, prerequisites, steps, results, settings and limits |
| [Note writing guide](Note_Writing_Guide.md) | Copy syntax examples and understand their combinations |
| [Saving and Recovery](Save_and_Recovery.md) | Protect originals, inspect failed saves, restore backups and rescue work |
| [File Formats](File_Formats.md) | Choose note formats and keep PDF/annotation pairs together |
| [CLRO Note Format](CLRO_Note_Format.md) | Understand supported note display and compatibility with other software |
| [PDF Annotation Data](CLROP_Annotation_Format.md) | Save, move and share annotations without hand-editing JSON |
| [Read-only Viewer](Read_Only_Viewer.md) | Compare saved documents in tabs and manage viewing sessions |
| [Troubleshooting](Troubleshooting.md) | Take safe first steps when opening, saving or viewing fails |
| [Build and Verification](How_to_Build.md) | Build and verify from source (developers) |

The application has no external communication features. Saving uses recoverable operations and never overwrites an original PDF directly.

## License

- The [license](legal/LICENSE.md) is the English original and is the legally controlling text.
- See [third-party notices](legal/THIRD_PARTY_NOTICES.md) and `licenses/` for bundled components.
