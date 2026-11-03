#pragma once

namespace readonly_viewer {
// UI-thread-owned, transient coordinates. No file state or retained PDFium handle.
struct PdfViewportAnchor {
    int page = -1;
    double x_ratio = 0.0;
    double y_ratio = 0.0;
    double pixels_per_point = 0.0;
    bool captured = false;
};
}
