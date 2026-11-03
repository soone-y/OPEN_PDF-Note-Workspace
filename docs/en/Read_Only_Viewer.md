# Read multiple documents in the read-only viewer

`readonly_viewer.exe` is a small Windows application for viewing PDFs with `.clrop` annotations, notes, and bundled documents in multiple tabs. It lets you compare materials without starting the main editing application.

## Why it exists

The goal was to keep several files open and move between them within the PDF Note Workspace environment. The viewer focuses on reading, including PDFs whose annotations are stored separately in `.clrop`. It uses Windows controls and PDFium, without a browser engine or online service.

The viewer uses its own fixed interface colors and does not follow the main application's theme setting.

## Open a PDF with its annotations

Start `readonly_viewer.exe` from the Full or Lite distribution folder, then choose **Open File** (`Ctrl+O`) and select a PDF. You can also open it from the main application's read-only viewer commands.

When a same-named `.clrop` file is in the same folder, the viewer displays the saved annotations over the PDF.

```text
lecture.pdf     original PDF
lecture.clrop   annotations for this PDF
```

Keep both files together when copying, moving, or sharing them. Save annotations in the main application before viewing them here. Opening a `.clrop` file alone shows annotation types and information grouped by page as a document. Open the PDF to view its pages.

## Compare several files

- Select several files in **Open File**, or add files from the file list, then switch tabs. `Ctrl+Tab` selects the next tab; `Ctrl+Shift+Tab` selects the previous one.
- Choose files and headings on the left and read the document on the right. You can open a folder and navigate back or up to its parent.
- Drag tabs to reorder them, move them to another viewer window, or detach them by dragging outside the window. Separate windows let you read documents side by side. The tab context menu also provides move commands.
- Press `Ctrl+F` to search document text or PDF text. `F3` finds the next result and `Shift+F3` the previous one. `Ctrl+B` hides the left pane for focused reading.

The narrow band to the right of document text normally marks lines containing Japanese characters. While the search field contains a query, it marks only matching lines in the current text view. Clearing the query restores the normal display. Click the band to move to that part of the document.

## Select a PDF region and its text

Drag across the PDF to frame a region. Then drag inside that frame to select text and press `Ctrl+C` to copy it. Dragging beyond the frame never includes text outside it. With no text selected, copying still takes all text in the frame.

`Esc` clears the text selection; press it again to clear the frame. Start a drag outside the frame to define another region. Selection exists only while viewing and does not write to the PDF or annotations.

While viewing a PDF, choose **View → Grayscale** to switch to grayscale; choose it again to restore color. This display choice resets when the viewer exits and is not saved to the PDF or preferences.

While viewing a PDF, use the left or right 90° rotation commands in **View** to rotate every page temporarily within that tab. Press `Ctrl+R` to rotate right or `Ctrl+Shift+R` to rotate left. **Reset View Rotation (This Tab)** returns to the PDF’s original orientation. The angle survives tab switches and moves to another viewer window, and resets when the viewer exits. It is not saved to the PDF, `.clrop`, or preferences.

## Read notes and other documents

The viewer supports Markdown, `.clro` notes, plain text, and a byte view. Reading view displays headings, tables, and supported Mermaid diagrams within the document. Switch views when you want to inspect the source.

| Key | View |
| --- | --- |
| `Ctrl+1` | Reading |
| `Ctrl+2` | Raw |
| `Ctrl+3` | Hex |
| `Ctrl+4` | Diagram list |

Available views depend on the file. Inspect unsupported diagram syntax in the source view. Use the zoom buttons or `Ctrl++` / `Ctrl+-` to change the display size, and `Ctrl+0` to reset it.

Right-click an inline diagram to open that diagram in a separate window. Its initial view fits the drawing; use zoom, reset, **Fit to Window**, and the horizontal or vertical scrollbars to inspect it. The main zoom controls also operate on the diagram list. Ordinary wheel scrolling over an inline diagram scrolls the document.

Document, PDF, and diagram positions are held temporarily within each open tab. Each display mode keeps its own position, and changing the text pane width uses the text at the viewport edge as an anchor. Scroll positions are not written to preferences for restoration after the application exits.

## Common action keys

Open **Settings → Shortcut reference** to see the viewer's keys.

| Key | Action |
| --- | --- |
| `Ctrl+G` | Go to a page number, including pages outside the current displayed range |
| `Ctrl+E` | Fit the current page to the PDF pane, adjusting both zoom and position |
| `Ctrl+O` / `Ctrl+Shift+O` | Open file / folder |
| `Ctrl+Shift+Q` / `Ctrl+Alt+Shift+Q` | Close current tab / all tabs |
| `Ctrl+Shift+A` | Show/hide annotations |
| `Ctrl+Shift+Left` / `Ctrl+Shift+Right` | Rotate this tab left/right 90 degrees |
| `Ctrl+Shift+0` | Reset this tab's view rotation |
| `Ctrl+Alt+E` | Open the existing annotated PDF export |

Page navigation, annotation visibility, and rotation apply to a PDF outside the search input field; native text selection takes priority there. Export starts by choosing a destination and leaves the original unchanged. Earlier `Ctrl+R` / `Ctrl+Shift+R` rotation keys remain available. Workspace memo, the unified output dialog, and quick output belong to the main application.

## Export an annotated PDF copy

Open the PDF with its matching `.clrop`, then use **File > Export Annotated PDF** (Ctrl+Alt+E) and choose a destination different from the source PDF. The same source path is rejected. This does not save changes into the original PDF, annotation file or note.

The viewer draws annotations into page contents. This differs from the main application's standard PDF annotation export: the output writing is not intended for further individual annotation editing. Inspect the resulting appearance. Use [main-application output](Help_Reference.md#pdf-annotation-output) when standard annotations are needed, and retain the original PDF/CLROP pair for further work.

## Files and preferences

Normal viewing operations do not edit or save the original PDF, `.clrop`, or note. To reopen selected materials next time, enable restoration in **Settings**, then mark tabs or a folder persistent in **Path Management**. These restoration preferences are saved to `readonly_setup.json` beside the executable. Use the main application to edit annotations or notes and to export standard PDF annotations.

Keep the bundled DLLs, `LICENSE.md`, and `licenses/` with the executable when moving it. The **Licenses** menu opens the guide and original notices. See [PDF and CLROP files](CLROP_Annotation_Format.md) for format details and [Using the Application](Using_the_App.md) for editing and output.
