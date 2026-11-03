#pragma once

#include "core/shape_outline.h"
#include "fpdf_edit.h"

namespace shape_outline {
// The caller owns the returned object. Failure destroys the unfinished path.
[[nodiscard]] inline FPDF_PAGEOBJECT CreateDashedPdfPath(ShapeKind kind, double left, double top,
    double right, double bottom, double visualRotation, const std::vector<double>& dash, double wholeAngle = 0.0) {
    const double rotation = std::isfinite(visualRotation) ? visualRotation : kDefaultRotatedEllipseAngleRad;
    const auto outline = wholeAngle == 0.0 ? Build(kind,left,top,right,bottom,-rotation)
        : BuildOriented(kind,left,top,right,bottom,rotation,0,wholeAngle);
    const auto runs = Dash(outline,dash,1.0,0.75);
    FPDF_PAGEOBJECT path = nullptr;
    for (const auto& run : runs) {
        if (!path) path = FPDFPageObj_CreateNewPath(static_cast<float>(run.front().x), static_cast<float>(run.front().y));
        else if (!FPDFPath_MoveTo(path, static_cast<float>(run.front().x), static_cast<float>(run.front().y))) {
            FPDFPageObj_Destroy(path); return nullptr;
        }
        if (!path) return nullptr;
        for (size_t i = 1; i < run.size(); ++i) {
            if (!FPDFPath_LineTo(path, static_cast<float>(run[i].x), static_cast<float>(run[i].y))) {
                FPDFPageObj_Destroy(path); return nullptr;
            }
        }
    }
    return path;
}
} // namespace shape_outline
