// file: core/ui_notify.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>
#include <vector>

enum class SoftNoticeKind {
    Info,
    Warning,
    Error
};

void ShowSoftNotice(HWND owner, const std::wstring& text, SoftNoticeKind kind = SoftNoticeKind::Info);

// Hide only the matching owner's notice when its interaction resumes. Allow
// the next explicit attempt to show the same guidance again immediately.
void DismissSoftNotice(HWND owner, const std::wstring& expectedText);

enum class SilentDialogButtons {
    Ok,
    OkCancel,
    YesNo,
    YesNoCancel,
    OkYesNoCancel
};

enum class SilentDialogResult {
    None,
    Ok,
    Cancel,
    Yes,
    No
};

enum class SilentDialogPlacement {
    CenterOwner,
    OwnerLowerLeft,
    OwnerUpperLeft
};

// A display-only path. `value` is never used for file operations; dialogs show
// only a bounded compact representation and offer explicit copying of the
// exact path that the operation reported.
struct SilentDialogPath {
    std::wstring label;
    std::wstring value;
};

struct SilentDialogOptions {
    std::wstring title;
    std::wstring message;
    SoftNoticeKind kind = SoftNoticeKind::Info;
    SilentDialogButtons buttons = SilentDialogButtons::Ok;
    std::wstring okLabel;
    std::wstring cancelLabel;
    std::wstring yesLabel;
    std::wstring noLabel;
    SilentDialogResult defaultResult = SilentDialogResult::None;
    SilentDialogResult escapeResult = SilentDialogResult::None;
    int preferredWidthPx = 0;
    SilentDialogPlacement placement = SilentDialogPlacement::CenterOwner;
    std::vector<SilentDialogPath> paths;
    // Optional display-only text following the compact paths. Empty keeps the
    // existing message/path layout for callers that do not need detail sections.
    std::wstring additionalInformation;
};

SilentDialogResult ShowSilentDialog(HWND owner, const SilentDialogOptions& options);
void ShowSilentMessageDialog(HWND owner, const std::wstring& title, const std::wstring& message,
                             SoftNoticeKind kind = SoftNoticeKind::Info,
                             const std::vector<SilentDialogPath>& paths = {});
