// Standalone recovery tool: `pdf_note_workspace.exe --recovery [workspace]
// [--stage <stage>] [--wait-pid <pid>]`.
//
// It runs before normal startup and never reads the app setup, so it stays
// usable when the main app cannot start. It lists every checkpoint (stage) of
// one workspace, shows whether each original still exists, and opens the
// existing review window for the chosen checkpoint. While it runs it owns the
// originals itself, so the main app must not run on the same package: the
// tool waits for or asks the user to exit the main app, and holds a named
// mutex that the main app checks at startup.
#include "app/startup_instance.h"
#include "core/workspace_layout.h"
#include "core/app_core.h"
#include "core/document_source_protection.h"
#include "core/localization.h"
#include "core/private_fonts.h"
#include "core/ui_notify.h"
#include "diagnostics/write_checks.h"
#include "file_output/file_output.h"
#include "note/note_identity_store.h"
#include "pdf_view/pdf_view.h"
#include "recovery/recovery_visual.h"
#include "recovery/stage_recovery.h"
#include "ui/window_corners.h"
#include "workspace/workspace_write_lock.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace recovery {
namespace {

std::wstring S(const wchar_t *id) {
  return localization::Text(std::wstring(L"recovery.tool.") + id);
}

struct OwnedHandle {
  HANDLE value = nullptr;
  OwnedHandle() = default;
  explicit OwnedHandle(HANDLE h) : value(h) {}
  OwnedHandle(const OwnedHandle &) = delete;
  OwnedHandle &operator=(const OwnedHandle &) = delete;
  ~OwnedHandle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};

std::wstring ExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD n =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size())
    return {};
  path.resize(n);
  return path;
}

std::wstring QuoteArg(const std::wstring &value) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    if (ch == L'"')
      out.append(slashes * 2 + 1, L'\\');
    else
      out.append(slashes, L'\\');
    slashes = 0;
    out += ch;
  }
  out.append(slashes * 2, L'\\');
  out += L'"';
  return out;
}

bool MainAppRunning() {
  OwnedHandle mutex(OpenMutexW(SYNCHRONIZE, FALSE, SingleInstanceMutexName().c_str()));
  return mutex.value != nullptr;
}

bool IsWorkspaceRoot(const std::filesystem::path &root) {
  if (root.empty() || !root.is_absolute() || !write_checks::IsSafeLocalPath(root))
    return false;
  const DWORD attributes = GetFileAttributesW(
      ToExtendedWin32PathIfAbsoluteLocal(workspace_layout::Root(root)).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::filesystem::path StageFolder(const std::filesystem::path &root) {
  return workspace_layout::Unsaved(root);
}

// Explicit, user-driven folder choice (common dialog) when no workspace was
// given. Nothing is read from the app setup.
std::filesystem::path PickWorkspace() {
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return {};
  std::filesystem::path result;
  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options)))
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
  const std::wstring title = S(L"pick_workspace");
  dialog->SetTitle(title.c_str());
  if (SUCCEEDED(dialog->Show(nullptr))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

struct Row {
  file_output::StagedDiffEntry entry;
  bool originalExists = false;
  // What the work needs, step by step; the first non-OK step is the problem.
  std::vector<recovery_visual::Node> nodes;
  std::wstring problem;
  bool needsAction = false;
};

struct ToolState {
  HWND window = nullptr, list = nullptr, info = nullptr, status = nullptr;
  HWND recover = nullptr, folder = nullptr, manual = nullptr, close = nullptr;
  HWND flow = nullptr;
  std::filesystem::path workspace, preferredStage;
  std::vector<Row> rows;
  std::wstring summary;
  HFONT font = nullptr;
  bool reviewing = false;
};
ToolState *tool = nullptr;

enum : int { kList = 100, kRecover = 101, kFolder = 102, kManual = 103, kClose = 104,
             kFlow = 105 };
constexpr int kBannerHeight = 76;
constexpr int kFlowHeight = 104;

bool PathExists(const std::wstring &path) {
  return !path.empty() &&
         GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str()) !=
             INVALID_FILE_ATTRIBUTES;
}

void SetStatus(const std::wstring &text) {
  if (tool && tool->status)
    SetWindowTextW(tool->status, text.c_str());
}

std::wstring FileNameOf(const std::wstring &path) {
  return std::filesystem::path(path).filename().wstring();
}

