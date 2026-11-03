#pragma once

#include <windows.h>
#include <commctrl.h>
#include <new>

namespace ui::themed_push_buttons {
using DrawButton = bool (*)(const DRAWITEMSTRUCT*);

namespace detail {
inline constexpr UINT_PTR kSubclass = 0x54504254;
inline constexpr const wchar_t* kButtonState = L"PdfNote.ThemePushButton.State";
inline constexpr const wchar_t* kParentState = L"PdfNote.ThemePushButton.Parent";
struct ParentState { DrawButton draw; unsigned buttons = 0; };
struct ButtonState { LONG_PTR type; HWND parent; };
inline LRESULT CALLBACK ParentProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
inline LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

[[nodiscard]] inline ButtonState* ButtonOf(HWND window) {
    return reinterpret_cast<ButtonState*>(GetPropW(window, kButtonState));
}
[[nodiscard]] inline ParentState* ParentOf(HWND window) {
    return reinterpret_cast<ParentState*>(GetPropW(window, kParentState));
}

inline void ReleaseParent(HWND parent) {
    auto* state = ParentOf(parent);
    if (state && --state->buttons == 0) {
        RemoveWindowSubclass(parent, ParentProc, kSubclass);
        RemovePropW(parent, kParentState);
        delete state;
    }
}

inline LRESULT CALLBACK ParentProc(HWND parent, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR data) {
    auto* state = reinterpret_cast<ParentState*>(data);
    if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item && item->CtlType == ODT_BUTTON) {
            const auto* button = ButtonOf(item->hwndItem);
            // Only buttons converted here belong to this renderer. Existing
            // owner-drawn swatches/previews and forwarded messages stay native.
            if (button && button->parent == parent && state->draw(item)) return TRUE;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(parent, ParentProc, kSubclass);
        RemovePropW(parent, kParentState);
        delete state;
    }
    return DefSubclassProc(parent, message, wp, lp);
}

inline LRESULT CALLBACK ButtonProc(HWND button, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR data) {
    auto* state = reinterpret_cast<ButtonState*>(data);
    if (message == WM_GETDLGCODE) {
        const LRESULT code = DefSubclassProc(button, message, wp, lp);
        return (code & ~(DLGC_UNDEFPUSHBUTTON | DLGC_DEFPUSHBUTTON)) |
               (state->type == BS_DEFPUSHBUTTON ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    }
    if (message == BM_SETSTYLE) {
        const auto type = static_cast<LONG_PTR>(wp & BS_TYPEMASK);
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
            state->type = type;
            wp = (wp & ~static_cast<WPARAM>(BS_TYPEMASK)) | BS_OWNERDRAW;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(button, ButtonProc, kSubclass);
        RemovePropW(button, kButtonState);
        ReleaseParent(state->parent);
        delete state;
    }
    return DefSubclassProc(button, message, wp, lp);
}

inline void RefreshFrame(HWND button) {
    SetWindowPos(button, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(button, nullptr, TRUE);
}

[[nodiscard]] inline bool Enable(HWND button, DrawButton draw) {
    if (ButtonOf(button)) return true;
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    const LONG_PTR type = style & BS_TYPEMASK;
    // Types are an enum. Checkboxes, radios, groups and dedicated owner-drawn
    // buttons retain their own behavior and drawing contract.
    if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return true;
    const HWND parent = GetParent(button);
    auto* state = new (std::nothrow) ButtonState{type, parent};
    if (!state) return false;
    auto* host = ParentOf(parent);
    if (!host) {
        host = new (std::nothrow) ParentState{draw};
        if (!host) { delete state; return false; }
        if (!SetPropW(parent, kParentState, reinterpret_cast<HANDLE>(host))) {
            delete host;
            delete state;
            return false;
        }
        if (!SetWindowSubclass(parent, ParentProc, kSubclass, reinterpret_cast<DWORD_PTR>(host))) {
            RemovePropW(parent, kParentState);
            delete host;
            delete state;
            return false;
        }
    }
    ++host->buttons;
    if (!SetPropW(button, kButtonState, reinterpret_cast<HANDLE>(state))) {
        ReleaseParent(parent);
        delete state;
        return false;
    }
    if (!SetWindowSubclass(button, ButtonProc, kSubclass, reinterpret_cast<DWORD_PTR>(state))) {
        RemovePropW(button, kButtonState);
        ReleaseParent(parent);
        delete state;
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(button, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW) &&
        GetLastError() != ERROR_SUCCESS) {
        RemoveWindowSubclass(button, ButtonProc, kSubclass);
        RemovePropW(button, kButtonState);
        ReleaseParent(parent);
        delete state;
        return false;
    }
    RefreshFrame(button);
    return true;
}

[[nodiscard]] inline bool Restore(HWND button) {
    auto* state = ButtonOf(button);
    if (!state) return true;
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(button, GWL_STYLE, (style & ~BS_TYPEMASK) | state->type) &&
        GetLastError() != ERROR_SUCCESS) return false;
    RemoveWindowSubclass(button, ButtonProc, kSubclass);
    RemovePropW(button, kButtonState);
    ReleaseParent(state->parent);
    delete state;
    RefreshFrame(button);
    return true;
}

struct ApplyContext { bool enabled; DrawButton draw; bool ok = true; };
inline BOOL CALLBACK ApplyChild(HWND child, LPARAM data) {
    auto& context = *reinterpret_cast<ApplyContext*>(data);
    wchar_t name[32]{};
    if (GetClassNameW(child, name, 32) && _wcsicmp(name, L"Button") == 0) {
        const bool ok = context.enabled ? Enable(child, context.draw) : Restore(child);
        context.ok = context.ok && ok;
    }
    return TRUE;
}
} // namespace detail

// UI-thread only. The window subtree owns registrations until restore/destroy;
// the caller supplies a renderer without sharing application state here.
[[nodiscard]] inline bool Apply(HWND root, bool enabled, DrawButton draw) {
    if (!root || !IsWindow(root) || (enabled && !draw)) return false;
    detail::ApplyContext context{enabled, draw};
    EnumChildWindows(root, detail::ApplyChild, reinterpret_cast<LPARAM>(&context));
    return context.ok;
}
} // namespace ui::themed_push_buttons
