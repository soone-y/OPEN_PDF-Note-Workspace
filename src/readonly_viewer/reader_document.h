#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "readonly_viewer/mermaid_subset_model.h"
#include "readonly_viewer/file_range.h"
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

// A completed TeX math range in the original source, supplied by the shared
// note parser. Only spans the caller already proved parseable are listed.
struct ReaderMathSpan {
    size_t start = 0;
    size_t end = 0;
    bool block = false;
    std::wstring tex;
};
// One displayed formula. Its single placeholder character in RenderedDocument::text
// becomes one embedded picture, so RichEdit character positions stay aligned.
struct ReaderMath {
    size_t offset = 0;
    size_t span = 0;
    bool display = false;
    int headingLevel = 0;
};
constexpr wchar_t kReaderMathPlaceholder = 0xFFFC;

struct RenderedDocument {
    std::wstring text;
    std::vector<StyleRun> styles;
    std::vector<ReaderHeading> headings;
    std::vector<ReaderTable> tables;
    std::vector<ReaderMath> maths;
};

struct InlineDiagram {
    size_t text_offset = 0;
    size_t reserved_length = 0;
    textviewer::mermaid::DiagramModel model;
};

std::wstring_view TrimMarkdownLine(std::wstring_view line);

// Pure, UI-thread-owned preparation. No I/O, external objects or HTML execution.
void AppendStyledText(RenderedDocument& document, std::wstring_view text, unsigned int style, int headingLevel);
[[nodiscard]] RenderedDocument RenderMarkdown(const std::wstring& source, std::vector<InlineDiagram>* inline_diagrams = nullptr,
                                              const std::vector<ReaderMathSpan>* math = nullptr);
// TeX source stays literal except completed math; sectioning commands only feed the outline.
[[nodiscard]] RenderedDocument RenderTeXSource(const std::wstring& source, const std::vector<ReaderMathSpan>& math);
// Returns a complete RTF picture group for one formula, or empty to show a neutral mark.
using ReaderMathPicture = std::function<std::string(const ReaderMath&)>;
[[nodiscard]] std::string BuildReaderRtf(const RenderedDocument& document, int widthTwips,
                                         const ReaderMathPicture& picture = {});
[[nodiscard]] int ReaderHeadingFontHalfPoints(int level);