// Reads the checkpoint the same way recovery does and states, per step, what
// is fine, what is missing, and what needs a decision.
void DescribeRow(Row &row) {
  using recovery_visual::Mark;
  using recovery_visual::Node;
  const bool note = row.entry.kind == file_output::StagedDiffKind::Note;
  const auto inspection = file_output::InspectStagedDiff(row.entry.stagePath);
  const std::wstring revision =
      row.entry.revision ? std::to_wstring(*row.entry.revision) : std::wstring(L"-");
  const auto F = [](const wchar_t *id, const std::vector<std::pair<std::wstring_view, std::wstring>> &values) {
    return localization::Format(std::wstring(L"recovery.tool.") + id, values);
  };
  Node stage{S(L"node_stage"), F(L"stage_ok", {{L"REV", revision}}), Mark::Ok};
  switch (inspection.failure) {
  case recovery::Failure::Metadata:
  case recovery::Failure::Unsupported:
  case recovery::Failure::Read:
  case recovery::Failure::Parse:
    stage = {S(L"node_stage"), S(L"stage_unreadable"), Mark::Bad};
    break;
  case recovery::Failure::MissingData:
    stage = {S(L"node_stage"), F(L"stage_partial", {{L"REV", revision}}), Mark::Warn};
    break;
  default:
    if (inspection.incomplete)
      stage = {S(L"node_stage"), F(L"stage_partial", {{L"REV", revision}}), Mark::Warn};
    break;
  }
  const std::wstring name = FileNameOf(row.entry.targetPath);
  Node original{S(note ? L"node_note" : L"node_pdf"), name, Mark::Ok};
  if (!row.originalExists)
    original = {original.title, F(L"original_missing", {{L"NAME", name}}), Mark::Bad};
  else if (inspection.failure == recovery::Failure::TargetMismatch)
    original = {original.title, F(L"original_changed", {{L"NAME", name}}), Mark::Warn};
  Node destination{S(note ? L"node_dest_note" : L"node_dest_clrop"),
                   S(L"dest_ok"), Mark::Ok};
  if (!row.originalExists)
    destination = {destination.title, S(note ? L"dest_new_file" : L"dest_needs_pdf"),
                   note ? Mark::Warn : Mark::Bad};
  else if (inspection.failure == recovery::Failure::AnnotationConflict ||
           inspection.failure == recovery::Failure::NoteConflict)
    destination = {destination.title, S(L"dest_conflict"), Mark::Warn};
  else if (inspection.failure == recovery::Failure::DestinationChanged)
    destination = {destination.title, S(L"dest_changed"), Mark::Warn};
  else if (inspection.failure == recovery::Failure::Backup ||
           inspection.failure == recovery::Failure::Write ||
           inspection.failure == recovery::Failure::Commit)
    destination = {destination.title, S(L"dest_write_failed"), Mark::Warn};
  row.nodes = {stage, original, destination};
  row.problem = S(L"problem_none");
  row.needsAction = false;
  for (const Mark wanted : {Mark::Bad, Mark::Warn}) {
    for (const auto &node : row.nodes) {
      if (node.mark != wanted)
        continue;
      row.problem = node.title + L"：" + node.detail;
      row.needsAction = true;
      break;
    }
    if (row.needsAction)
      break;
  }
}

