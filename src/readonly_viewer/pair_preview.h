#pragma once
#include <windows.h>
#include <optional>
namespace readonly_viewer {
[[nodiscard]] std::optional<int> RunPdfPairPreviewCommand(HINSTANCE instance, int show);
}
