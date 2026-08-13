#pragma once

#include <windows.h>
#include <windowsx.h>

#include <cstddef>
#include <initializer_list>
#include <optional>

namespace ui::context_menu {

// Owns a popup menu and closes it on every return path after creation.
class PopupMenu final {
public:
    PopupMenu() : handle_(CreatePopupMenu()) {}
    ~PopupMenu() { Close(); }

    PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;

    [[nodiscard]] explicit operator bool() const { return handle_ != nullptr; }
    [[nodiscard]] HMENU get() const { return handle_; }

    void Close() {
        if (!handle_) return;
        DestroyMenu(handle_);
        handle_ = nullptr;
    }

    [[nodiscard]] UINT TrackCommand(HWND owner, POINT screenPoint, UINT flags) const {
        if (!handle_) return 0;
        return TrackPopupMenu(handle_, flags, screenPoint.x, screenPoint.y, 0, owner, nullptr);
    }

private:
    HMENU handle_ = nullptr;
};

struct ListBoxContextTarget {
    int index = -1;
    POINT screenPoint{};
};

struct ListBoxContextOptions {
    bool useCurrentSelectionWhenPointerMisses = false;
    int fallbackIndexWhenPointerMisses = -1;
    bool selectResolvedItem = true;
    bool selectOnKeyboardInvocation = false;
    bool invalidateAfterSelection = false;
};

// Resolves both mouse- and keyboard-invoked list-box context menus consistently.
[[nodiscard]] inline std::optional<ListBoxContextTarget> ResolveListBoxContextTarget(
    HWND list,
    LPARAM lParam,
    std::size_t itemCount,
    bool keyboardInvocation,
    ListBoxContextOptions options = {}) {
    if (!list || itemCount == 0) return std::nullopt;

    int index = -1;
    POINT screenPoint{};
    if (keyboardInvocation) {
        index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(itemCount)) return std::nullopt;

        RECT itemRect{};
        if (SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
            screenPoint = {itemRect.left + 18, (itemRect.top + itemRect.bottom) / 2};
            ClientToScreen(list, &screenPoint);
        } else {
            GetCursorPos(&screenPoint);
        }
    } else {
        screenPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT clientPoint = screenPoint;
        ScreenToClient(list, &clientPoint);
        const DWORD hit = static_cast<DWORD>(
            SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(clientPoint.x, clientPoint.y)));
        if (HIWORD(hit) != 0) {
            if (!options.useCurrentSelectionWhenPointerMisses) return std::nullopt;
            index = options.fallbackIndexWhenPointerMisses >= 0
                ? options.fallbackIndexWhenPointerMisses
                : static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        } else {
            index = static_cast<int>(LOWORD(hit));
        }
        if (index < 0 || index >= static_cast<int>(itemCount)) return std::nullopt;
    }

    const bool updateSelection = options.selectResolvedItem &&
        (!keyboardInvocation || options.selectOnKeyboardInvocation);
    if (updateSelection) {
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
        if (options.invalidateAfterSelection) InvalidateRect(list, nullptr, FALSE);
    }
    return ListBoxContextTarget{index, screenPoint};
}

struct PathContextMenuItem {
    UINT command = 0;
    const wchar_t* label = nullptr;
    bool enabled = true;
};

inline void AppendPathContextMenuItems(HMENU menu,
                                       std::initializer_list<PathContextMenuItem> items) {
    for (const PathContextMenuItem& item : items) {
        if (!item.label) continue;
        AppendMenuW(menu, MF_STRING | (item.enabled ? 0 : MF_GRAYED), item.command, item.label);
    }
}

} // namespace ui::context_menu
