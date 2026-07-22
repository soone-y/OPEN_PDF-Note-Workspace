// file: core/ui_notify.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>

enum class SoftNoticeKind {
    Info,
    Warning,
    Error
};

void ShowSoftNotice(HWND owner, const std::wstring& text, SoftNoticeKind kind = SoftNoticeKind::Info);

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
};

SilentDialogResult ShowSilentDialog(HWND owner, const SilentDialogOptions& options);
void ShowSilentMessageDialog(HWND owner, const std::wstring& title, const std::wstring& message,
                             SoftNoticeKind kind = SoftNoticeKind::Info);
