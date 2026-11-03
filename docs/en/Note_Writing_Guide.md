# Note writing guide

Choose a form from the quick reference first. Detailed steps, aliases, and combinations follow later.

## Space below the last line

Use **Note bottom space** in Note settings or the bottom-right status assist. Choose a fixed size (pixels at 100% scaling), a percentage of text size, or a percentage of the note viewport height. The default is 32 fixed pixels. Set the selected value to 0 to disable it. Short panes limit the space to leave at least one text line.

Structured display can scroll past the end. Typing, inserting a newline, or moving the caret automatically scrolls far enough to keep the configured space below the caret. Wheel scrolling alone does not pull the view back to the caret. Raw display reserves space below the editing area. No blank lines are added, and saved or exported text is unchanged.

## Syntax quick reference

`M` Markdown-style; `c` Custom extension; `T` TeX math. `M` includes the supported table, task-list, and strikethrough extensions.

| Kind | Write | Minimal form |
| --- | --- | --- |
| `M` | Paragraphs and breaks | Blank lines separate paragraphs. End a line with two spaces or `\` for a hard break |
| `M` | Headings | `# Heading` (one to six `#` characters) |
| `M` | Horizontal rule | `---` on its own line |
| `M` | Bullets and numbering | `- Item`, `1. Item`; indent to nest |
| `M` | Tasks | `- [ ] Pending`, `- [x] Done` |
| `M` | Quotes | `> Quoted text` |
| `M` | Bold, italic, strike | `**bold**`, `*italic*`, `~~strike~~` |
| `M` | Inline code | `` `code` `` |
| `M` | Code blocks | Enclose with standalone `` ```text `` and `` ``` `` lines |
| `M` | Tables | Header and `---` separator rows; vertical bars separate cells |
| `M` | File links | `[Format reference](CLRO_Note_Format.md)`; reference-style links also work |
| `M` | Image references | `![description]` followed by `(image path)`; displays description and reference |
| `M` | Literal punctuation | `\*`, inline code, and character references such as `&amp;` |
| `c` | Underline | `<u>text</>` |
| `c` | Text and background color | `<char=1a73e8>text</>`, `<back=fff2cc>text</>` |
| `c` | Font and text size | `<font=Consolas>text</>`, `<s=24>text</>` (px) |
| `c` | Indentation and position | `<d=2>text</>`, `<d=center>text</>` |
| `c` | Line height | `<m=5>text</>` (1.5 times baseline) |
| `c` | Note/PDF link points | `<l=topic-a>text</>`; create the PDF point with Link (PDF link) |
| `c` | Containers | Enclose with standalone `::: Note` and `:::` lines |
| `c` | Block titles | Add `title="Title"` to the opening line |
| `c` | Math enclosures | `$x^2$`, `\(x^2\)`; use `$$...$$` or `\[...\]` for block math |
| `c` | Math tags | `<math>x^2</math>` (TeX inside) |
| `T` | Scripts, fractions, roots | Put `x_i^2`, `\frac{a}{b}`, or `\sqrt{x}` inside math delimiters |
| `T` | Matrices, cases, alignment | `\begin{pmatrix}...\end{pmatrix}` and related environments |
| `T` | Symbols and functions | `\alpha`, `\sum`, `\sin`, etc.; full supported list later |

## Fullwidth punctuation (.clro)

The Note settings include **CLRO: ASCII syntax only (disable Japanese color names)**, OFF by default. When ON, use ASCII tag delimiters, names, equals signs, quotes and separators, and English color names or numeric RGB values. `<c=red>` and `<c=#ff0000>` work; fullwidth forms such as `＜ｃ＝red＞` and the Japanese color aliases listed below do not apply color decoration. Japanese prose and font names, and the saved source, are preserved. This preference applies to `.clro` notes.

In `.clro` notes, fullwidth syntax is supported within style tags. Use halfwidth Markdown markers for headings, emphasis, strikethrough, lists, tables, links, code, and containers. Fullwidth prose such as `～(example)`, `～～`, and `＊＊` retains its literal appearance.

Existing style tags accept fullwidth brackets, slashes, equals signs, and quotes, for example `＜u＞underline＜／＞` and `＜char＝＂red＂＞red＜／char＞`. Letters in tag and attribute names may also be fullwidth: `<ｃ＝red>red text</>`, `<Ｃ=red>red text</>`, and `＜ｂａｃｋ＝lightyellow＞background＜／＞`. Halfwidth/fullwidth letters may be mixed; name matching is case-insensitive. Prose, attribute values (including color names, font names, and link IDs), link destinations, and code content retain their original characters. For example, the value `ｒｅｄ` is distinct from the color name `red`.

