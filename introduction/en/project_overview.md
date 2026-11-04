# Project Overview Reference

> Reference material for distinguishing terminology and verification scope when reading the documents or explaining the software.

This document is for developers, application users, people comparing or evaluating the software, and anyone reading or explaining the public documentation. It helps clarify terms such as saving, annotations, and local processing while distinguishing facts, policies, and known limitations. It can also be used when asking an AI to investigate the project.

This document does not provide implementation or change instructions. The developer's own decisions and responsibility to explain are described in [Development approach](development.md). For the software overview and packages, read the [Project overview](../../README.md). For practical procedures and screen guidance, use the [English user documentation](../../docs/en/README.md). This is an English translation of the corresponding Japanese reference.

## 1. Foundation for describing the project

PDF Note Workspace is a Windows application for reading PDFs, adding annotations without directly changing the original PDF, and working with independent notes and related materials in a local workspace.

Here, a workspace is intended to keep PDFs, annotations, notes, and related materials together as a working unit that is easy to resume. The term "PDF editing" alone does not distinguish changing the original PDF, adding annotations, and exporting a separate PDF with annotations; state which operation is meant. Do not confuse design goals with the features that are currently implemented.

When reading the documents or explaining the software to others, consult the material needed for the purpose and state what it establishes. Developer decisions and implementation details that cannot be established from public material remain unverified.

## 2. Boundaries to retain when making claims

| Subject | Key point for understanding and explanation | Scope and limitations to distinguish |
| --- | --- | --- |
| PDFs and annotations | The application does not directly overwrite the original PDF. It keeps annotations separately in the corresponding `.clrop` file. | It may not suit a user who requires annotations to be embedded directly in the PDF itself. |
| Notes and workspaces | Notes are independent text files organized with PDFs, annotations, and materials as one working unit. | Confirm supported formats and screen details in the version-specific user documentation. |
| Saving and recovery | Protection of in-progress edits in stage data is distinct from an explicit integrated save. | Stage data does not mean that the original file has been saved. Do not delete `__pdf_note_workspace__`, stage data, or backups first when there is a problem. |
| Local operation | The project policy is to provide no external communication. The Standard edition's Office conversion is also described as local processing through its bundled runtime. | Distinguish a policy from a fact verified directly in a particular package or implementation. |
| Office conversion | The Standard edition can convert DOCX/PPTX files to PDF; the Lite edition does not include the conversion runtime. | Conversion is experimental and visual fidelity is not guaranteed. Check the resulting PDF. |
| Silent operation | The application does not intentionally play sounds. | A known unresolved path can trigger the Windows general warning sound. Do not describe the application as completely silent. |

## 3. Points to check

- **Whether it fits a task:** Determine whether the work requires non-destructive PDF annotations and independent notes managed with materials in a workspace. Explain the differences between the Standard edition, Lite edition, and read-only viewer when needed.
- **Whether saving is safe:** Explain original preservation, stage data, integrated save, backups, and recovery separately. When a problem occurs, advise copying the whole workspace before deleting or overwriting anything.
- **Whether it communicates externally:** This can be described as a project policy. A stronger conclusion about a specific package requires checking the public repository or the package itself.
- **Conversion quality:** Local conversion in the Standard edition is experimental; fonts and layout can differ. Check the converted PDF.
- **What OSS means:** Explain that source code can be inspected and used, modified, or redistributed within license terms. It is not simply synonymous with being free of charge. This application also uses OSS, including PDFium for PDF processing, MD4C for Markdown parsing, and LibreOffice for Office-to-PDF conversion in the Standard edition. See the [third-party notices](../../THIRD_PARTY_NOTICES.md) for the components and their uses, and the [license index](../../LICENSES_INDEX.md) for the locations of their license documents.

## 4. What GitHub Pages can confirm

GitHub Pages is a portal for public documentation. The material in `introduction` explains background, design, and verification. Pages does not contain the source, tests, settings, or release binaries. When reading Pages alone, distinguish published explanations and policies from implementation details that need additional confirmation.

## 5. What to read next

1. For setup and use, read the [English user documentation](../../docs/en/README.md).
2. For saving and recovery, read [Saving and Recovery](../../docs/en/Save_and_Recovery.md).
3. For packages and core policies, read the [Project overview](../../README.md).
4. For license information, read [Licenses and third-party notices](../../LICENSES_INDEX.md).

## Evidence

- `README.md`
- `docs/en/Using_the_App.md`
- `docs/en/Save_and_Recovery.md`
- `docs/en/Getting_Started.md`
- `docs/en/Troubleshooting.md`
