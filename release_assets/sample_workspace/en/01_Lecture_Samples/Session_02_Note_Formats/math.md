# Markdown Note

Markdown structures prose with headings and lists. `'single quotation'` is ordinary text; use a **backtick** for `inline_code`.

## Headings and emphasis

**bold**, *italic*, ~~strikethrough~~, and `const value = 42;`

> Start a quotation with `>`. It separates a supporting note from the main text.

::: note char=#1a73e8 title="Supporting note"
This is a container delimited by `:::`. It keeps related supporting text together.
:::

---

## Lists

1. Read a section
2. Write the key point
- Add a detail
- [x] Checked
- [ ] Review later

## Code container

```text
title: Session 02
format: markdown
```

## Table and mathematics

| Syntax | Purpose | Example |
| :--- | :---: | ---: |
| `#` | heading | `## Subtitle` |
| `-` | list | `- item` |
| `` ` `` | inline code | `value` |

Inline math: \(a^2 + b^2 = c^2\)

Block math:

$$
\int_0^1 x^2\,dx = \frac{1}{3}
$$