Use halfwidth delimiters, tags, and TeX commands for math. `＄x＄` and `＼（x＼）` are literal text; fullwidth characters inside math are never folded. Saved source retains your spelling. This punctuation support does not apply to `.md`, `.markdown`, or `.txt`.

For Markdown output, **Convert fullwidth syntax to halfwidth (CLRO → md)** defaults to ON. It converts punctuation and names in recognized style tags. Prose characters, code content, math, link destinations, and attribute values stay unchanged. Turn it OFF to retain the original spelling. Each queued export retains its own choice. This option does not apply to HTML or TXT.

## Apply with input assist

Type in the lower-left input field, choose formatting, then Apply or Enter. To format existing text, leave the field empty and select note text instead. Bullet, Quote, Page reference, and Link (PDF link) are available in the same panel.

Open this document with **Writing guide**, even before opening a note. Use `.clro`, `.md`, or `.markdown` for formatted notes.

## Detailed writing instructions

Code blocks contain text you can copy. Compare source and display as you try an example.

### Paragraphs, headings, and rules `M`

Separate paragraphs with a blank line. Ordinary newlines belong to the paragraph. For an explicit hard break, end a line with two spaces or a `\`. Visual wrapping does not insert newlines into the document.

```text
# Main heading
## Chapter
### Section
#### Subsection
##### Smaller heading
###### Smallest heading

First paragraph.

Next paragraph.

---
```

Put a space after the heading's `#` characters. `***` and `___` also form rules. A line of `===` below text can form a heading, but this app treats `---` as a horizontal rule. Prefer hash headings for clarity.

### Bullets, numbering, tasks, and quotes `M`

```text
- Item
- Next item
  - Nested item

1. First
2. Next

- [ ] Pending
- [x] Done

> Quoted text
> Continued
>
> Another quoted paragraph
```

Use `-`, `*`, or `+` for bullets and `1.` or `1)` for numbering, followed by a space. Indent nested content to the parent's content position. Set task status in the source with `[ ]`, `[x]`, or `[X]`. Quotes may contain headings and lists.

### Inline formatting `M`

| Syntax | Meaning |
| --- | --- |
| `**bold**` or `__bold__` | Bold |
| `*italic*` or `_italic_` | Italic |
| `***bold italic***` | Both |
| `~~strike~~` | Strikethrough |
| `` `code` `` | Inline code; do not interpret its contents |
| `\*text\*` | Literal punctuation |

Do not put spaces between emphasis markers and their text. For underline, use `<u>text</>`. When inline code contains backticks, enclose it in a longer run of backticks.

### Show code and syntax literally `M`

The four outer backticks below display an example containing three backticks. When writing a code block in a note, use the inner three-backtick opening and closing lines.

````text
```text
**This stays literal**
<char=ff0000>This does not change color</>
$x^2$ stays literal too
```
````

Tilde fences (`~~~`) also work. Closing fences use the same character and at least as many repetitions as the opening fence. An opening label such as `text` identifies the language; it does not execute code. Use fences rather than four-space indented code blocks.

### Tables `M`

```text
| Left | Center | Right |
| :--- | :---: | ---: |
| Text | Text | 123 |
| Next | **Bold** | 456 |
```

Place the separator row after the header and use `|` between cells. Write `\|` for a literal pipe inside a cell. Columns normally align in composed view; editing switches the whole table to source. Use horizontal scrolling for a wide table.

### Links and image references `M`

```text
[Another note](another-note.md)
[Name with spaces](<another note.md>)
[Reference][ref]

[ref]: reference.md

![Image description](figure.png)
```

Inline and reference-style links can be written. Image syntax provides description text and a reference target; it does not embed a rendered image in the note. Writing a URL does not initiate communication or downloads. Link display and supported in-app navigation are separate capabilities.

### Custom formatting attributes `c`

Use `<attribute, attribute=value>text</>`. Separate attributes with commas or whitespace and quote values containing spaces with single or double quotes. `</>` closes the innermost open formatting span. Named closes such as `</char>` are also accepted. Code and math bodies do not interpret ordinary custom formatting.

