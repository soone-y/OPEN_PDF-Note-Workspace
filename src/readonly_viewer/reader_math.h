#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "readonly_viewer/reader_document.h"

namespace mathrender { class Formula; }

namespace readonly_viewer {

// Formulas found in one opened document. UI-thread owned, display only.
struct ReaderMathSet {
    std::vector<ReaderMathSpan> spans;
    std::vector<std::shared_ptr<const mathrender::Formula>> formulas; // Parallel to spans.
    std::map<std::tuple<size_t, bool, int, int>, std::string> pictures;
    std::map<std::tuple<size_t, bool, int, int>, int> descents; // Twips below the text baseline.
};

enum class ReaderMathSource { Note, TeX };

// Uses the main application's note/TeX parser so delimiters, escapes and code
// exclusions match the editor. Spans that MicroTeX cannot parse stay source text.
[[nodiscard]] std::shared_ptr<ReaderMathSet> CollectReaderMath(const std::wstring& fileName,
                                                               const std::wstring& text,
                                                               ReaderMathSource source);

// Complete RTF picture group (PNG) for one formula, or empty on failure.
[[nodiscard]] std::string BuildReaderMathPicture(ReaderMathSet& set, const ReaderMath& math, int dpi);
// RichEdit places an image on the baseline and ignores RTF \dn for it, so the
// caller lowers each inline picture character by this many twips after loading.
[[nodiscard]] int ReaderMathDescentTwips(const ReaderMathSet& set, const ReaderMath& math, int dpi);

} // namespace readonly_viewer
