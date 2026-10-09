#pragma once

#include "core/annot_types.h"

namespace pdf_annotation_export {

// Caller owns this new, empty output document and closes it with PDFium.
// A controlled memory initializer supplies valid indirect default-font
// resources and an empty Fields array. It never reads a user file or path.
[[nodiscard]] FPDF_DOCUMENT CreateDocument();

// Output-copy boundary only. Callers own a newly created destination document
// and an appearance-only scratch page in it; never pass the opened source page.
// On failure the caller discards the whole destination before any disk write.
// Successful object-based appearances are transferred from scratch to annotation.
[[nodiscard]] bool Add(FPDF_DOCUMENT output, FPDF_PAGE destination, FPDF_PAGE scratch,
                       const Annotation& annotation, bool preferFreeText);
// Until all native geometry/AP/resource types can be transformed losslessly,
// reject non-unit scaling of annotated input PDFs. App-side annotations are
// added AFTER page scaling and are not affected by this restriction.
[[nodiscard]] bool CanScaleExisting(FPDF_PAGE destination, double scale);

// PDFium's creation API leaves direct annotation dictionaries until its
// annotation list is constructed. Do this explicitly, never as a render side
// effect, so saved annotations have interoperable indirect references and APs.
[[nodiscard]] bool Finalize(FPDF_PAGE destination);

} // namespace pdf_annotation_export