| Attribute and aliases | Example | Meaning |
| --- | --- | --- |
| `u` | `<u>underline</>` | Underline |
| `b`, `bold`, `strong` | `<b>bold</>` | Bold |
| `i`, `italic`, `em` | `<i>italic</>` | Italic |
| `x`, `s`, `strike`, `del` without a value | `<x>strike</>` | Strikethrough. `s=number` instead sets text size |
| `char`, `c` | `<char=red>color</>` | Named or six-digit RGB text color; see the table below |
| `back` | `<back=fff2cc>background</>` | Background color |
| `font`, `f` | `<font="Yu Gothic">text</>` | Font name; available fonts depend on the system |
| `s` with a value | `<s=24>large</>` | Text size in px. Also accepts `24px`, `+2`, and `-2` |
| `m` | `<m=5>more height</>` | Add five units of 0.1 to baseline line height (1.5 times). `m+0.5` and `m-0.5` are relative; bare `m` adds 0.5 unit |
| `d` with an integer | `<d=2>indent</>` | Indentation in character-width units |
| `d` with a position | `<d=center>center</>` | `left`, `center`, or `right`; `<d="right -1">text</>` offsets inward by one character |
| `l`, `link` | `<l=topic-a>point</>` | Note/PDF link identifier |
| `la`, `linkaccent` | `<l=topic-a, la>link</>` | Link accent color |
| `lu`, `linkunderline` | `<l=topic-a, lu>link</>` | Link underline |

```text
<char=1a73e8, back=fff2cc, u>Combined formatting</>
<font=Consolas><s=24>Font and size</></>
<back=fff2cc>Outer <u>inner</> outer again</>
```

### Named colors `c`

Use English or Japanese names, for example `<char=red>red text</>` or `<back=lightyellow>pale yellow background</>`. English names are case-insensitive. Quoted values also work.

To use a Japanese alias, enter the decoded characters. The `\uXXXX` sequences below are Unicode reference notation; they are not escape syntax for note input. The English names can be entered directly.

| English | Japanese aliases (Unicode escapes) | RGB |
| --- | --- | --- |
| `black` | `\u9ed2` / `\u9ed2\u8272` | `#000000` |
| `white` | `\u767d` / `\u767d\u8272` | `#FFFFFF` |
| `gray` / `grey` | `\u7070` / `\u7070\u8272` | `#808080` |
| `lightgray` / `lightgrey` | `\u8584\u7070` / `\u8584\u7070\u8272` | `#D3D3D3` |
| `red` | `\u8d64` / `\u8d64\u8272` | `#FF0000` |
| `orange` | `\u30aa\u30ec\u30f3\u30b8` / `\u30aa\u30ec\u30f3\u30b8\u8272` / `\u6a59` / `\u6a59\u8272` | `#FFA500` |
| `yellow` | `\u9ec4` / `\u9ec4\u8272` | `#FFFF00` |
| `green` | `\u7dd1` / `\u7dd1\u8272` | `#008000` |
| `lime` | `\u30e9\u30a4\u30e0` / `\u660e\u308b\u3044\u7dd1` / `\u660e\u308b\u3044\u7dd1\u8272` | `#00FF00` |
| `blue` | `\u9752` / `\u9752\u8272` | `#0000FF` |
| `navy` | `\u7d3a` / `\u7d3a\u8272` | `#000080` |
| `cyan` | `\u30b7\u30a2\u30f3` / `\u6c34\u8272` | `#00FFFF` |
| `purple` | `\u7d2b` / `\u7d2b\u8272` | `#800080` |
| `magenta` | `\u30de\u30bc\u30f3\u30bf` / `\u8d64\u7d2b` / `\u8d64\u7d2b\u8272` | `#FF00FF` |
| `pink` | `\u30d4\u30f3\u30af` / `\u30d4\u30f3\u30af\u8272` / `\u6843\u8272` | `#FFC0CB` |
| `brown` | `\u8336` / `\u8336\u8272` | `#A52A2A` |
| `lightred` | `\u8584\u8d64` / `\u8584\u8d64\u8272` | `#FFE4E4` |
| `lightorange` | `\u8584\u6a59` / `\u8584\u6a59\u8272` / `\u8584\u30aa\u30ec\u30f3\u30b8` | `#FFF0DB` |
| `lightyellow` | `\u8584\u9ec4` / `\u8584\u9ec4\u8272` | `#FFF9C4` |
| `lightgreen` | `\u8584\u7dd1` / `\u8584\u7dd1\u8272` | `#E5F3E5` |
| `lightblue` | `\u8584\u9752` / `\u8584\u9752\u8272` | `#E3F0FF` |
| `lightpurple` | `\u8584\u7d2b` / `\u8584\u7d2b\u8272` | `#F0E6FA` |
| `lightpink` | `\u8584\u6843` / `\u8584\u6843\u8272` / `\u8584\u30d4\u30f3\u30af` | `#FCE4EC` |