void FillList() {
  ListView_DeleteAllItems(tool->list);
  tool->rows.clear();
  for (const auto &entry : file_output::ListStagedDiffEntries()) {
    if (!entry.isLatest)
      continue;
    Row row{entry, PathExists(entry.targetPath)};
    DescribeRow(row);
    tool->rows.push_back(std::move(row));
  }
  size_t needs = 0;
  for (const auto &row : tool->rows)
    needs += row.needsAction ? 1 : 0;
  tool->summary = tool->rows.empty()
      ? S(L"status_empty")
      : localization::Format(L"recovery.tool.summary",
                             {{L"NEED", std::to_wstring(needs)},
                              {L"READY", std::to_wstring(tool->rows.size() - needs)}});
  int preferred = -1;
  for (size_t i = 0; i < tool->rows.size(); ++i) {
    const auto &row = tool->rows[i];
    const std::wstring kind = S(row.entry.kind == file_output::StagedDiffKind::Note
                                    ? L"kind_note"
                                    : L"kind_annotation");
    const std::wstring state = (row.needsAction ? std::wstring(L"\u26A0 ") : std::wstring(L"\u2713 ")) + row.problem;
    const std::wstring revision =
        row.entry.revision ? std::to_wstring(*row.entry.revision) : std::wstring(L"-");
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(i);
    item.pszText = const_cast<wchar_t *>(kind.c_str());
    const int index = ListView_InsertItem(tool->list, &item);
    ListView_SetItemText(tool->list, index, 1, const_cast<wchar_t *>(row.entry.targetPath.c_str()));
    ListView_SetItemText(tool->list, index, 2, const_cast<wchar_t *>(state.c_str()));
    ListView_SetItemText(tool->list, index, 3, const_cast<wchar_t *>(revision.c_str()));
    if (!tool->preferredStage.empty() &&
        document_source_protection::Key(row.entry.stagePath) ==
            document_source_protection::Key(tool->preferredStage))
      preferred = index;
  }
  if (!tool->rows.empty()) {
    const int select = preferred >= 0 ? preferred : 0;
    ListView_SetItemState(tool->list, select, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(tool->list, select, FALSE);
  }
  EnableWindow(tool->recover, !tool->rows.empty());
  SetStatus(tool->summary);
  InvalidateRect(tool->window, nullptr, FALSE);
  if (tool->flow)
    InvalidateRect(tool->flow, nullptr, TRUE);
}

int SelectedRow() {
  const int index = ListView_GetNextItem(tool->list, -1, LVNI_SELECTED);
  return index >= 0 && static_cast<size_t>(index) < tool->rows.size() ? index : -1;
}

// Same steps as the in-app recovery child: prepare under the workspace lock,
// finish an already-saved candidate, otherwise run the review window.
void RecoverSelected() {
  const int index = SelectedRow();
  if (index < 0 || tool->reviewing)
    return;
  const auto stage = tool->rows[static_cast<size_t>(index)].entry.stagePath;
  std::wstring error;
  WorkspaceOperationLock lock(tool->workspace, &error);
  Review review;
  if (!lock.acquired() || !file_output::PrepareStageRecovery(stage, &review)) {
    SetStatus(S(L"open_failed"));
    return;
  }
  OwnedHandle session(CreateFileW(
      ToExtendedWin32PathIfAbsoluteLocal(review.directory / L"active.lock").c_str(),
      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (session.value == INVALID_HANDLE_VALUE) {
    SetStatus(S(L"open_failed"));
    return;
  }
  ReviewExit result = ReviewExit::Failed;
  if (review.candidate.saved && file_output::CompleteStageRecovery(review)) {
    result = ReviewExit::Saved;
  } else {
    tool->reviewing = true;
    EnableWindow(tool->window, FALSE);
    result = RunStageRecoveryWindow(review, tool->window);
    EnableWindow(tool->window, TRUE);
    tool->reviewing = false;
    g_hMainWnd = tool->window;
    SetForegroundWindow(tool->window);
  }
  tool->preferredStage.clear();
  FillList();
  SetStatus(S(result == ReviewExit::Saved      ? L"status_saved"
              : result == ReviewExit::Deferred ? L"status_deferred"
                                               : L"status_interrupted"));
}

void OpenStageFolder() {
  const auto folder = StageFolder(tool->workspace);
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(tool->window, L"open", folder.c_str(),
                                              nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    SetStatus(S(L"folder_failed"));
}

LRESULT CALLBACK ManualProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_SIZE) {
    if (HWND edit = GetDlgItem(hwnd, 1))
      MoveWindow(edit, 8, 8, LOWORD(lParam) - 16, HIWORD(lParam) - 16, TRUE);
    return 0;
  }
  if (msg == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The same text the app leaves as README_RECOVERY.txt in the stage folder.
void ShowManual() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = ManualProc;
  wc.hInstance = g_hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"PdfNoteRecoveryManual";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, S(L"manual_title").c_str(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                760, 620, tool->window, nullptr, g_hInst, nullptr);
  if (!window)
    return;
  std::wstring text;
  for (const wchar_t ch : localization::Text(L"recovery.manual_readme")) {
    if (ch == L'\n')
      text += L'\r';
    text += ch;
  }
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                  ES_READONLY | ES_AUTOVSCROLL,
                              8, 8, 700, 560, window, reinterpret_cast<HMENU>(1),
                              g_hInst, nullptr);
  SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(tool->font), TRUE);
  (void)::ui::ApplyAuxiliaryWindowCorners(window);
  ShowWindow(window, SW_SHOWNORMAL);
  RECT client{};
  GetClientRect(window, &client);
  SendMessageW(window, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
}

void Layout() {
  RECT rc{};
  GetClientRect(tool->window, &rc);
  const int w = rc.right, h = rc.bottom, pad = 10, button = 30, infoH = 52, statusH = 24;
  MoveWindow(tool->info, pad, kBannerHeight + pad, w - pad * 2, infoH, TRUE);
  const int listTop = kBannerHeight + pad + infoH + 6;
  const int listH = h - listTop - kFlowHeight - button - statusH - pad * 4;
  MoveWindow(tool->list, pad, listTop, w - pad * 2, listH > 80 ? listH : 80, TRUE);
  const int flowTop = listTop + (listH > 80 ? listH : 80) + pad;
  MoveWindow(tool->flow, pad, flowTop, w - pad * 2, kFlowHeight, TRUE);
  const int buttonsTop = flowTop + kFlowHeight + pad;
  const int bw = 180;
  MoveWindow(tool->recover, pad, buttonsTop, bw, button, TRUE);
  MoveWindow(tool->folder, pad + (bw + 8), buttonsTop, bw, button, TRUE);
  MoveWindow(tool->manual, pad + (bw + 8) * 2, buttonsTop, bw, button, TRUE);
  MoveWindow(tool->close, w - pad - 110, buttonsTop, 110, button, TRUE);
  MoveWindow(tool->status, pad, buttonsTop + button + 6, w - pad * 2, statusH, TRUE);
  const int problemW = (w - pad * 2) * 2 / 5;
  ListView_SetColumnWidth(tool->list, 2, problemW);
  const int nameW = w - pad * 2 - 110 - problemW - 60 - 24;
  ListView_SetColumnWidth(tool->list, 1, nameW > 160 ? nameW : 160);
}

std::wstring WatermarkText() { return S(L"watermark"); }

void PaintBannerAndBackground(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT band{0, 0, rc.right, kBannerHeight};
  recovery_visual::PaintBanner(dc, band, tool ? tool->font : nullptr, GetSysColor(COLOR_WINDOW),
                               S(L"banner_title"),
                               S(L"banner_warning"), tool ? tool->summary : std::wstring());
  RECT rest{0, kBannerHeight, rc.right, rc.bottom};
  const COLORREF face = GetSysColor(COLOR_WINDOW);
  HBRUSH brush = CreateSolidBrush(face);
  FillRect(dc, &rest, brush);
  DeleteObject(brush);
  recovery_visual::PaintWatermark(dc, rest, tool ? tool->font : nullptr, face, WatermarkText());
  EndPaint(hwnd, &ps);
}

void DrawFlow(const DRAWITEMSTRUCT *item) {
  const int index = SelectedRow();
  std::vector<recovery_visual::Node> nodes;
  if (index >= 0)
    nodes = tool->rows[static_cast<size_t>(index)].nodes;
  recovery_visual::PaintFlow(item->hDC, item->rcItem, tool->font, GetSysColor(COLOR_WINDOW), nodes);
  if (nodes.empty()) {
    RECT text = item->rcItem;
    SetBkMode(item->hDC, TRANSPARENT);
    SelectObject(item->hDC, tool->font);
    DrawTextW(item->hDC, S(L"flow_empty").c_str(), -1, &text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
}

// The list shows the same repeated warning behind its rows. Its own
// background image support is not available here, so erase it ourselves and
// repaint fully after scrolling so the pattern never tears.
LRESULT CALLBACK ListBackgroundProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR, DWORD_PTR) {
  if (msg == WM_ERASEBKGND && tool) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const COLORREF back = GetSysColor(COLOR_WINDOW);
    HDC dc = reinterpret_cast<HDC>(wParam);
    HBRUSH brush = CreateSolidBrush(back);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    recovery_visual::PaintWatermark(dc, rc, tool->font, back, WatermarkText());
    return 1;
  }
  const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
  if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN)
    InvalidateRect(hwnd, nullptr, TRUE);
  return result;
}

void ApplyListWatermark() {
  if (!SetWindowSubclass(tool->list, ListBackgroundProc, 1, 0))
    return;
  ListView_SetBkColor(tool->list, CLR_NONE);
  ListView_SetTextBkColor(tool->list, CLR_NONE);
}

LRESULT CALLBACK ToolProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_SIZE:
    if (tool && tool->list) {
      Layout();
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    PaintBannerAndBackground(hwnd);
    return 0;
  case WM_CTLCOLORSTATIC:
    if (tool && reinterpret_cast<HWND>(lParam) != tool->flow) {
      // Static texts sit on the watermark: draw them on a plain face so they stay legible.
      SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    break;
  case WM_DRAWITEM:
    if (wParam == kFlow) {
      DrawFlow(reinterpret_cast<const DRAWITEMSTRUCT *>(lParam));
      return TRUE;
    }
    break;
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case kRecover: RecoverSelected(); return 0;
    case kFolder: OpenStageFolder(); return 0;
    case kManual: ShowManual(); return 0;
    case kClose: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
    default: break;
    }
    break;
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lParam);
    if (header->idFrom == kList && header->code == NM_DBLCLK) {
      RecoverSelected();
      return 0;
    }
    if (header->idFrom == kList && header->code == LVN_ITEMCHANGED && tool && tool->flow)
      InvalidateRect(tool->flow, nullptr, TRUE);
    break;
  }
  case WM_CLOSE:
    if (tool && tool->reviewing)
      return 0;
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND Child(HWND parent, const wchar_t *cls, const std::wstring &text, DWORD style, int id) {
  HWND child = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                               parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               g_hInst, nullptr);
  SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(tool->font), TRUE);
  return child;
}

