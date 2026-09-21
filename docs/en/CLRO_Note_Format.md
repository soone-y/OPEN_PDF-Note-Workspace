# `.clro` Note Format

`.clro` is the default note extension for notes created in PDF Note Workspace. Its name comes from “Classroom.” Its contents are ordinary UTF-8 text, not a binary container or a PDF annotation file.

## Choose the right format

| Goal | Format |
| --- | --- |
| Create notes mainly in this application | `.clro` |
| Work with other Markdown applications | `.md` / `.markdown` |
| Keep an unformatted memo | `.txt` |

`.clro`, `.md`, and `.markdown` use the same Markdown/MD4C note route. The difference is their product role: new notes, suggested names, and default naming use `.clro`. Existing `.md` files are never renamed or converted automatically, and copy, rename, and link operations keep the selected extension.

## What it is good for

Use Markdown-style headings, lists, emphasis, links, code, and tables to structure notes. Headings are also available in the application's heading view.

### Tables

Pipe tables are normally shown as aligned columns. Header cells are visually distinct, and `:` in the separator row controls left, center, or right alignment. Use the note's horizontal scroll bar when a table is wider than the view.

When the caret enters a table cell, or a selection reaches the table, the entire table switches to its editable source view. It returns to the structured view after focus leaves the table. This prevents source text and structured table content from being drawn over one another.

The same note route supports application-specific markup for cases where Markdown is not enough, such as underline, text color, background color, font selection, and internal note links.

```text
<u>underlined text</>
<char=#1a73e8>blue text</>
<back=#fff2cc>text with a background color</>
<link=sample-note><la><lu>an internal-note link marker</></></>
```

This markup is not exclusive to `.clro`; `.md` and `.markdown` use the same route. Prefer ordinary Markdown first, then use application-specific markup only where its display or internal links are needed.

## Mathematics and compatibility

Math can be written with `$...$`, `$$...$$`, `\(...\)`, or `\[...\]`. Math display is experimental, so verify the rendered result for important notes. The older `<math>...</>` form is not accepted as new Markdown/TeX note syntax.

Other applications can open a `.clro` file as ordinary text. General Markdown may remain readable, but application-specific markup, math rendering, and internal note links are not guaranteed to display or work elsewhere. This application does not promise complete support for every Markdown feature or identical GitHub rendering.

For PDF annotation data, see [`.clrop` PDF Annotation Data](CLROP_Annotation_Format.md). For format selection, see [File Formats](File_Formats.md).