Pale colors have independent names suitable for backgrounds. Each name has the same fixed RGB value for text and backgrounds; choosing a background does not automatically lighten a base color. For adaptive high contrast instead of a fixed RGB color, use `hicont`, `hi-contrast`, `highcontrast`, or `high-contrast`. Its Japanese alias is `\u9ad8\u30b3\u30f3\u30c8\u30e9\u30b9\u30c8` in the Unicode notation above.

Six-digit RGB forms (`1a73e8`, `#1a73e8`, and `0x1a73e8`) remain supported. Unknown names and invalid values do not apply a color. Source and Markdown output keep the original names; HTML output uses the corresponding RGB value. Color tags inside code remain literal.

### Note/PDF links `c`

Custom note links connect locations with the same identifier. `l` is an alias for `link`; `la` applies an accent color and `lu` underlines the link.

```text
<link=topic-a, la, lu>Source</>

<link=topic-a>Destination</>
```

Use **Link** (PDF link) to enter link creation, click the note position and the corresponding PDF position, and finish when both points exist. Choosing Link again ends pending creation and removes incomplete PDF markers. See [link operations](Help_Reference.md#markup-math-and-links). Writing a note identifier alone does not create the PDF point. **Page reference** inserts text with a page number, a separate operation from the position link.

### Container blocks `c`

```text
::: note
**Additional information.**

Lists, tables, and equations can go inside.
:::
```

Put opening and closing markers on separate lines. The opening text becomes the title; the body can use other syntax.

### Math delimiters `c`

Math display is experimental and supports part of TeX. Use `$...$` or `\(...\)` inline, and `$$...$$` or `\[...\]` for a separate equation.

```text
Speed is $v = \frac{d}{t}$.

$$
E = mc^2
$$

\(a_1 + a_2\)

\[
\sqrt{x^2+y^2}
\]
```

Markdown notes also accept `<math>...</math>` and `<math>...</>`. A tag in a sentence is inline; a standalone enclosure is a block. Set `display=inline` or `display=block` explicitly if needed; values may be quoted. Keep the opening tag on one line.

```text
The equation is <math>E=mc^2</math>.

<math display="block">
\frac{1}{2}
</math>
```

## Advanced writing and reference

### Titles and nesting `c`

An opening line such as `` ```cpp title="example.cpp" `` displays a title separately from the language name. The title is literal text, without interpreting its syntax.

Put opening and closing markers on separate lines. Text such as `note` on the opening line is displayed as a title; `::: note title="Additional information"` specifies it explicitly. `title=""` hides the title; malformed quotes and duplicate `title=` attributes also produce no title.

A named opening inside creates a nested container. An unnamed `:::` can also open one; closing markers have at least the opening's number of colons. Up to three leading spaces are allowed. An unclosed container continues to the end of the document. Titles are literal text and do not enable folding or CSS.

### Attribute details `c`

The color's `#` is optional. `hicont`, `hi-contrast`, `highcontrast`, or `high-contrast` selects a high-contrast color for readability. Boolean formatting accepts forms such as `u=1`, `b=true`, `i=on`, and `lu=yes`. `false` or `0` does not cancel outer formatting. Text size is bounded to 6-256px and additional line height to 0-60 units. Unknown attributes do not provide arbitrary HTML or CSS.

### Compatibility headings and scope endings `c`

For compatibility with existing notes, standalone `<h1>heading</>` through `<h6>heading</>` lines also form headings. `</#>`, `</##>`, and corresponding levels close heading-related indentation and formatting scopes. Prefer Markdown headings for ordinary notes.

### Math tag attributes and constraints `c`

The math tag, attribute names, and display values ignore case. Unclosed or nested tags, and invalid or repeated `display`, remain source text. Unknown attributes have no display effect. Inline math stays on one line; block equations have no surrounding sentence text. MathML and self-closing `<math/>` are unsupported.

### Supported math forms `T`

| Content | Examples |
| --- | --- |
| Scripts and grouping | `x^{2}`, `a_{ij}`, `x_i^2`, `{a+b}` |
| Fractions | `\frac{a}{b}`, `\dfrac{a}{b}`, `\tfrac{a}{b}` |
| Roots | `\sqrt{x}`, `\sqrt[3]{x}` |
| Binomial coefficient | `\binom{n}{r}` |
| Delimiters | `\left(\frac{a}{b}\right)`, `\left\{x\right\}` |
| Accents | `\bar{x}`, `\overline{x}`, `\hat{x}`, `\widehat{x}`, `\vec{x}`, `\dot{x}`, `\ddot{x}` |
| Sum, product, integral, limit | `\sum_{i=1}^{n}i`, `\prod_i x_i`, `\int_0^1 x\,dx`, `\oint f`, `\lim_{x\to 0}f(x)` |
| Script placement | `\limits`, `\nolimits` |
| Text groups | `\text{description}`, `\operatorname{rank}` |
| Spacing | `\,`, `\;`, `\:`, `\quad`, `\qquad` (simplified spaces) |

`\!` adds no space. `\displaystyle`, `\textstyle`, `\mathrm`, `\mathbf`, `\mathit`, `\mathsf`, `\mathtt`, `\mathbb`, `\mathcal`, and `\textrm` are accepted without guaranteeing a TeX system's fonts or typesetting.

For matrices, cases, and aligned equations, separate columns with `&` and rows with `\\`. Supported environments are `matrix`, `pmatrix`, `bmatrix`, `Bmatrix`, `vmatrix`, `Vmatrix`, `cases`, `aligned`, and `align`.

```text
$$
\begin{pmatrix}
a & b \\
c & d
\end{pmatrix}
$$
```

#### Math symbols and functions

Prefix each supported command name below with `\`, preserving case: for example, `\alpha` and `\Gamma`.

| Group | Names |
| --- | --- |
| Lowercase Greek | `alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi pi rho sigma tau upsilon phi chi psi omega` |
| Uppercase Greek | `Gamma Delta Theta Lambda Xi Pi Sigma Upsilon Phi Psi Omega` |
| Functions | `sin cos tan log ln exp sinh cosh tanh lim limsup liminf` |
| Operators | `cdot times div pm sum prod int oint` |
| Relations | `leq le geq ge neq approx equiv in ni notin subset subseteq supset supseteq` |
| Sets and logic | `cap cup setminus forall exists` |
| Arrows | `to rightarrow leftarrow leftrightarrow Rightarrow Leftarrow Leftrightarrow` |
| Other | `infty partial nabla angle circ ldots cdots langle rangle mid` |

Unknown math commands may remain as their names. Package loading, macro definitions, and compilation of a complete TeX document are unsupported. Check important equations after saving or exporting.

### Detailed input assist behavior

| Action | Result in the note |
| --- | --- |
| Type in the input field; choose Apply or press Enter | Insert with the selected formatting. If note text is selected, insert immediately after it |
| Leave the input field empty; select note text; choose Apply | Apply formatting to the selected text |
| Apply with both the field and selection empty | Show guidance; leave the note unchanged |
| Bullet or Quote | Prefix typed text or each selected line with `- ` or `> `. With neither, insert a starting marker |
| Page reference | Insert the current PDF's name and page number as text |
| Link (PDF link) | Start linking a PDF position with the note |

Choose heading levels 1 through 6. Combine bold, italic, strike, underline, text color, and background. `d` controls indentation, `m` adds line height, and `s` sets text size; see the Custom formatting attributes table for units and examples. Leave unused numeric fields empty.

Input spaces are preserved and no trailing newline is added automatically. Headings, bullets, and quotes add the line breaks needed to separate them from surrounding text. Enter commits IME conversion first; apply after conversion ends. The field clears only after successful insertion and retains text on failure. Use the note's Undo and Redo to reverse changes.

### Formats and display

`.clro`, `.md`, and `.markdown` share the Markdown note syntax. `.txt` and `.csv` are plain text without formatting interpretation. `.tex` has a separate TeX math route; use a Markdown note for Markdown and the custom formatting described here.

Composed view displays formatting while the line being edited or selected shows its source. Tables and block equations switch as whole editing units. Choose source view to keep syntax visible, and enable the math display mode for equations. The read-only viewer's document display and the editor's note display differ.

### Preserve syntax literally and understand limits

Escape Markdown punctuation with `\`, or use inline code and fenced code blocks. Code keeps `$`, `<math>`, custom formatting, and `:::` literal. Character references also have named, decimal, and hexadecimal forms such as `&amp;`, `&#169;`, and `&#xA9;`.

The editor does not execute or render arbitrary HTML, CSS, or JavaScript. HTML comments and YAML front matter are not hidden metadata. Mermaid remains code in the editing app. Support does not imply every Markdown extension or identical rendering in other software.

See [Note format](CLRO_Note_Format.md) for format choices, [Help reference](Help_Reference.md) for controls and settings, and [Read-only viewer](Read_Only_Viewer.md) for search and source display.
