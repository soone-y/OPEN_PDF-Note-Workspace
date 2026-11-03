#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

namespace reader_next {
constexpr COLORREF kCanvas = RGB(218, 226, 234);
constexpr COLORREF kSidebar = RGB(227, 234, 241);
constexpr COLORREF kToolbar = RGB(238, 242, 246);
constexpr COLORREF kPaper = RGB(253, 252, 249);
constexpr COLORREF kInk = RGB(32, 44, 56);
constexpr COLORREF kMuted = RGB(95, 112, 128);
constexpr COLORREF kAccent = RGB(0, 105, 99);
constexpr COLORREF kSelection = RGB(190, 219, 217);
constexpr UINT kFolderBackId = 819;
constexpr UINT kFolderUpId = 821;
constexpr UINT kSearchId = 810;
constexpr UINT kFindNextId = 811;
constexpr UINT kFindPreviousId = 812;
constexpr UINT kZoomInId = 813;
constexpr UINT kZoomOutId = 814;
constexpr UINT kZoomResetId = 815;
constexpr UINT kFocusId = 816;
constexpr UINT kSearchStepMessage = WM_APP + 20;
// Main WindowProc timer ledger: 210 = debounced, sliced text-search overview.
constexpr UINT_PTR kSearchOverviewTimer = 210;
// UI-thread-owned, transient search overview. Native RichEdit supplies UTF-16
// match coordinates and wrapped line numbers; scanning never changes selection.
class SearchOverview {
public:
    enum class Result { Pending, Complete, Failed };
    [[nodiscard]] bool Reset(HWND editor, const std::wstring& query, int overviewHeight);
    [[nodiscard]] Result Step(HWND editor);
    void Clear();
    [[nodiscard]] bool Active() const { return active_; }
    [[nodiscard]] int TotalLines() const { return totalLines_; }
    [[nodiscard]] const std::vector<int>& Lines() const { return lines_; }
private:
    bool active_ = false;
    LONG next_ = 0;
    int totalLines_ = 1;
    int overviewHeight_ = 1;
    std::wstring query_;
    std::vector<int> pendingLines_, lines_;
};
[[nodiscard]] int Px(HWND owner, int value);
void Initialize(HWND owner);
void StyleControl(HWND control, bool tree = false);
void SetActive(HWND button, bool active);
[[nodiscard]] HWND Button(HWND owner, UINT id, const wchar_t* label);
[[nodiscard]] bool DrawButton(const DRAWITEMSTRUCT& item);
void DrawTab(const DRAWITEMSTRUCT& item, const std::wstring& title, bool selected);
void Paint(HWND owner, int split, int toolbarBottom, bool empty);
[[nodiscard]] LRESULT DrawTree(NMTVCUSTOMDRAW& draw);
[[nodiscard]] HBRUSH ControlBrush(HWND control, HDC dc);
void Cleanup();
}
