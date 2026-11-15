#pragma once

#include "core/annot_types.h"
#include <memory>
#include <type_traits>

namespace pdf_annotation_detach {
struct DocumentCloser {
    // PDFium borrows memory until document close; ownership travels with pdf.
    std::shared_ptr<const std::vector<unsigned char>> backing;
    void operator()(FPDF_DOCUMENT doc) const { if (doc) FPDF_CloseDocument(doc); }
};
using Document = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, DocumentCloser>;
struct Result {
    Document pdf;
    std::vector<Annotation> annotations; // canonical PDF points; pass to clrop_bridge
    size_t unsupported = 0; // native annotations with no supported conversion
    size_t retainedForOrder = 0; // convertible annotations below retained ones
    size_t converted = 0; // clrop entries reconstructed from standard PDF fields
};

// No disk I/O or UI. Source is borrowed, never mutated. Result owns the copied
// PDF; publish PDF + clrop together only after saving/validating both elsewhere.
// On failure result is unchanged. Restore validated editable data when present
// or convert supported standard annotations from their PDF fields.
// Retain annotations below a later unsupported annotation to preserve Z order.
[[nodiscard]] bool Detach(FPDF_DOCUMENT source, Result& result, std::wstring& error);

// Export-copy boundary: preserve the complete editable annotation, including
// types represented by Ink/Stamp in PDF. Never call on an opened source PDF.
// Called after PDFium has finalized the newly created annotation appearance.
[[nodiscard]] bool Seal(FPDF_ANNOTATION destination);
[[nodiscard]] bool Attach(FPDF_ANNOTATION destination, const Annotation& annotation);
} // namespace pdf_annotation_detach
