# Read multiple documents in the read-only viewer

`readonly_viewer.exe` is a small Windows application for viewing PDFs with `.clrop` annotations, notes, and bundled documents in multiple tabs. It lets you compare materials without starting the main editing application.

## Why it exists

The goal was to keep several files open and move between them within the PDF Note Workspace environment. The viewer focuses on reading, including PDFs whose annotations are stored separately in `.clrop`. It uses Windows controls and PDFium, without a browser engine or online service.

The viewer uses its own fixed interface colors and does not follow the main application's theme setting.

## Open a PDF with its annotations

Start `readonly_viewer.exe` from the Full or Lite distribution folder, then choose **Open File** (`Ctrl+O`) and select a PDF. You can also open it from the main application's read-only viewer commands.

When a same-named `.clrop` file is in the same folder, the viewer displays the saved annotations over the PDF.

Text annotations use their saved font, point size, line breaks, vertical writing, and content orientation. Stars retain their pentagram-line, outline, or filled form and whole-shape rotation, combined with the PDF's original orientation and temporary view rotation.

```text
lecture.pdf     original PDF
lecture.clrop   annotations for this PDF
```

Keep both files together when copying, moving, or sharing them. Save annotations in the main application before viewing them here. Opening a `.clrop` file alone shows annotation types and information grouped by page as a document. Open the PDF to view its pages.

## Compare several files

- Select several files in **Open File**, or add files from the file list, then switch tabs. `Ctrl+Tab` selects the next tab; `Ctrl+Shift+Tab` selects the previous one.
- Choose files and headings on the left and read the document on the right. You can open a folder and navigate back or up to its parent.
- Navigate with **← Back** and **↑ Up** above the list. The parent folder is not mixed with files. Browseable folders have a **›** suffix; files show their PDF, note, annotation, or other file type.
- You can narrow the window to 480 pixels at 100% display scaling. The sidebar temporarily hides in narrow windows and returns when you widen them. View controls that do not fit on the toolbar remain available in the **View** menu.
- When the sidebar is hidden in a narrow window, choose **Documents** above the tabs to select a document. The same command is available in **File**, including in Focus Mode. The temporary list closes after selection or cancellation without widening the window.
- When an opened folder contains `pdf/` or `note/`, the file tree displays their contents in expandable **PDF (pdf)** and **Notes (note)** groups. Either folder can be used on its own. Other folders and files directly in the opened folder remain visible. Select a subfolder within a group to browse it.
- Drag tabs to reorder them, move them to another viewer window, or detach them by dragging outside the window. Separate windows let you read documents side by side. The tab context menu also provides move commands.
- Press `Ctrl+F` to search document text or PDF text. `F3` finds the next result and `Shift+F3` the previous one.
- Choose **View → Focus Mode** (`Ctrl+B`) to use the full area below the menu bar for reading. Switch documents from **Tab List** and change zoom or display format from **View**. The search field opens when requested and closes when you run a search or press `Esc`. Choose **Focus Mode** again or press `Ctrl+B` to restore the regular layout.

The narrow band to the right of document text normally marks lines containing Japanese characters. While the search field contains a query, it marks only matching lines in the current text view. Clearing the query restores the normal display. Click the band to move to that part of the document.

## Choose where to read a long file

Text larger than 50 MiB and files detected as binary that exceed 64 KiB prompt for a range before loading the whole file. The default byte count is 64 KiB.

- Enter a zero-based byte offset and byte count in decimal or `0x`-prefixed hexadecimal. The end offset is excluded.
- The start slider moves the range while keeping its byte count. The end slider changes its extent. **Previous Range** and **Next Range** move by the current byte count. Edits update the selection and load warning; reading starts with **Open This Range**.
- Use **Read Range**, the View menu, or the tab context menu to choose another range. Use the menu if the button is hidden in a narrow window. Ordinary-sized text and binary files also support range selection.

Each read is limited to 256 MiB. Confirmation is disabled if the estimated load is too high for available memory. Invalid input, cancellation, and read failures preserve the current display. The original file is never written. Ranges belong to the current tab and are not restored on the next launch.

**Hex** shows each byte as two hexadecimal digits; **Binary** shows it as eight binary digits (0 and 1). Both show the original file offset in hexadecimal on the left and printable ASCII characters on the right. Other values appear as `.`, without decoding Japanese or other text encodings. Hex uses 16 bytes per row; Binary uses eight. The view stays selected when switching tabs within the same window or changing the read range. Switching views changes neither the original file nor the read range.

Both display at most the first 64 KiB of a loaded range, with a notice when more bytes are omitted. Change the read range to view another part. Cutting text inside a character may produce garbled text in Source view. Use Hex or Binary to inspect the exact bytes.

## Check the PDF and annotation pairing

An open PDF is protected against external editing, deletion, renaming and replacement. Close its views before editing it in another application. A same-name `.clrop` is checked against file size, page count and original page dimensions; recorded SHA-256 is also checked. Mismatched annotations are hidden and a notice appears in the PDF panel. Use the main application to review the pairing or update its identity. A changed `.clrop` is checked again. The viewer never rewrites the original or its pairing information.

The main application's `Tools → Find PDF / CLROP candidates` can open a dedicated candidate preview. This read-only, unconfirmed overlay can include mismatched annotations for inspection. It is separate from normal pairing acceptance and saves no viewer settings or pairing information. See [finding PDF candidates](CLROP_Annotation_Format.md#find-pdf-candidates) for the two search modes.

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

Copying a selection that includes document text requires the open PDF's copy permission. Selections containing only separate-file annotations may still be copied. Export stops for encrypted or digitally signed PDFs because it cannot preserve those protections. The PDF pane shows the reason when a restriction stops an operation.

Open the PDF with its matching `.clrop`, then use **File > Export Annotated PDF** (Ctrl+Alt+E) and choose a destination different from the source PDF. The same source path is rejected. This does not save changes into the original PDF, annotation file or note.

The viewer draws annotations into page contents. This differs from the main application's standard PDF annotation export: the output writing is not intended for further individual annotation editing. Inspect the resulting appearance. Use [main-application output](Help_Reference.md#pdf-annotation-output) when standard annotations are needed, and retain the original PDF/CLROP pair for further work.

## Files and preferences

Normal viewing operations do not edit or save the original PDF, `.clrop`, or note. Use the main application to edit annotations or notes and to export standard PDF annotations.

In **Next Startup**, enable **Reopen documents at next startup**, then register only the documents or folder you want with **Reopen this document next time** and **Reopen this folder next time**. **ON with zero registered targets is valid.** Enabling reopening never automatically registers the current PDF, note, or folder.

Use **Review and manage startup targets** to inspect registrations and select **Remove ... from startup** to unregister an item. Closing a tab keeps its registration. Turning reopening OFF also keeps registrations, while preventing them from opening at startup. Changes are saved to `readonly_setup.json` beside the executable; a failed save restores the previous settings. Existing dedicated preferences take precedence, so an empty selection is never replenished from an older shared setup.

Keep the bundled DLLs, `LICENSE.md`, and `licenses/` with the executable when moving it. The **Licenses** menu opens the guide and original notices. See [PDF and CLROP files](CLROP_Annotation_Format.md) for format details and [Using the Application](Using_the_App.md) for editing and output.
