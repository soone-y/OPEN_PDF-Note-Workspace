# `.clrop` PDF Annotation Data

`.clrop` is the JSON file that PDF Note Workspace uses to save annotations for a PDF. Its name denotes the PDF-annotation counterpart of the Classroom-derived `.clro` note extension. It is not a note or the PDF itself, and annotations do not directly rewrite the PDF.

## Keep it with its PDF

When you annotate a PDF, the application places a same-named `.clrop` file in the same folder.

```text
lecture.pdf     original PDF
lecture.clrop   annotation data for lecture.pdf
```

Both files are needed to display and edit annotations again. When moving, renaming, copying, or sharing a PDF, handle its `.clrop` file together with it.

## What the file contains

::: details Inspect the `.clrop` JSON structure

The current format is version 2 JSON; version 1 remains readable. It contains information for identifying the PDF and annotations grouped by page.

```json
{
  "version": 2,
  "pdf_id": { "path": "lecture.pdf", "size": 123456, "page_count": 12, "sha256": "..." },
  "pages": [ { "page": 0, "items": [ { "type": "text" } ] } ]
}
```

This is a simplified example. Fields vary by annotation type. The application currently writes text, math, text markers and color changes, freehand markers, freehand ink, lines, arrows, waves, note-link markers, and shapes. PDF identification includes more than a path: it can use size, page count, page sizes, and SHA-256 data.

:::

## Inspecting the text

Saved JSON uses UTF-8 and Windows line endings (CRLF) to separate pages, annotations, content and coordinates. Short related fields share a line. Long stroke and marker arrays wrap between coordinate groups, preserving each group. Newlines inside strings remain JSON escapes. A text editor can be used to inspect the content; change and save annotations in the application.

Each number uses the shortest representation that reads back to the same internal value. For example, `0.34999999999999998` becomes `0.35`. Values retain the precision needed for positions, shapes and recovery. Opacity 1, horizontal writing, inherited background assistance and single arrow heads are omitted because their absence has the same meaning. Display, printing and PDF output precision stay unchanged.

The combined effect of formatting, shorter numbers and omitted defaults can increase or decrease the file size, depending on the annotations. Existing files adopt this representation on their next save. The format remains version 2.

## Text marker display modes

In version 2, an omitted `marker_display_mode` means Multiply. Legacy translucent display is stored as `"marker_display_mode":"legacy"`. An explicit `"multiply"` is also accepted and omitted on save. This choice affects the application display only; PDF and PNG output methods stay unchanged.

Version 1 markers without a mode load with Legacy display. Saving upgrades the file to version 2 and explicitly records `legacy` only where needed to retain the old display. Opening a file alone does not upgrade it. Existing markers can be changed to Multiply in the annotation editor. Choose the creation default under **Settings → Annotations → Marker**.

Editing version 2 requires an application that supports it. Older applications supporting only version 1 stop loading the new format.

## Parenthesis-like arcs

In version 2, a `shape` with `shape_kind: "arc"` is an open curve. The `bbox` width controls bow depth and its height controls the vertical span. Its base orientation is `(`; `shape_angle` of pi gives `)`, and `content_rotation` also applies. `shape_draw_mode` is `"outline"` or omitted; `dash` supports solid or dashed lines. Filled interpretations are rejected while preserving the input. Further editing requires a version supporting this shape; dashed arcs require this extension. PDF output uses editable Ink strokes, with separate strokes for dash runs.

## Shape boundaries

In version 2, a `shape` may set `shape_boundary` to `"black"` or `"white"` for a boundary independent of its fill color. Omission keeps the existing rendering with no added boundary. Explicit `"none"` is accepted and omitted on save. The boundary uses `width`, `alpha` and `dash`. Filled shapes retain their original `color`; outline-only shapes use the boundary color. This field also affects PDF and PNG output.

## Whole-shape rotation

In version 2, `shape_angle` is the clockwise whole-shape angle in radians, around the center of its bounding box. Omission means zero; zero is omitted on save. The old rotated ellipse's local `shape_rotation` remains separate for compatibility; existing shapes are not automatically converted. Rotation handles are temporary UI and are excluded from saved annotations and output. Use a version supporting `shape_angle` to edit rotated shapes.

## Annotation orientation and temporary view rotation

For text, math and shapes, `content_rotation` is a clockwise content angle in original PDF coordinates: 0, 90, 180 or 270 degrees, with 0 as the default when omitted. Positions use PDF coordinates before applying the CropBox origin and the PDF's own page rotation. View changes never rewrite stored positions. `shape_rotation` remains the local angle of a rotated ellipse.

Temporary page rotation selected from the View menu is not stored in `.clrop`. Only the paper-anchored direction of annotations created in that view is recorded. Use an application version that understands this orientation field for further editing.

## Do not edit it by hand

`.clrop` is a JSON file managed by the application. Displaying annotations on the correct PDF requires consistent fields and a correct match with that PDF. Manual edits can cause display problems or associate annotations with the wrong PDF.

Add, edit, and remove annotations in the application. On save, the application writes UTF-8 JSON through its safe-write path. If saving fails, it does not leave the saved `.clrop` file partly rewritten; use the saved-state, restore, and backup features to review pending work.

Do not rename `.clrop` to `.clro`, change its extension to change its type, or edit its contents in a text editor.

For the human-authored note format, see [`.clro` Note Format](CLRO_Note_Format.md). For format selection and moving files, see [File Formats](File_Formats.md).
