#pragma once
#include "clrop/types.h"
#include <windows.h>

namespace readonly_viewer {
// Cache owns only disposable GDI rasters, bounded to 64 MiB and 16 entries.
// Neither the source Item nor PDF/CLROP files are modified.
[[nodiscard]] bool DrawAnnotationText(HDC dc, const RECT& bounds, const clrop::Item& item);
void ClearAnnotationTextCache();
}
