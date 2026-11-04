#pragma once
#include "core/text_selection_geometry.h"
#include "fpdf_text.h"

namespace text_selection {
// Caller owns the text-page lifetime and PDFium lock. Returned rectangles and
// run axes are in original PDF coordinates; caller projects only the rectangles.
[[nodiscard]] inline std::vector<CharBox> ReadPdf(FPDF_TEXTPAGE text) {
    const int count=FPDFText_CountChars(text);
    if(count<=0)return {};
    std::vector<CharBox> boxes(static_cast<size_t>(count));
    std::vector<Origin> origins(boxes.size());
    for(int i=0;i<count;++i) {
        auto& b=boxes[static_cast<size_t>(i)];auto& o=origins[static_cast<size_t>(i)];
        double left=0,right=0,bottom=0,top=0;
        if(FPDFText_GetCharBox(text,i,&left,&right,&bottom,&top))
            b={float(left),float(right),float(top),float(bottom)};
        const auto ch=FPDFText_GetUnicode(text,i);
        o.object=reinterpret_cast<uintptr_t>(FPDFText_GetTextObject(text,i));
        o.em=FPDFText_GetFontSize(text,i);
        // FontSize can be 1 while the text matrix supplies the visible size.
        // Origins and glyph bounds are already in page space, so use page-space
        // em height too. A rotation does not change the vector's length.
        FS_MATRIX matrix{};
        if(FPDFText_GetMatrix(text,i,&matrix)) {
            const double scale=std::hypot(matrix.c,matrix.d);
            if(std::isfinite(scale) && scale>0)o.em*=scale;
        }
        o.drawable=ch>0x20 && ch!=0x3000 && o.object && FPDFText_GetCharOrigin(text,i,&o.x,&o.y);
    }
    Classify(boxes,origins);
    // Preserve whitespace geometry for rectangular copy/search, but not bands.
    for(size_t i=0;i<boxes.size();++i) if(!origins[i].drawable) boxes[i].axis=Axis::Skip;
    return boxes;
}
} // namespace text_selection
