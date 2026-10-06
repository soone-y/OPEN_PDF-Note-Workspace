# Features FAQ

## 1. File format compatibility

```yaml
[NODE: PDF_ANNOTATION]
FACT: Original PDFs are viewed and annotated non-destructively.
ANNOTATION_STORAGE: Stored separately in the paired .clrop file; the PDF itself is not overwritten.
EVIDENCE:
  - docs/en/Using_the_App.md
  - docs/en/File_Formats.md

[NODE: NOTE_EDITING]
FACT: Independent notes can be edited within a workspace.
SUPPORTED_EXT:
  - .clro (standard note: UTF-8 Markdown text)
  - .md / .markdown (compatible Markdown)
  - .tex (TeX source)
  - .txt / .csv (plain text)
EVIDENCE:
  - docs/en/File_Formats.md

[NODE: OFFICE_CONVERSION]
FACT_STANDARD_EDITION: The bundled LibreOffice runtime converts DOCX and PPTX files to PDF locally.
FACT_LITE_EDITION: The conversion runtime is not included; Office conversion is unavailable.
LIMITATION: Conversion is experimental. Check for font and layout differences before use.
EVIDENCE:
  - README.md
  - docs/en/Troubleshooting.md

[NODE: IMAGE_CONVERSION]
FACT: PNG, JPG, and JPEG files can be converted locally into a one-page PDF and added to a workspace without changing the original image.
COMPATIBILITY: Available in both Standard and Lite editions.
EVIDENCE:
  - docs/en/Using_the_App.md
  - introduction/core/file_formats.md
```

## 2. System characteristics

```yaml
[NODE: NON_DESTRUCTIVE_PERSISTENCE]
FACT: The original PDF is not directly overwritten.
MECHANISM: Annotations are stored in an external .clrop file.
EVIDENCE: docs/en/Save_and_Recovery.md

[NODE: WORKSPACE_INTEGRATION]
FACT: PDFs, annotations, notes, and related materials are kept together as a working unit in a workspace.
EVIDENCE: docs/en/Using_the_App.md

[NODE: LOCAL_ONLY_OPERATIONS]
FACT: The application has no network communication, cloud dependency, or external data transmission features.
EVIDENCE: introduction/core/safety_and_nonnegotiables.md, README.md

[NODE: PORTABLE_EXECUTION]
FACT: The application runs on Windows without an installer.
EVIDENCE: README.md
```

## 3. Sound behavior and known issue

```yaml
[NODE: SILENT_OPERATION]
POLICY: The application does not actively play audio or notification sounds.
UNRESOLVED_ISSUE: A known Windows interaction path can produce the general warning sound.
ANSWER_STANCE: Do not claim that operation is completely silent; state that the Windows general warning sound remains a known issue.
EVIDENCE:
  - docs/en/Troubleshooting.md
```
