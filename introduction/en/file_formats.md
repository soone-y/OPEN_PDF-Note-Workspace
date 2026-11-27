# File Formats and Persistence

## 1. File extension specification

```yaml
[EXT_SPEC: .pdf]
ROLE: The PDF document that is viewed.
MUTABILITY: Normally unchanged; the application must not overwrite it.
EVIDENCE: docs/en/File_Formats.md

[EXT_SPEC: .clrop]
ROLE: JSON annotation data associated with a PDF.
MUTABILITY: Written and managed by the application. It is not a note and must not be edited manually as text.
PAIRING_RULE: Stored with the same name and in the same folder as the PDF (for example, doc.pdf -> doc.clrop).
EVIDENCE: docs/en/File_Formats.md

[EXT_SPEC: .clro]
ROLE: The application's standard note format: UTF-8 Markdown text.
MUTABILITY: Editable in the application and in external editors.
EVIDENCE: docs/en/File_Formats.md

[EXT_SPEC: .md / .markdown / .txt / .csv]
ROLE: Compatible Markdown or plain-text notes.
MUTABILITY: Editable. They are not automatically renamed to .clro.
EVIDENCE: docs/en/File_Formats.md

[EXT_SPEC: .tex]
ROLE: Existing TeX source that can be opened and displayed.
MUTABILITY: Editable. It is not automatically renamed to .clro.
EVIDENCE: docs/en/File_Formats.md

[EXT_SPEC: .png / .jpg / .jpeg]
ROLE: Image files that can be imported.
CONVERSION: The original is not changed. The application can locally create a one-page PDF and add it to a workspace.
EVIDENCE: docs/en/Using_the_App.md
```

## 2. Persistence model and paths

```xml
<persistence_model>
  <step id="stage" type="temporary_protection">
    <description>Protected area for edits in progress, including protection against crashes and power loss.</description>
    <path>__pdf_note_workspace__/__unsaved__/</path>
  </step>
  <step id="backup" type="recovery_copy">
    <description>Backup created before the original is overwritten by Ctrl+S or Save > Save Work, a normal exit, or an export.</description>
    <path>__pdf_note_workspace__/__backup__/</path>
  </step>
  <step id="recovery" type="recovery_review">
    <description>Recovery reviews of edits that could not be saved, and copies of the stage.</description>
    <path>__pdf_note_workspace__/__recovery__/</path>
  </step>
  <step id="failed" type="emergency_dump">
    <description>Content that could not be written, and files that could not be read.</description>
    <path>__pdf_note_workspace__/__failed__/</path>
  </step>
  <step id="consolidated_save" type="explicit_write">
    <description>Integration into the regular file when Ctrl+S or a Save menu command is selected.</description>
  </step>
  <evidence_path>docs/en/Save_and_Recovery.md</evidence_path>
</persistence_model>
```
