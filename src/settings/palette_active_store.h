#pragma once
#include "settings/palette_stock_store.h"

// Active slots may contain equal colors (including legacy hidden white slots).
// Shares bounded no-follow reads, stale detection, lock, backup and atomic write.
namespace palette_active {
using Snapshot = palette_stock::Snapshot;
[[nodiscard]] inline bool Parse(const std::string& bytes, std::vector<COLORREF>& out) {
    return palette_stock::ParseColors(bytes, out, true);
}
[[nodiscard]] inline bool Serialize(const std::vector<COLORREF>& colors, std::string& out) {
    return palette_stock::SerializeColors(colors, out, true);
}
[[nodiscard]] inline bool Load(const std::filesystem::path& root, Snapshot& out) {
    return palette_stock::LoadDocument(root, out, L"user_palette.json", Parse);
}
[[nodiscard]] inline bool Save(Snapshot& snapshot, const std::vector<COLORREF>& colors) {
    return palette_stock::SaveDocument(snapshot, colors, L"user_palette.json", Parse, Serialize);
}
}
