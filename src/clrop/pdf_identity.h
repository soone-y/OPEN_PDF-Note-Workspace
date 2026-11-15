#pragma once

#include "types.h"
#include <cmath>

// Shared by the editor and read-only viewer. Paths locate candidates; they do
// not establish PDF identity. Missing legacy fields retain the editor's rules.
namespace clrop {
[[nodiscard]] inline bool PdfIdHasFastFingerprint(const PdfId& id) {
    return id.pageCount > 0 && static_cast<size_t>(id.pageCount) == id.pageSizesPt.size();
}

[[nodiscard]] inline bool PdfIdFastMatches(const PdfId& lhs, const PdfId& rhs) {
    if (lhs.size != 0 && rhs.size != 0 && lhs.size != rhs.size) return false;
    if (PdfIdHasFastFingerprint(lhs) && PdfIdHasFastFingerprint(rhs)) {
        if (lhs.pageCount != rhs.pageCount) return false;
        for (size_t i = 0; i < lhs.pageSizesPt.size(); ++i) {
            for (size_t axis = 0; axis < 2; ++axis) {
                if (!(std::abs(lhs.pageSizesPt[i][axis] - rhs.pageSizesPt[i][axis]) <= 0.01))
                    return false;
            }
        }
        return true;
    }
    if (lhs.pageCount > 0 && rhs.pageCount > 0 && lhs.pageCount != rhs.pageCount) return false;
    return lhs.size != 0 && rhs.size != 0 && lhs.size == rhs.size;
}

[[nodiscard]] inline bool PdfIdStrongMatches(const PdfId& lhs, const PdfId& rhs) {
    return PdfIdFastMatches(lhs, rhs) && !lhs.sha256.empty() && !rhs.sha256.empty() &&
        lhs.sha256 == rhs.sha256;
}
} // namespace clrop
