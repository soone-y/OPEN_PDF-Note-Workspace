#pragma once

#include "note/note_model.h"

#include <string_view>
#include <vector>

namespace note {

enum class MathInputFlavor {
    Latex,
    Markup,
};

struct MathInputAnalysis {
    bool has_input = false;
    bool has_wrapping = false;
    MathInputFlavor flavor = MathInputFlavor::Markup;
    MathKind kind = MathKind::Inline;
    MathDelimiter delimiter = MathDelimiter::Dollar;
    std::wstring trimmed_text;
    std::wstring content_text;
    std::vector<Diagnostic> diagnostics;
};

MathInputAnalysis AnalyzeMathBoxInput(std::wstring_view rawText);
std::wstring MathDelimiterLabel(MathDelimiter delimiter);

} // namespace note
