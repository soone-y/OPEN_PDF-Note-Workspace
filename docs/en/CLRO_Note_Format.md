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
<char=1a73e8>blue text</>
<back=fff2cc>text with a background color</>
<link=sample-note>an internal-note link marker</>
```

This markup is not exclusive to `.clro`; `.md` and `.markdown` use the same route. Prefer ordinary Markdown first, then use application-specific markup only where its display or internal links are needed.

## Container blocks

Surround text with standalone `::: note` and `:::` lines to display a block with a background, border, and inset. Headings, emphasis, links, tables, and math inside it retain their normal interpretation. A code block is a block whose body is instead displayed literally.

```text
::: note
**Additional information** and `code`
:::
```

`note` is an identifying name; a bare `:::` can also open an unnamed block. A named opening line inside a container creates a nested block. An unnamed closing line closes the innermost block and must have at least as many colons as its opening line. Up to three leading spaces are accepted. An unclosed block extends to the end of the document. Styling attributes and folding are not supported.

Editing or selecting a row shows that row's source, not the entire enclosing container. Tables and display math inside it retain their own editing units. TXT output removes the wrapper syntax while preserving its body. Colons inside code or math do not open containers.

## Mathematics and compatibility

Math can be written with `$...$`, `$$...$$`, `\(...\)`, or `\[...\]`. Math display is experimental, so verify the rendered result for important notes.

In `.clro`, `.md`, and `.markdown`, `<math>...</math>` can also wrap TeX math. A wrapper within a sentence is inline math; a standalone wrapper, with no surrounding text on its boundary lines, is display math.

```text
The equation is <math>E = mc^2</math>.

<math>
\frac{1}{2}
</math>
```

Both `</math>` and `</>` close math. Math owns its closing token, so it does not accidentally close surrounding underline or link markup. `<math display=inline>x</>` remains inline even on a standalone row; `<math display='block'>x</math>` requests display math. Values may be unquoted, single-quoted, or double-quoted. Tag names, attribute names, and display values are case-insensitive. Write the opening tag on one row.

Unknown attributes remain in source and Markdown output, are ignored for display, and produce a diagnostic. Invalid or duplicate `display` attributes, unclosed wrappers, and nesting retain source rather than becoming math. Inline math must occupy one row; display math must stand alone without surrounding prose. Code and escaped opening tags are not interpreted. Self-closing tags and MathML are not supported; the body is TeX. Editing shows source for the inline row or the entire display-math block, just like other math delimiters. `.tex` display continues to use TeX math delimiters only.

Other applications can open a `.clro` file as ordinary text. General Markdown may remain readable, but application-specific markup, math rendering, and internal note links are not guaranteed to display or work elsewhere. This application does not promise complete support for every Markdown feature or identical GitHub rendering.

For PDF annotation data, see [`.clrop` PDF Annotation Data](CLROP_Annotation_Format.md). For format selection, see [File Formats](File_Formats.md).
