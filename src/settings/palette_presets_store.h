#pragma once
#include "settings/palette_presets.h"
#include "settings/palette_stock_store.h"

// Reuse the bounded, no-follow, stale-snapshot, backup-before-replace store.
// Both local palette stores share the nonblocking workspace writer lock.
namespace palette_presets {
using Snapshot = palette_stock::DocumentSnapshot<Document>;
[[nodiscard]] inline std::filesystem::path FilePath(const std::filesystem::path& root) {
    return palette_stock::FilePath(root, L"palette_presets.json");
}
[[nodiscard]] inline bool Load(const std::filesystem::path& root, Snapshot& out) {
    return palette_stock::LoadDocument(root, out, L"palette_presets.json", Parse);
}
[[nodiscard]] inline bool Save(Snapshot& state, const Document& doc) {
    return palette_stock::SaveDocument(state, doc, L"palette_presets.json", Parse, Serialize);
}
} // namespace palette_presets
