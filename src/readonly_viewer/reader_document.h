#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "readonly_viewer/mermaid_subset_model.h"
enum TextStyle : unsigned int {
    TextStyleNone = 0,
    TextStyleBold = 1 << 0,
    TextStyleItalic = 1 << 1,
    TextStyleCode = 1 << 2,
    TextStyleStrike = 1 << 3,
    TextStyleLink = 1 << 4,
};

struct StyleRun {
    size_t start = 0;
    size_t end = 0;
    unsigned int style = TextStyleNone;
    int headingLevel = 0;
};

struct ReaderHeading { int level = 1; size_t offset = 0; std::wstring title; size_t sourceOffset = 0; };
struct ReaderTable { size_t start = 0, end = 0; std::vector<std::vector<std::wstring>> rows; std::vector<int> alignment; };
struct RenderedDocument {
    std::wstring text;
    std::vector<StyleRun> styles;
    std::vector<ReaderHeading> headings;
    std::vector<ReaderTable> tables;
};

struct InlineDiagram {
    size_t text_offset = 0;
    size_t reserved_length = 0;
    textviewer::mermaid::DiagramModel model;
};

std::wstring_view TrimMarkdownLine(std::wstring_view line);

struct ByteRange {
    uintmax_t start = 0;
    uintmax_t end = 0;
};

// Pure, UI-thread-owned preparation. No I/O, external objects or HTML execution.
void AppendStyledText(RenderedDocument& document, std::wstring_view text, unsigned int style, int headingLevel);
[[nodiscard]] RenderedDocument RenderMarkdown(const std::wstring& source, std::vector<InlineDiagram>* inline_diagrams = nullptr);
[[nodiscard]] std::string BuildReaderRtf(const RenderedDocument& document, int widthTwips);
[[nodiscard]] int ReaderHeadingFontHalfPoints(int level);
