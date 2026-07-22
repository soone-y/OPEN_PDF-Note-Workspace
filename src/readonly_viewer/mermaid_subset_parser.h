#pragma once

#include <string_view>

#include "readonly_viewer/mermaid_subset_model.h"

namespace textviewer::mermaid {

[[nodiscard]] ParseResult ParseFlowchart(std::wstring_view source,
                                         const ParseLimits& limits = {});

} // namespace textviewer::mermaid
