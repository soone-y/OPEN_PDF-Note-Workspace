#pragma once

#include "fpdf_signature.h"

namespace pdf_permissions {

enum class Operation { CopyText, ExportImage, ExportPdf };

// Uses the permissions of the already authenticated document. PDFium does not
// enforce these flags on text extraction, rasterization, or page import.
[[nodiscard]] inline const wchar_t* BlockMessage(FPDF_DOCUMENT document, Operation operation) {
    constexpr unsigned long copyContent = 0x10u;  // PDF access permissions, bit 5.
    if (!document) return L"pdf.permissions.unavailable";
    if ((FPDF_GetDocPermissions(document) & copyContent) == 0) {
        switch (operation) {
        case Operation::CopyText: return L"pdf.permissions.copy_denied";
        case Operation::ExportImage: return L"pdf.permissions.image_denied";
        case Operation::ExportPdf: return L"pdf.permissions.pdf_denied";
        }
    }
    if (operation != Operation::ExportPdf) return nullptr;

    // Creating or modifying a separate PDF cannot preserve source encryption
    // or a digital signature over the source bytes.
    if (FPDF_GetSecurityHandlerRevision(document) >= 0)
        return L"pdf.permissions.encrypted_pdf";
    const int signatures = FPDF_GetSignatureCount(document);
    if (signatures < 0) return L"pdf.permissions.unavailable";
    for (int index = 0; index < signatures; ++index) {
        const auto signature = FPDF_GetSignatureObject(document, index);
        // PDFium counts empty signature fields too.
        if (signature && (FPDFSignatureObj_GetContents(signature, nullptr, 0) != 0 ||
                          FPDFSignatureObj_GetByteRange(signature, nullptr, 0) != 0))
            return L"pdf.permissions.signed_pdf";
    }
    return nullptr;
}

}  // namespace pdf_permissions
