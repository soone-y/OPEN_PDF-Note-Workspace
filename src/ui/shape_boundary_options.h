#pragma once
#include "core/shape_boundary.h"
#include "core/localization.h"
#include <windows.h>
#include <initializer_list>

namespace ui {
inline void PopulateLineStyleOptions(HWND combo,bool shape,bool allowDash=true) {
    for (const auto value : {ShapeLineStyle::Solid,ShapeLineStyle::Dashed,ShapeLineStyle::BlackBoundary,ShapeLineStyle::WhiteBoundary}) {
        if(value==ShapeLineStyle::Dashed && !allowDash)continue;
        if(!shape && value>=ShapeLineStyle::BlackBoundary)continue;
        const auto id = value == ShapeLineStyle::BlackBoundary ? L"shape.boundary.black" :
            value == ShapeLineStyle::WhiteBoundary ? L"shape.boundary.white" :
            value == ShapeLineStyle::Dashed ? L"ui.layout.c93df0074dab" : L"ui.layout.93c21b87d6e3";
        const auto index = SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(localization::Text(id).c_str()));
        if (index != CB_ERR && index != CB_ERRSPACE) SendMessageW(combo,CB_SETITEMDATA,index,static_cast<LPARAM>(value));
    }
    SendMessageW(combo,CB_SETDROPPEDWIDTH,180,0);
}
inline bool SyncLineStyleOption(HWND combo,ShapeLineStyle value,bool shape,bool allowDash=true) {
    const int expected=1+(allowDash?1:0)+(shape?2:0);
    bool changed=SendMessageW(combo,CB_GETCOUNT,0,0)!=expected;
    // Arc and ordinary line tools both have fewer choices than a closed shape.
    if(!changed && expected>1)
        changed=SendMessageW(combo,CB_GETITEMDATA,1,0)!=static_cast<LPARAM>(allowDash?ShapeLineStyle::Dashed:ShapeLineStyle::BlackBoundary);
    if(changed) {
        SendMessageW(combo,CB_RESETCONTENT,0,0);
        PopulateLineStyleOptions(combo,shape,allowDash);
    }
    for (int i=0;i<SendMessageW(combo,CB_GETCOUNT,0,0);++i) {
        if (SendMessageW(combo,CB_GETITEMDATA,i,0) != static_cast<LPARAM>(value)) continue;
        if (SendMessageW(combo,CB_GETCURSEL,0,0)==i) return changed;
        SendMessageW(combo,CB_SETCURSEL,i,0);return true;
    }
    return changed;
}
} // namespace ui