int RunToolWindow(ToolState &state) {
  tool = &state;
  struct Reset { ~Reset() { tool = nullptr; } } reset;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  state.font = CreateUIFont();
  WNDCLASSW wc{};
  wc.lpfnWndProc = ToolProc;
  wc.hInstance = g_hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"PdfNoteStandaloneRecovery";
  RegisterClassW(&wc);
  const std::wstring title = S(L"title") + L" - " + state.workspace.filename().wstring();
  state.window = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720, nullptr, nullptr,
                                 g_hInst, nullptr);
  if (!state.window)
    return 1;
  g_hMainWnd = state.window;
  (void)::ui::ApplyAuxiliaryWindowCorners(state.window);
  state.info = Child(state.window, L"STATIC",
                     localization::Format(L"recovery.tool.info",
                                          {{L"WORKSPACE", state.workspace.wstring()}}),
                     SS_LEFT | SS_NOPREFIX, 0);
  state.list = Child(state.window, WC_LISTVIEWW, L"",
                     WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                     kList);
  ListView_SetExtendedListViewStyle(state.list, LVS_EX_FULLROWSELECT);
  const std::wstring headers[] = {S(L"col_kind"), S(L"col_original"), S(L"col_problem"),
                                  S(L"col_revision")};
  const int widths[] = {110, 360, 380, 60};
  for (int i = 0; i < 4; ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t *>(headers[i].c_str());
    column.cx = widths[i];
    ListView_InsertColumn(state.list, i, &column);
  }
  state.recover = Child(state.window, L"BUTTON", S(L"recover"), WS_TABSTOP | BS_DEFPUSHBUTTON, kRecover);
  state.folder = Child(state.window, L"BUTTON", S(L"open_folder"), WS_TABSTOP, kFolder);
  state.manual = Child(state.window, L"BUTTON", S(L"manual"), WS_TABSTOP, kManual);
  state.close = Child(state.window, L"BUTTON", S(L"close"), WS_TABSTOP, kClose);
  state.status = Child(state.window, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);
  state.flow = Child(state.window, L"STATIC", L"", SS_OWNERDRAW, kFlow);
  ApplyListWatermark();
  FillList();
  ShowWindow(state.window, SW_SHOWNORMAL);
  Layout();
  SetForegroundWindow(state.window);
  SetFocus(state.list);
  if (!state.preferredStage.empty() && SelectedRow() >= 0)
    PostMessageW(state.window, WM_COMMAND, MAKEWPARAM(kRecover, 0), 0);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(state.window, &message))
      continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (state.font) {
    DeleteObject(state.font);
    state.font = nullptr;
  }
  return 0;
}

