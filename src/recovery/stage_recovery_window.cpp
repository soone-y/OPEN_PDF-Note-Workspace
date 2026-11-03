#include "core/document_source_protection.h"
#include "core/localization.h"
#include "core/text_encoding.h"
#include "core/ui_notify.h"
#include "pdf_view/pdf_view.h"
#include "recovery/annotation_transfer.h"
#include "recovery/stage_recovery.h"
#include "ui/noop_nav_guard.h"
#include "ui/window_corners.h"
#include "workspace/workspace_write_lock.h"
#include <commctrl.h>
#include <commdlg.h>
#include <memory>
#include <richedit.h>
#include <windowsx.h>

namespace recovery {
namespace {
enum {
  Sources = 7101,
  FinalList,
  NoteEditor,
  NoteReference,
  ReferencePicker,
  Save,
  SaveAs,
  Defer,
  ChoosePdf,
  Undo,
  Redo,
  Partial
};
constexpr wchar_t viewClass[] = L"PdfStageRecoveryView";
constexpr wchar_t managerClass[] = L"PdfStageRecoveryManager";
struct Item {
  size_t source;
  int annotation = -1;
};
struct WindowState {
  Review &review;
  Candidate candidate;
  HWND view = nullptr, manager = nullptr, tree = nullptr, list = nullptr,
       editor = nullptr, reference = nullptr, picker = nullptr;
  HWND sourceTitle = nullptr, previewTitle = nullptr, finalTitle = nullptr,
       status = nullptr;
  std::vector<Candidate> undo, redo;
  std::vector<Item> items;
  std::optional<Item> drag;
  std::unique_ptr<DocumentOpenLockCandidate> selectedPdfLock;
  ReviewExit result = ReviewExit::Failed;
  bool updating = false, done = false, hover = false, closing = false;
  bool noteReady = true;
};
WindowState *ui = nullptr;
std::wstring T(const wchar_t *id) {
  return localization::Text(std::wstring(L"recovery.app.") + id);
}
std::wstring Common(const wchar_t *id) {
  return localization::Text(std::wstring(L"recovery.demo.") + id);
}
void Status(const wchar_t *id) { SetWindowTextW(ui->status, T(id).c_str()); }
HWND Control(HWND owner, const wchar_t *kind, const std::wstring &text,
             DWORD style, int id) {
  if (wcscmp(kind, L"STATIC") == 0)
    style |= SS_NOPREFIX;
  HWND window = CreateWindowExW(
      0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, owner,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
  if (window)
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
  return window;
}
std::wstring Description(const Annotation &annotation) {
  const wchar_t *key = L"annotation";
  switch (annotation.type) {
  case Annotation::Type::TextBox:
    key = L"text";
    break;
  case Annotation::Type::MathBox:
    key = L"math";
    break;
  case Annotation::Type::Shape:
    key = L"shape";
    break;
  case Annotation::Type::Freehand:
    key = L"freehand";
    break;
  case Annotation::Type::Line:
    key = L"line";
    break;
  case Annotation::Type::Arrow:
    key = L"arrow";
    break;
  case Annotation::Type::Wave:
    key = L"wave";
    break;
  case Annotation::Type::MarkerFree:
    key = L"free_marker";
    break;
  case Annotation::Type::MarkerText:
    key = L"text_marker";
    break;
  case Annotation::Type::TextColor:
    key = L"text_color";
    break;
  case Annotation::Type::LinkMarker:
    key = L"link_marker";
    break;
  }
  auto label =
      localization::Text(std::wstring(L"readonly.clrop.kind.") + key) + L" · " +
      Common(L"page_prefix") +
      std::to_wstring(static_cast<long long>(annotation.pageIndex) + 1);
  if ((annotation.type == Annotation::Type::TextBox ||
       annotation.type == Annotation::Type::MathBox) &&
      !annotation.text.empty()) {
    auto snippet = annotation.text.substr(0, 100);
    if (!snippet.empty() && snippet.back() >= 0xd800 &&
        snippet.back() <= 0xdbff)
      snippet.pop_back();
    std::replace(snippet.begin(), snippet.end(), L'\r', L' ');
    std::replace(snippet.begin(), snippet.end(), L'\n', L' ');
    label += L" — " + snippet;
  }
  return label;
}
std::wstring LogicalLines(std::wstring_view text) {
  std::wstring result;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r') {
      result += L'\n';
      if (i + 1 < text.size() && text[i + 1] == L'\n')
        ++i;
    } else
      result += text[i];
  }
  return result;
}
bool Protect() {
  if (ui->review.isNote && ui->noteReady && ui->editor && !ui->updating &&
      !ui->candidate.saved) {
    const int n = GetWindowTextLengthW(ui->editor);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    const int copied = GetWindowTextW(ui->editor, text.data(), n + 1);
    if (n > 0 && copied == 0) {
      Status(L"protect_failed");
      return false;
    }
    text.resize(static_cast<size_t>(copied));
    if (!text_encoding::IsWellFormedUtf16(text)) {
      Status(L"protect_failed");
      return false;
    }
    // RichEdit normalizes line endings. A view-only round trip must not
    // rewrite the original bytes merely because LF was displayed as CRLF.
    if (LogicalLines(text) != LogicalLines(UTF8ToWide(ui->candidate.note)))
      ui->candidate.note = WideToUTF8(text);
  }
  if (!file_output::ProtectStageRecovery(ui->review, ui->candidate)) {
    Status(L"protect_failed");
    return false;
  }
  ui->review.candidate = ui->candidate;
  return true;
}
void Layout() {
  RECT r{};
  GetClientRect(ui->view, &r);
  const int w = r.right, h = r.bottom;
  auto move = [](HWND window, int x, int y, int width, int height) {
    MoveWindow(window, x, y, std::max(1, width), std::max(1, height), TRUE);
  };
  const bool note = ui->review.isNote;
  if (note) {
    const int split = w * 3 / 5;
    move(ui->previewTitle, 12, 12, split - 24, 30);
    move(ui->editor, 12, 48, split - 24, h - 60);
    move(ui->sourceTitle, split + 8, 12, w - split - 20, 30);
    move(ui->picker, split + 8, 48, w - split - 20, 300);
    move(ui->reference, split + 8, 84, w - split - 20, h - 96);
  } else {
    const int left = std::clamp(w / 5, 210, 300),
              right = std::clamp(w / 5, 210, 280);
    move(ui->sourceTitle, 12, 12, left - 24, 30);
    move(ui->tree, 12, 48, left - 24, h - 60);
    move(ui->previewTitle, left + 8, 12, w - left - right - 16, 30);
    move(g_hPdfView, left, 48, w - left - right, h - 48);
    move(ui->finalTitle, w - right + 12, 12, right - 24, 30);
    move(ui->list, w - right + 12, 48, right - 24, h - 60);
  }
  for (HWND window : {ui->tree, ui->list, ui->finalTitle, g_hPdfView})
    ShowWindow(window, note ? SW_HIDE : SW_SHOW);
  for (HWND window : {ui->editor, ui->reference, ui->picker})
    ShowWindow(window, note ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(ui->manager, ChoosePdf), note ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(ui->manager, Partial),
             ui->review.incomplete ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(ui->manager, Save),
               ui->noteReady && ui->review.recoverable &&
                   ui->candidate.targetConfirmed);
  EnableWindow(GetDlgItem(ui->manager, SaveAs),
               ui->noteReady && !ui->candidate.saved &&
                   ui->review.recoverable &&
                   (note || !ui->candidate.pdf.empty()));
  SendMessageW(ui->editor, EM_SETREADONLY,
               !ui->noteReady || !ui->review.recoverable || ui->candidate.saved,
               0);
  EnableWindow(GetDlgItem(ui->manager, ChoosePdf), !ui->candidate.saved);
  EnableWindow(GetDlgItem(ui->manager, Undo), !ui->candidate.saved);
  EnableWindow(GetDlgItem(ui->manager, Redo), !ui->candidate.saved);
  InvalidateRect(ui->view, nullptr, TRUE);
}
void SelectFinal(int index) {
  if (index < 0 ||
      static_cast<size_t>(index) >= ui->candidate.annotations.size())
    return;
  const auto &annotation =
      ui->candidate.annotations[static_cast<size_t>(index)];
  JumpToPdfPoint(g_hPdfView, annotation.pageIndex,
                 (annotation.x1 + annotation.x2) / 2,
                 (annotation.y1 + annotation.y2) / 2);
}
void Refresh() {
  ui->updating = true;
  if (ui->review.isNote) {
    const auto text = UTF8ToWide(ui->candidate.note);
    const bool set = SetWindowTextW(ui->editor, text.c_str()) != FALSE;
    const int length = GetWindowTextLengthW(ui->editor);
    std::wstring shown(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(ui->editor, shown.data(), length + 1);
    shown.resize(static_cast<size_t>(length));
    ui->noteReady = set && LogicalLines(shown) == LogicalLines(text);
    if (!ui->noteReady)
      Status(L"unreadable");
  } else {
    g_annots = ui->candidate.annotations;
    g_annotsDirty = g_annotsNeedsIntegrate = false;
    TreeView_DeleteAllItems(ui->tree);
    ui->items.clear();
    SendMessageW(ui->list, LB_RESETCONTENT, 0, 0);
    auto insert = [](HTREEITEM parent, const std::wstring &title, size_t source,
                     int annotation) {
      ui->items.push_back({source, annotation});
      TVINSERTSTRUCTW value{};
      value.hParent = parent;
      value.hInsertAfter = TVI_LAST;
      value.item.mask = TVIF_TEXT | TVIF_PARAM;
      value.item.pszText = const_cast<wchar_t *>(title.c_str());
      value.item.lParam = static_cast<LPARAM>(ui->items.size());
      return TreeView_InsertItem(ui->tree, &value);
    };
    for (size_t group = 0; group < ui->review.sources.size(); ++group) {
      const auto &annotations = group ? ui->review.sources[group].annotations
                                      : ui->candidate.annotations;
      const auto title = (group ? ui->review.sources[group].title
                                : Common(L"destination_group")) +
                         L" (" + std::to_wstring(annotations.size()) + L")";
      const auto root = insert(TVI_ROOT, title, group, -1);
      for (size_t i = 0; i < annotations.size(); ++i)
        insert(root, Description(annotations[i]), group, static_cast<int>(i));
      TreeView_Expand(ui->tree, root, TVE_EXPAND);
    }
    for (const auto &annotation : ui->candidate.annotations) {
      const auto label = Description(annotation);
      SendMessageW(ui->list, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(label.c_str()));
    }
    InvalidateRect(g_hPdfView, nullptr, FALSE);
  }
  const auto heading = ui->review.isNote
                           ? Common(L"final_note")
                           : Common(L"final_preview") + L" — " +
                                 ui->candidate.pdf.filename().wstring();
  SetWindowTextW(ui->previewTitle, heading.c_str());
  ui->updating = false;
  Layout();
}
void CancelDrag() {
  ui->drag.reset();
  ui->hover = false;
  TreeView_SelectDropTarget(ui->tree, nullptr);
  if (GetCapture() == ui->view)
    ReleaseCapture();
  InvalidateRect(g_hPdfView, nullptr, FALSE);
}
bool DropTarget(POINT screen) {
  HWND target = WindowFromPoint(screen);
  TreeView_SelectDropTarget(ui->tree, nullptr);
  if (target == ui->tree) {
    TVHITTESTINFO hit{};
    hit.pt = screen;
    ScreenToClient(ui->tree, &hit.pt);
    const auto handle = TreeView_HitTest(ui->tree, &hit);
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = handle;
    if (handle && TreeView_GetItem(ui->tree, &item) && item.lParam > 0 &&
        static_cast<size_t>(item.lParam) <= ui->items.size() &&
        ui->items[static_cast<size_t>(item.lParam) - 1].source == 0) {
      TreeView_SelectDropTarget(ui->tree, handle);
      return true;
    }
  }
  return target == g_hPdfView || IsChild(g_hPdfView, target) ||
         target == ui->list;
}
void Drop(Item item) {
  if (item.source == 0 || item.source >= ui->review.sources.size() ||
      ui->candidate.saved)
    return;
  auto source = ui->review.sources[item.source].annotations;
  if (item.annotation >= 0) {
    if (static_cast<size_t>(item.annotation) >= source.size())
      return;
    source = {source[static_cast<size_t>(item.annotation)]};
  }
  for (const auto &annotation : source)
    if (annotation.pageIndex < 0 || annotation.pageIndex >= g_pdf.pageCount) {
      Status(L"choose_pdf_required");
      return;
    }
  auto next = ui->candidate;
  if (!AppendAnnotations(&next.annotations, source)) {
    Status(L"operation_failed");
    return;
  }
  if (!file_output::ProtectStageRecovery(ui->review, next)) {
    Status(L"protect_failed");
    return;
  }
  const int first = static_cast<int>(ui->candidate.annotations.size());
  ui->undo.push_back(ui->candidate);
  ui->redo.clear();
  ui->candidate = std::move(next);
  ui->review.candidate = ui->candidate;
  Refresh();
  SendMessageW(ui->list, LB_SETCURSEL, first, 0);
  SelectFinal(first);
  Status(L"added");
}
void History(bool undo) {
  if (ui->review.isNote) {
    SendMessageW(ui->editor, undo ? EM_UNDO : EM_REDO, 0, 0);
    return;
  }
  if (ui->candidate.saved)
    return;
  auto &from = undo ? ui->undo : ui->redo;
  auto &to = undo ? ui->redo : ui->undo;
  if (from.empty())
    return;
  if (!file_output::ProtectStageRecovery(ui->review, from.back())) {
    Status(L"protect_failed");
    return;
  }
  to.push_back(ui->candidate);
  ui->candidate = std::move(from.back());
  from.pop_back();
  ui->review.candidate = ui->candidate;
  Refresh();
}
void Finish(ReviewExit result) {
  if (!Protect())
    return;
  ui->result = result;
  ui->done = true;
  DestroyWindow(ui->manager);
  DestroyWindow(ui->view);
}
bool SaveCandidate(const std::filesystem::path &destination = {}) {
  if (!ui->noteReady) {
    Status(L"unreadable");
    return false;
  }
  if (!Protect())
    return false;
  if (!file_output::SaveStageRecovery(ui->review, ui->candidate, destination)) {
    Layout();
    Status(L"save_failed");
    return false;
  }
  Finish(ReviewExit::Saved);
  return true;
}
void SaveAsNew();
void RequestClose() {
  if (ui->closing)
    return;
  ui->closing = true;
  SilentDialogOptions options;
  options.title = T(L"title");
  options.message = T(L"close_question");
  options.buttons = SilentDialogButtons::YesNoCancel;
  options.yesLabel = T(L"save_close");
  options.noLabel = T(L"defer_close");
  options.cancelLabel = T(L"continue");
  options.defaultResult = SilentDialogResult::Cancel;
  options.escapeResult = SilentDialogResult::Cancel;
  const auto result = ShowSilentDialog(ui->manager, options);
  ui->closing = false;
  if (result == SilentDialogResult::Yes) {
    if (ui->candidate.targetConfirmed && ui->review.recoverable)
      (void)SaveCandidate();
    else if (ui->review.recoverable &&
             (ui->review.isNote || !ui->candidate.pdf.empty()))
      SaveAsNew();
    else
      Status(ui->review.recoverable ? L"choose_pdf_required" : L"unreadable");
  } else if (result == SilentDialogResult::No)
    Finish(ReviewExit::Deferred);
}
void ChooseTarget() {
  wchar_t file[32768]{};
  const auto title = T(L"choose_pdf");
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = ui->manager;
  dialog.lpstrFile = file;
  dialog.nMaxFile = 32768;
  dialog.lpstrFilter = L"PDF (*.pdf)\0*.pdf\0\0";
  dialog.lpstrTitle = title.c_str();
  dialog.Flags =
      OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&dialog))
    return;
  std::unique_ptr<DocumentOpenLockCandidate> lock;
  if (ui->review.originalTarget.empty() ||
      document_source_protection::Key(ui->review.originalTarget) !=
          document_source_protection::Key(file)) {
    std::wstring error;
    lock = std::make_unique<DocumentOpenLockCandidate>(file, &error);
    if (!lock->acquired()) {
      Status(L"operation_failed");
      return;
    }
  }
  auto next = ui->candidate;
  if (!file_output::SelectStageRecoveryPdf(ui->review, next, file)) {
    Status(L"operation_failed");
    return;
  }
  if (!LoadPdfDocument(g_hPdfView, next.pdf.wstring())) {
    ui->candidate.targetConfirmed = false;
    (void)Protect();
    Refresh();
    Status(L"choose_pdf_required");
    return;
  }
  ui->selectedPdfLock = std::move(lock);
  ui->candidate = std::move(next);
  ui->review.candidate = ui->candidate;
  ui->undo.clear();
  ui->redo.clear();
  Refresh();
  Status(L"pdf_selected");
}
void SaveAsNew() {
  wchar_t file[32768]{};
  wcscpy_s(file, ui->review.isNote ? L"recovered-note.md"
                                   : L"recovered-document.pdf");
  const auto title = T(L"save_as_close");
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = ui->manager;
  dialog.lpstrFile = file;
  dialog.nMaxFile = 32768;
  dialog.lpstrTitle = title.c_str();
  dialog.lpstrFilter = ui->review.isNote ? L"Markdown (*.md)\0*.md\0\0"
                                         : L"PDF (*.pdf)\0*.pdf\0\0";
  dialog.lpstrDefExt = ui->review.isNote ? L"md" : L"pdf";
  dialog.Flags =
      OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog))
    (void)SaveCandidate(file);
}
LRESULT CALLBACK PdfGate(HWND window, UINT message, WPARAM wp, LPARAM lp,
                         UINT_PTR, DWORD_PTR) {
  switch (message) {
  case WM_LBUTTONDOWN:
    // The editor's native click path can follow link annotations before it
    // checks Pan mode. Own this gesture so preview input cannot open/edit a
    // different document. Native mouse-move handles navigation only.
    SetFocus(window);
    if (g_pdf.pageCount > 0) {
      SetCapture(window);
      g_pdf.panning = true;
      g_pdf.panAnchor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      g_pdf.panStartX = g_pdf.scrollX;
      g_pdf.panStartY = g_pdf.scrollY;
      g_pdf.panAxisLock = decltype(g_pdf.panAxisLock)::None;
    }
    return 0;
  case WM_LBUTTONUP:
    g_pdf.panning = false;
    g_pdf.panAxisLock = decltype(g_pdf.panAxisLock)::None;
    if (GetCapture() == window)
      ReleaseCapture();
    return 0;
  case WM_MOUSEMOVE:
    if (!g_pdf.panning)
      return 0;
    break;
  case WM_TOUCH:
    CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lp));
    return 0;
  case WM_POINTERDOWN:
  case WM_POINTERUPDATE:
  case WM_POINTERUP:
  case WM_GESTURE:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
  case WM_APPCOMMAND:
  case WM_HOTKEY:
    return 0;
  case WM_CHAR:
  case WM_IME_CHAR:
  case WM_IME_COMPOSITION:
  case WM_CONTEXTMENU:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_LBUTTONDBLCLK:
  case WM_DROPFILES:
  case WM_COMMAND:
  case WM_PASTE:
  case WM_CUT:
  case WM_CLEAR:
    return 0;
  case WM_KEYDOWN:
    if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
         GetKeyState(VK_SHIFT)) &
        0x8000)
      return 0;
    if (wp != VK_LEFT && wp != VK_RIGHT && wp != VK_UP && wp != VK_DOWN &&
        wp != VK_PRIOR && wp != VK_NEXT && wp != VK_HOME && wp != VK_END)
      return 0;
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (!ui)
    return DefWindowProcW(window, message, wp, lp);
  try {
    switch (message) {
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_MINIMIZE)
        return 0;
      break;
    case WM_SIZE:
      if (ui->editor && ui->manager)
        Layout();
      return 0;
    case WM_CLOSE:
      RequestClose();
      return 0;
    case WM_QUERYENDSESSION:
      return Protect() ? TRUE : FALSE;
    case WM_ENDSESSION:
      if (wp) {
        if (Protect()) {
          ui->result = ReviewExit::Deferred;
          ui->done = true;
        }
      }
      return 0;
    case WM_NOTIFY: {
      const auto *header = reinterpret_cast<NMHDR *>(lp);
      if (!ui->updating && header->hwndFrom == ui->tree &&
          header->code == TVN_BEGINDRAGW && !ui->candidate.saved) {
        const auto *notification = reinterpret_cast<NMTREEVIEWW *>(lp);
        const auto index = notification->itemNew.lParam;
        if (index > 0 && static_cast<size_t>(index) <= ui->items.size() &&
            ui->items[static_cast<size_t>(index) - 1].source > 0) {
          ui->drag = ui->items[static_cast<size_t>(index) - 1];
          SetCapture(ui->view);
        }
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (ui->drag) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(window, &point);
        SetCursor(LoadCursorW(nullptr, DropTarget(point) ? IDC_CROSS : IDC_NO));
      }
      return 0;
    case WM_LBUTTONUP:
      if (ui->drag) {
        const auto item = *ui->drag;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(window, &point);
        const bool accepted = DropTarget(point);
        CancelDrag();
        if (accepted)
          Drop(item);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (ui->drag)
        CancelDrag();
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == FinalList && HIWORD(wp) == LBN_SELCHANGE)
        SelectFinal(
            static_cast<int>(SendMessageW(ui->list, LB_GETCURSEL, 0, 0)));
      else if (LOWORD(wp) == ReferencePicker && HIWORD(wp) == CBN_SELCHANGE) {
        const auto i = SendMessageW(ui->picker, CB_GETCURSEL, 0, 0);
        if (i >= 0 && static_cast<size_t>(i) < ui->review.sources.size())
          SetWindowTextW(
              ui->reference,
              UTF8ToWide(ui->review.sources[static_cast<size_t>(i)].note)
                  .c_str());
      } else if (LOWORD(wp) == NoteEditor && HIWORD(wp) == EN_CHANGE &&
                 !ui->updating && ui->review.isNote && !ui->candidate.saved) {
        (void)Protect();
      } else if (LOWORD(wp) == Save)
        (void)SaveCandidate();
      else if (LOWORD(wp) == SaveAs)
        SaveAsNew();
      else if (LOWORD(wp) == Defer)
        Finish(ReviewExit::Deferred);
      else if (LOWORD(wp) == ChoosePdf)
        ChooseTarget();
      else if (LOWORD(wp) == Undo || LOWORD(wp) == Redo)
        History(LOWORD(wp) == Undo);
      else if (LOWORD(wp) == Partial) {
        ui->candidate.partialAccepted =
            SendMessageW(GetDlgItem(window, Partial), BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        (void)Protect();
      }
      return 0;
    default:
      break;
    }
  } catch (...) {
    Status(L"operation_failed");
    return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}
} // namespace