// Asks the user to exit the main app until it is gone or they cancel.
bool WaitForMainAppToExit() {
  while (MainAppRunning()) {
    SilentDialogOptions dialog;
    dialog.title = S(L"title");
    dialog.message = S(L"main_running");
    dialog.kind = SoftNoticeKind::Warning;
    dialog.buttons = SilentDialogButtons::YesNo;
    dialog.yesLabel = S(L"retry");
    dialog.noLabel = S(L"close");
    dialog.defaultResult = SilentDialogResult::Yes;
    dialog.escapeResult = SilentDialogResult::No;
    if (ShowSilentDialog(nullptr, dialog) != SilentDialogResult::Yes)
      return false;
  }
  return true;
}

} // namespace

std::wstring StandaloneRecoveryMutexName() {
  return SingleInstanceMutexName() + L"_StandaloneRecovery";
}

bool LaunchStandaloneRecoveryAfterExit(const std::filesystem::path &workspace,
                                       const std::filesystem::path &stage) {
  const std::wstring executable = ExecutablePath();
  if (executable.empty() || workspace.empty())
    return false;
  std::wstring command = QuoteArg(executable) + L" --recovery " + QuoteArg(workspace.wstring());
  if (!stage.empty())
    command += L" --stage " + QuoteArg(stage.wstring());
  command += L" --wait-pid " + std::to_wstring(GetCurrentProcessId());
  const std::wstring directory = std::filesystem::path(executable).parent_path().wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr, directory.c_str(), &startup,
                      &process))
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool TryRunStandaloneRecovery(int *exitCode) {
  int count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments)
    return false;
  struct Free {
    LPWSTR *p;
    ~Free() { LocalFree(p); }
  } free{arguments};
  if (count < 2 || std::wstring(arguments[1]) != L"--recovery")
    return false;
  if (exitCode)
    *exitCode = 1;
  try {
    std::filesystem::path workspace, stage;
    DWORD waitPid = 0;
    for (int i = 2; i < count; ++i) {
      const std::wstring arg(arguments[i]);
      if (arg == L"--stage" && i + 1 < count) {
        stage = arguments[++i];
      } else if (arg == L"--wait-pid" && i + 1 < count) {
        const std::wstring text(arguments[++i]);
        if (!text.empty() && text.size() <= 10 &&
            text.find_first_not_of(L"0123456789") == std::wstring::npos)
          waitPid = static_cast<DWORD>(std::stoul(text));
      } else if (workspace.empty()) {
        workspace = arg;
      }
    }
    g_hInst = GetModuleHandleW(nullptr);
    InstallGlobalBeepFilter();
    const bool coInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    struct CoUninit {
      bool on;
      ~CoUninit() { if (on) CoUninitialize(); }
    } coUninit{coInitialized};
    const ::ui::ScopedAuxiliaryWindowCorners corners;
    // Handed off from the main app: let it finish exiting first.
    if (waitPid != 0) {
      OwnedHandle parent(OpenProcess(SYNCHRONIZE, FALSE, waitPid));
      if (parent.value)
        WaitForSingleObject(parent.value, 30000);
    }
    OwnedHandle toolMutex(CreateMutexW(nullptr, TRUE, StandaloneRecoveryMutexName().c_str()));
    if (!toolMutex.value || GetLastError() == ERROR_ALREADY_EXISTS) {
      ShowSilentMessageDialog(nullptr, S(L"title"), S(L"already_running"),
                              SoftNoticeKind::Warning);
      return true;
    }
    if (!WaitForMainAppToExit())
      return true;
    if (workspace.empty())
      workspace = PickWorkspace();
    if (workspace.empty())
      return true;
    if (!IsWorkspaceRoot(workspace)) {
      ShowSilentMessageDialog(nullptr, S(L"title"),
                              localization::Format(L"recovery.tool.not_workspace",
                                                   {{L"PATH", workspace.wstring()}}),
                              SoftNoticeKind::Warning);
      return true;
    }
    g_workspaceRoot = workspace.wstring();
    g_config.autoSaveSeconds = 0;
    EnterStandaloneRecoveryProcess();
    FPDF_InitLibrary();
    FPDF_SetSandBoxPolicy(FPDF_POLICY_MACHINETIME_ACCESS, false);
    LoadLibreOfficePrivateFontsForProcess();
    struct Shutdown {
      ~Shutdown() {
        ClearPdfState();
        ReleaseAllDocumentOpenLocks();
        document_source_protection::ReleaseAll();
        UnloadLibreOfficePrivateFontsForProcess();
        FPDF_DestroyLibrary();
      }
    } shutdown;
    std::wstring error;
    if (!note::ConfigureRuntimeNoteIdentityStore(workspace, &error)) {
      ShowSilentMessageDialog(nullptr, S(L"title"), S(L"open_failed"), SoftNoticeKind::Warning);
      return true;
    }
    ToolState state;
    state.workspace = workspace;
    state.preferredStage = stage;
    const int code = RunToolWindow(state);
    if (exitCode)
      *exitCode = code;
    return true;
  } catch (...) {
    return true;
  }
}

} // namespace recovery