ReviewExit RunStageRecoveryWindow(Review &review, HWND parent) {
  WindowState state{review, review.candidate};
  ui = &state;
  struct Reset {
    ~Reset() {
      ClearPdfState();
      g_annots.clear();
      ui = nullptr;
    }
  } reset;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  HMODULE rich =
      LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!rich)
    return ReviewExit::Failed;
  struct Unload {
    HMODULE module;
    ~Unload() { FreeLibrary(module); }
  } unload{rich};
  g_hInst = GetModuleHandleW(nullptr);
  g_toolMode = ToolMode::Pan;
  g_showAnnots = true;
  g_hUIFont = CreateUIFont();
  WNDCLASSW wc{};
  wc.hInstance = g_hInst;
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = WindowProc;
  wc.lpszClassName = viewClass;
  RegisterClassW(&wc);
  wc.lpszClassName = managerClass;
  RegisterClassW(&wc);
  wc.lpszClassName = kPdfViewClass;
  wc.lpfnWndProc = PdfViewProc;
  RegisterClassW(&wc);
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
    work = {0, 0, 1280, 800};
  const int width =
                std::max(950, static_cast<int>(work.right - work.left) - 380),
            height =
                std::max(600, static_cast<int>(work.bottom - work.top) - 60);
  state.view = CreateWindowExW(
      0, viewClass, T(L"title").c_str(),
      (WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX) | WS_CLIPCHILDREN, work.left + 12,
      work.top + 12, width, height, parent, nullptr, g_hInst, nullptr);
  if (!state.view)
    return ReviewExit::Failed;
  g_hMainWnd = state.view;
  (void)::ui::ApplyAuxiliaryWindowCorners(state.view);
  state.sourceTitle = Control(
      state.view, L"STATIC",
      Common(review.isNote ? L"source_heading" : L"annotation_groups"), 0, 0);
  state.tree = Control(state.view, WC_TREEVIEWW, L"",
                       WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT |
                           TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                       Sources);
  state.picker =
      Control(state.view, L"COMBOBOX", L"",
              WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ReferencePicker);
  state.reference = Control(state.view, MSFTEDIT_CLASS, L"",
                            WS_TABSTOP | WS_BORDER | ES_MULTILINE |
                                ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                            NoteReference);
  state.previewTitle = Control(state.view, L"STATIC", L"", 0, 0);
  g_hPdfView = Control(state.view, kPdfViewClass, L"",
                       WS_TABSTOP | WS_VSCROLL | WS_HSCROLL, 0);
  state.finalTitle =
      Control(state.view, L"STATIC", Common(L"final_annotations"), 0, 0);
  state.list =
      Control(state.view, L"LISTBOX", L"",
              WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, FinalList);
  state.editor = Control(state.view, MSFTEDIT_CLASS, L"",
                         WS_TABSTOP | WS_BORDER | ES_MULTILINE |
                             ES_AUTOVSCROLL | WS_VSCROLL,
                         NoteEditor);
  if (!g_hPdfView || !state.editor || !state.reference || !state.tree ||
      !state.list || !state.picker ||
      !SetWindowSubclass(g_hPdfView, PdfGate, 1, 0))
    return ReviewExit::Failed;
  if (SendMessageW(state.editor, EM_SETTEXTMODE,
                   TM_PLAINTEXT | TM_MULTILEVELUNDO, 0) != 0 ||
      SendMessageW(state.reference, EM_SETTEXTMODE,
                   TM_PLAINTEXT | TM_MULTILEVELUNDO, 0) != 0)
    return ReviewExit::Failed;
  SendMessageW(state.editor, EM_SETEVENTMASK, 0, ENM_CHANGE);
  SendMessageW(state.editor, EM_EXLIMITTEXT, 0, 128 * 1024 * 1024);
  SendMessageW(state.reference, EM_EXLIMITTEXT, 0, 128 * 1024 * 1024);
  state.manager = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_CONTROLPARENT, managerClass, T(L"title").c_str(),
      WS_CAPTION | WS_SYSMENU | WS_POPUP, work.right - 360, work.top + 24, 350,
      440, state.view, nullptr, g_hInst, nullptr);
  if (!state.manager)
    return ReviewExit::Failed;
  (void)::ui::ApplyAuxiliaryWindowCorners(state.manager);
  const auto filename = review.originalTarget.filename().wstring();
  HWND name = Control(state.manager, L"STATIC", filename, 0, 0);
  MoveWindow(name, 12, 12, 310, 32, TRUE);
  state.status = Control(state.manager, L"STATIC",
                         T(review.recoverable
                               ? (review.isNote ? L"note_hint" : L"pdf_hint")
                               : L"unreadable"),
                         0, 0);
  MoveWindow(state.status, 12, 48, 310, 100, TRUE);
  const std::vector<std::pair<int, const wchar_t *>> buttons = {
      {Save, L"save_close"},   {SaveAs, L"save_as_close"},
      {Defer, L"defer_close"}, {ChoosePdf, L"choose_pdf"},
      {Undo, L"undo"},         {Redo, L"redo"}};
  for (size_t i = 0; i < buttons.size(); ++i) {
    HWND button = Control(state.manager, L"BUTTON", T(buttons[i].second),
                          WS_TABSTOP | BS_PUSHBUTTON, buttons[i].first);
    MoveWindow(button, 12, 156 + static_cast<int>(i) * 34, 310, 30, TRUE);
  }
  HWND partial = Control(state.manager, L"BUTTON", T(L"partial"),
                         WS_TABSTOP | BS_AUTOCHECKBOX, Partial);
  MoveWindow(partial, 12, 364, 310, 32, TRUE);
  SendMessageW(partial, BM_SETCHECK,
               state.candidate.partialAccepted ? BST_CHECKED : BST_UNCHECKED,
               0);
  for (const auto &source : review.sources)
    SendMessageW(state.picker, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(source.title.c_str()));
  SendMessageW(state.picker, CB_SETCURSEL, review.sources.size() > 1 ? 1 : 0,
               0);
  if (!review.sources.empty())
    SetWindowTextW(
        state.reference,
        UTF8ToWide(review.sources[review.sources.size() > 1 ? 1 : 0].note)
            .c_str());
  Refresh();
  ShowWindow(state.view, SW_SHOW);
  ShowWindow(state.manager, SW_SHOW);
  SetForegroundWindow(state.manager);
  if (!review.isNote && !state.candidate.pdf.empty() &&
      !LoadPdfDocument(g_hPdfView, state.candidate.pdf.wstring())) {
    state.candidate.targetConfirmed = false;
    (void)Protect();
    Status(L"choose_pdf_required");
  }
  Refresh();
  MSG message{};
  while (!state.done) {
    const int result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      if (result == 0)
        PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (state.drag && message.message == WM_KEYDOWN &&
        message.wParam == VK_ESCAPE) {
      CancelDrag();
      continue;
    }
    if (::ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(message))
      continue;
    if (message.message == WM_KEYDOWN && message.wParam == VK_F6) {
      if (state.drag)
        CancelDrag();
      const bool manager = GetAncestor(GetFocus(), GA_ROOT) == state.manager;
      SetForegroundWindow(manager ? state.view : state.manager);
      SetFocus(manager ? (review.isNote ? state.editor : state.tree)
                       : GetDlgItem(state.manager, Defer));
      continue;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
        GetAncestor(GetFocus(), GA_ROOT) == state.view &&
        (GetFocus() != state.editor || (GetKeyState(VK_CONTROL) & 0x8000))) {
      if (state.drag)
        CancelDrag();
      const HWND next = GetNextDlgTabItem(
          state.view, GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
      if (next)
        SetFocus(next);
      continue;
    }
    if (!IsDialogMessageW(state.manager, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(state.manager))
    DestroyWindow(state.manager);
  if (IsWindow(state.view))
    DestroyWindow(state.view);
  if (g_hUIFont) {
    DeleteObject(g_hUIFont);
    g_hUIFont = nullptr;
  }
  return state.result;
}
} // namespace recovery
