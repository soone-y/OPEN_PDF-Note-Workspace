// file: schedule.cpp
#include "schedule/schedule.h"

#include "core/app_core.h"
#include "core/atomic_write.h"
#include "ui/combobox_guard.h"

#include <algorithm>
#include <filesystem>
#include <vector>
#include <commctrl.h>
#include <fstream>
#include <regex>
#include <sstream>

namespace {
constexpr int kScheduleMaxDays = 7;
constexpr int kScheduleMaxPeriods = 13;
constexpr int kDefaultScheduleMask = 0x1F;
constexpr int kScheduleCellBase = 6100;
constexpr int kMargin = 12;
constexpr int kHeaderHeight = 20;
constexpr int kRowHeaderWidth = 48;
constexpr int kCellWidth = 130;
constexpr int kCellHeight = 24;
constexpr int kGap = 6;
constexpr int kComboDropHeight = 200;
static constexpr wchar_t kScheduleWndClass[] = L"LectureScheduleWnd";
static HWND g_hScheduleWnd = nullptr;
static HWND g_hGlobalMemoWnd = nullptr;
constexpr int kMemoListId = 6300;
constexpr int kMemoDeadlineId = 6301;
constexpr int kMemoTextId = 6302;
constexpr int kMemoDoneId = 6303;
constexpr int kMemoNewId = 6304;
constexpr int kMemoSaveId = 6305;
constexpr int kMemoDeleteId = 6306;
constexpr int kMemoStatusId = 6307;

struct GlobalMemo {
    std::wstring deadline;
    std::wstring text;
    bool done = false;
};

struct GlobalMemoCtx {
    std::vector<GlobalMemo> items;
    HWND list = nullptr;
    HWND deadline = nullptr;
    HWND text = nullptr;
    HWND done = nullptr;
    HWND save = nullptr;
    HWND status = nullptr;
    int editingIndex = -1;
};

static std::filesystem::path GlobalMemoPath() {
    return std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__settings__" / L"global_memos.json";
}
static std::string EscapeMemoJson(const std::wstring& text) {
    const std::string value = WideToUTF8(text);
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(c >> 4) & 0x0f];
                out += kHex[c & 0x0f];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

static bool DecodeMemoJson(const std::string& text, std::wstring* out) {
    if (!out) return false;
    std::string utf8;
    utf8.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '\\') {
            utf8 += c;
            continue;
        }
        if (++i >= text.size()) return false;
        switch (text[i]) {
        case '\\': utf8 += '\\'; break;
        case '"': utf8 += '"'; break;
        case 'n': utf8 += '\n'; break;
        case 'r': utf8 += '\r'; break;
        case 't': utf8 += '\t'; break;
        case 'b': utf8 += '\b'; break;
        case 'f': utf8 += '\f'; break;
        case 'u': {
            if (i + 4 >= text.size() || text[i + 1] != '0' || text[i + 2] != '0') return false;
            const auto hexValue = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hexValue(text[i + 3]);
            const int low = hexValue(text[i + 4]);
            if (high < 0 || low < 0) return false;
            utf8 += static_cast<char>((high << 4) | low);
            i += 4;
            break;
        }
        default: return false;
        }
    }
    *out = UTF8ToWide(utf8);
    return true;
}

static bool IsValidMemoDeadline(const std::wstring& value) {
    if (value.empty()) return true;
    if (value.size() != 16) return false;
    int y=0,m=0,d=0,h=0,min=0; wchar_t tail=0;
    if (swscanf_s(value.c_str(), L"%d-%d-%d %d:%d%c", &y,&m,&d,&h,&min,&tail,1) != 5) return false;
    SYSTEMTIME st{}; st.wYear=static_cast<WORD>(y); st.wMonth=static_cast<WORD>(m); st.wDay=static_cast<WORD>(d); st.wHour=static_cast<WORD>(h); st.wMinute=static_cast<WORD>(min);
    FILETIME ft{}; return SystemTimeToFileTime(&st, &ft) != FALSE;
}
static void LoadGlobalMemos(std::vector<GlobalMemo>* out) {
    if (!out) return;
    out->clear();
    std::ifstream in(GlobalMemoPath(), std::ios::binary);
    if (!in) return;
    std::string json((std::istreambuf_iterator<char>(in)), {});
    const std::regex entry(R"memo(\{\s*"deadline"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"text"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"done"\s*:\s*(true|false)\s*\})memo");
    for (std::sregex_iterator it(json.begin(), json.end(), entry), end; it != end; ++it) {
        GlobalMemo memo;
        memo.done = (*it)[3] == "true";
        if (DecodeMemoJson((*it)[1].str(), &memo.deadline) &&
            DecodeMemoJson((*it)[2].str(), &memo.text) &&
            IsValidMemoDeadline(memo.deadline) && !memo.text.empty()) {
            out->push_back(std::move(memo));
        }
    }
}

[[nodiscard]] static bool SaveGlobalMemos(const std::vector<GlobalMemo>& items) {
    if (g_workspaceRoot.empty()) return false;
    std::ostringstream out;
    out << "{\n  \"items\": [\n";
    for (size_t i=0;i<items.size();++i) { const auto& m=items[i]; out << "    {\"deadline\":\"" << EscapeMemoJson(m.deadline) << "\",\"text\":\"" << EscapeMemoJson(m.text) << "\",\"done\":" << (m.done ? "true" : "false") << "}" << (i+1<items.size()?",":"") << "\n"; }
    out << "  ]\n}\n"; std::wstring error; const auto path=GlobalMemoPath();
    return atomic_write::AtomicWriteUtf8(path, out.str(), path.parent_path(), &error);
}
static int CompareMemo(const GlobalMemo& a, const GlobalMemo& b) {
    if (a.done != b.done) return a.done ? 1 : -1;
    if (a.deadline.empty() != b.deadline.empty()) return a.deadline.empty() ? 1 : -1;
    return _wcsicmp(a.deadline.c_str(), b.deadline.c_str());
}

static void SetGlobalMemoStatus(GlobalMemoCtx* ctx, const std::wstring& status) {
    if (ctx && ctx->status) SetWindowTextW(ctx->status, status.c_str());
}

static void SetGlobalMemoEditingState(GlobalMemoCtx* ctx, int editingIndex) {
    if (!ctx) return;
    ctx->editingIndex = editingIndex;
    const bool isEditing = editingIndex >= 0;
    if (ctx->save) SetWindowTextW(ctx->save, isEditing ? L"変更を保存" : L"新規として保存");
    SetGlobalMemoStatus(ctx, isEditing
        ? L"選択中のメモを編集しています。保存すると、このワークスペース内の共通メモを更新します。"
        : L"新規メモ（未保存）。保存すると、このワークスペース内の共通メモに追加されます。");
}

static void RefreshGlobalMemoList(GlobalMemoCtx* ctx) {
    if (!ctx || !ctx->list) return;
    std::sort(ctx->items.begin(), ctx->items.end(), [](const auto& a, const auto& b) {
        return CompareMemo(a, b) < 0;
    });
    SendMessageW(ctx->list, LB_RESETCONTENT, 0, 0);
    SYSTEMTIME now{}; GetLocalTime(&now); FILETIME nowFt{}; SystemTimeToFileTime(&now,&nowFt); ULARGE_INTEGER nowU{}; nowU.LowPart=nowFt.dwLowDateTime; nowU.HighPart=nowFt.dwHighDateTime;
    for (const auto& m:ctx->items) { std::wstring label=(m.done?L"[完了] ":L"[未完了] ")+(m.deadline.empty()?L"期限なし":m.deadline)+L"  "+m.text; if(!m.done && !m.deadline.empty()){int y,mo,d,h,mi; swscanf_s(m.deadline.c_str(),L"%d-%d-%d %d:%d",&y,&mo,&d,&h,&mi); SYSTEMTIME st{};st.wYear=y;st.wMonth=mo;st.wDay=d;st.wHour=h;st.wMinute=mi;FILETIME f{};SystemTimeToFileTime(&st,&f);ULARGE_INTEGER u{};u.LowPart=f.dwLowDateTime;u.HighPart=f.dwHighDateTime; long long days=static_cast<long long>(u.QuadPart-nowU.QuadPart)/(10000000LL*86400); label+=days<0?L"  (期限超過)":L"  (残り"+std::to_wstring(days)+L"日)";} SendMessageW(ctx->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())); }
}

static void SelectSavedGlobalMemo(GlobalMemoCtx* ctx, const GlobalMemo& saved) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->items.size(); ++i) {
        const auto& item = ctx->items[i];
        if (item.deadline == saved.deadline && item.text == saved.text && item.done == saved.done) {
            SendMessageW(ctx->list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
            SetGlobalMemoEditingState(ctx, static_cast<int>(i));
            return;
        }
    }
    SetGlobalMemoEditingState(ctx, -1);
}

static LRESULT CALLBACK GlobalMemoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx=reinterpret_cast<GlobalMemoCtx*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_CREATE){auto* c=new GlobalMemoCtx; LoadGlobalMemos(&c->items);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c));CreateWindowW(L"STATIC",L"授業に紐づかないメモ・締切を、このワークスペースだけに保存します。PDFや授業ノートの保存とは別です。",WS_CHILD|WS_VISIBLE,12,12,740,20,hwnd,nullptr,g_hInst,nullptr);CreateWindowW(L"STATIC",L"保存済みの全体メモ",WS_CHILD|WS_VISIBLE,12,42,240,20,hwnd,nullptr,g_hInst,nullptr);c->list=CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL,12,64,740,180,hwnd,(HMENU)kMemoListId,g_hInst,nullptr);CreateWindowW(L"STATIC",L"締切（任意: YYYY-MM-DD HH:MM）",WS_CHILD|WS_VISIBLE,12,258,240,20,hwnd,nullptr,g_hInst,nullptr);c->deadline=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER,12,280,210,24,hwnd,(HMENU)kMemoDeadlineId,g_hInst,nullptr);CreateWindowW(L"STATIC",L"メモ",WS_CHILD|WS_VISIBLE,238,258,100,20,hwnd,nullptr,g_hInst,nullptr);c->text=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,238,280,514,100,hwnd,(HMENU)kMemoTextId,g_hInst,nullptr);c->done=CreateWindowW(L"BUTTON",L"完了",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,12,320,80,24,hwnd,(HMENU)kMemoDoneId,g_hInst,nullptr);CreateWindowW(L"BUTTON",L"新規メモ",WS_CHILD|WS_VISIBLE,12,400,100,28,hwnd,(HMENU)kMemoNewId,g_hInst,nullptr);c->save=CreateWindowW(L"BUTTON",L"新規として保存",WS_CHILD|WS_VISIBLE,122,400,130,28,hwnd,(HMENU)kMemoSaveId,g_hInst,nullptr);CreateWindowW(L"BUTTON",L"選択したメモを削除",WS_CHILD|WS_VISIBLE,262,400,155,28,hwnd,(HMENU)kMemoDeleteId,g_hInst,nullptr);c->status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,12,442,740,38,hwnd,(HMENU)kMemoStatusId,g_hInst,nullptr);RefreshGlobalMemoList(c);if(!c->items.empty()){const auto& memo=c->items.front();SetWindowTextW(c->deadline,memo.deadline.c_str());SetWindowTextW(c->text,memo.text.c_str());SendMessageW(c->done,BM_SETCHECK,memo.done?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(c->list,LB_SETCURSEL,0,0);SetGlobalMemoEditingState(c,0);}else{SetGlobalMemoEditingState(c,-1);}return 0;}
    if(msg==WM_COMMAND && ctx){int id=LOWORD(wp); if(id==kMemoListId&&HIWORD(wp)==LBN_SELCHANGE){int n=(int)SendMessageW(ctx->list,LB_GETCURSEL,0,0);if(n>=0&&n<(int)ctx->items.size()){SetWindowTextW(ctx->deadline,ctx->items[n].deadline.c_str());SetWindowTextW(ctx->text,ctx->items[n].text.c_str());SendMessageW(ctx->done,BM_SETCHECK,ctx->items[n].done?BST_CHECKED:BST_UNCHECKED,0);SetGlobalMemoEditingState(ctx,n);}return 0;}if(id==kMemoNewId){SetWindowTextW(ctx->deadline,L"");SetWindowTextW(ctx->text,L"");SendMessageW(ctx->done,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(ctx->list,LB_SETCURSEL,static_cast<WPARAM>(-1),0);SetGlobalMemoEditingState(ctx,-1);SetFocus(ctx->text);return 0;}if(id==kMemoSaveId){const int deadlineLength=GetWindowTextLengthW(ctx->deadline);const int textLength=GetWindowTextLengthW(ctx->text);std::wstring deadline(static_cast<size_t>(deadlineLength)+1,L'\0'),text(static_cast<size_t>(textLength)+1,L'\0');GetWindowTextW(ctx->deadline,deadline.data(),deadlineLength+1);GetWindowTextW(ctx->text,text.data(),textLength+1);deadline.resize(deadlineLength);text.resize(textLength);if(!IsValidMemoDeadline(deadline)){SetGlobalMemoStatus(ctx,L"保存していません。締切は YYYY-MM-DD HH:MM 形式にするか、空欄にしてください。");return 0;}if(text.empty()){SetGlobalMemoStatus(ctx,L"保存していません。メモを入力してください。");return 0;}GlobalMemo m{deadline,text,SendMessageW(ctx->done,BM_GETCHECK,0,0)==BST_CHECKED};auto next=ctx->items;if(ctx->editingIndex>=0&&ctx->editingIndex<(int)next.size())next[ctx->editingIndex]=m;else next.push_back(m);if(!SaveGlobalMemos(next)){SetGlobalMemoStatus(ctx,L"保存できませんでした。内容は画面に残っています。保存先の権限・空き容量を確認してください。");return 0;}ctx->items=std::move(next);RefreshGlobalMemoList(ctx);SelectSavedGlobalMemo(ctx,m);SetGlobalMemoStatus(ctx,L"保存しました。このワークスペース内の共通メモとして保存済みです。");return 0;}if(id==kMemoDeleteId){if(ctx->editingIndex<0||ctx->editingIndex>=(int)ctx->items.size()){SetGlobalMemoStatus(ctx,L"削除するメモを一覧から選択してください。");return 0;}auto next=ctx->items;next.erase(next.begin()+ctx->editingIndex);if(!SaveGlobalMemos(next)){SetGlobalMemoStatus(ctx,L"削除を保存できませんでした。メモは残っています。");return 0;}ctx->items=std::move(next);RefreshGlobalMemoList(ctx);SetWindowTextW(ctx->deadline,L"");SetWindowTextW(ctx->text,L"");SendMessageW(ctx->done,BM_SETCHECK,BST_UNCHECKED,0);SetGlobalMemoEditingState(ctx,-1);SetGlobalMemoStatus(ctx,L"削除しました。変更後の一覧をこのワークスペースに保存済みです。");return 0;}}
    if(msg==WM_NCDESTROY){delete ctx;if(g_hGlobalMemoWnd==hwnd)g_hGlobalMemoWnd=nullptr;} return DefWindowProcW(hwnd,msg,wp,lp);
}

struct ScheduleCtx {
    int columns = 5;
    int periods = 6;
    std::vector<int> dayIndices;
    std::vector<int> cellMap;
    std::vector<HWND> dayLabels;
    std::vector<HWND> periodLabels;
    std::vector<HWND> cells;
    std::vector<std::wstring> lectureNames;
};

static void EnsureScheduleConfig() {
    g_config.scheduleDayMask &= 0x7F;
    if (g_config.scheduleDayMask == 0) g_config.scheduleDayMask = kDefaultScheduleMask;
    g_config.schedulePeriods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
    size_t cellTotal = static_cast<size_t>(kScheduleMaxDays * g_config.schedulePeriods);
    if (g_config.scheduleCells.size() < cellTotal) {
        g_config.scheduleCells.resize(cellTotal);
    } else if (g_config.scheduleCells.size() > cellTotal) {
        g_config.scheduleCells.resize(cellTotal);
    }
    size_t timeTotal = static_cast<size_t>(kScheduleMaxDays * kScheduleMaxPeriods);
    if (g_config.scheduleStartTimes.size() < timeTotal) {
        g_config.scheduleStartTimes.resize(timeTotal);
    } else if (g_config.scheduleStartTimes.size() > timeTotal) {
        g_config.scheduleStartTimes.resize(timeTotal);
    }
}

static std::vector<int> BuildEnabledDayIndices(int mask) {
    std::vector<int> indices;
    for (int i = 0; i < kScheduleMaxDays; ++i) {
        if (mask & (1 << i)) indices.push_back(i);
    }
    if (indices.empty()) indices.push_back(0);
    return indices;
}

static std::vector<std::wstring> BuildDayLabels(const std::vector<int>& dayIndices) {
    std::vector<std::wstring> labels;
    const std::vector<std::wstring> ja = { L"月", L"火", L"水", L"木", L"金", L"土", L"日" };
    const std::vector<std::wstring> en = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    const auto& base = IsEnglishUi() ? en : ja;
    for (int idx : dayIndices) {
        if (idx >= 0 && idx < static_cast<int>(base.size())) {
            labels.push_back(base[static_cast<size_t>(idx)]);
        }
    }
    return labels;
}

static std::wstring BuildPeriodLabel(int index) {
    int n = index + 1;
    if (IsEnglishUi()) {
        return L"P" + std::to_wstring(n);
    }
    return std::to_wstring(n) + L"限";
}

static std::vector<std::wstring> CollectLectureNames() {
    std::vector<std::wstring> names;
    if (!g_lectures.empty()) {
        names.reserve(g_lectures.size());
        for (const auto& path : g_lectures) {
            if (path.empty()) continue;
            std::wstring name = std::filesystem::path(path).filename().wstring();
            if (!name.empty()) names.push_back(std::move(name));
        }
        if (!names.empty()) return names;
    }
    if (g_workspaceRoot.empty()) return names;
    std::error_code ec;
    auto classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    if (!std::filesystem::exists(classesPath, ec)) return names;
    auto cacheName = std::filesystem::path(g_config.cacheDir).filename().wstring();
    for (const auto& entry : std::filesystem::directory_iterator(classesPath, ec)) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(entry.path(), isReparse) && isReparse) continue;
        std::error_code stEc;
        if (!entry.is_directory(stEc) || stEc) continue;
        auto name = entry.path().filename().wstring();
        if (!cacheName.empty() && name == cacheName) continue;
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

static void LayoutSchedule(HWND hWnd, ScheduleCtx* ctx) {
    if (!ctx) return;
    auto dayLabels = BuildDayLabels(ctx->dayIndices);
    int columns = static_cast<int>(dayLabels.size());
    int startX = kMargin + kRowHeaderWidth + kGap;
    int startY = kMargin + kHeaderHeight + kGap;

    for (int col = 0; col < columns; ++col) {
        int x = startX + col * (kCellWidth + kGap);
        if (col < static_cast<int>(ctx->dayLabels.size())) {
            SetWindowPos(ctx->dayLabels[col], nullptr, x, kMargin, kCellWidth, kHeaderHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowTextW(ctx->dayLabels[col], dayLabels[static_cast<size_t>(col)].c_str());
        }
    }

    for (int p = 0; p < ctx->periods; ++p) {
        int y = startY + p * (kCellHeight + kGap);
        if (p < static_cast<int>(ctx->periodLabels.size())) {
            auto label = BuildPeriodLabel(p);
            SetWindowPos(ctx->periodLabels[p], nullptr, kMargin, y, kRowHeaderWidth, kCellHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowTextW(ctx->periodLabels[p], label.c_str());
        }
    }

    for (int p = 0; p < ctx->periods; ++p) {
        for (int col = 0; col < columns; ++col) {
            size_t index = static_cast<size_t>(p * columns + col);
            if (index >= ctx->cells.size()) continue;
            int x = startX + col * (kCellWidth + kGap);
            int y = startY + p * (kCellHeight + kGap);
            SetWindowPos(ctx->cells[index], nullptr, x, y, kCellWidth, kCellHeight + kComboDropHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

static void FillScheduleCombo(HWND combo,
                              const std::vector<std::wstring>& lectureNames,
                              const std::wstring& current) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"-"));
    for (const auto& name : lectureNames) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    int sel = 0;
    if (!current.empty()) {
        for (size_t i = 0; i < lectureNames.size(); ++i) {
            if (lectureNames[i] == current) {
                sel = static_cast<int>(i + 1);
                break;
            }
        }
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
    SendMessageW(combo, CB_SETDROPPEDWIDTH, kCellWidth * 2, 0);
}

static void InitScheduleUi(HWND hWnd, ScheduleCtx* ctx) {
    if (!ctx) return;
    ctx->lectureNames = CollectLectureNames();
    ctx->dayIndices = BuildEnabledDayIndices(g_config.scheduleDayMask);
    ctx->columns = static_cast<int>(ctx->dayIndices.size());
    ctx->dayLabels.resize(static_cast<size_t>(ctx->columns));
    ctx->periodLabels.resize(static_cast<size_t>(ctx->periods));
    ctx->cells.resize(static_cast<size_t>(ctx->columns * ctx->periods));
    ctx->cellMap.resize(ctx->cells.size());

    for (int col = 0; col < ctx->columns; ++col) {
        ctx->dayLabels[col] = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        SetUIFont(ctx->dayLabels[col]);
    }

    for (int p = 0; p < ctx->periods; ++p) {
        ctx->periodLabels[p] = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        SetUIFont(ctx->periodLabels[p]);
    }

    for (int p = 0; p < ctx->periods; ++p) {
        for (int col = 0; col < ctx->columns; ++col) {
            size_t index = static_cast<size_t>(p * ctx->columns + col);
            int controlId = kScheduleCellBase + static_cast<int>(index);
            HWND combo = CreateWindowExW(
                0, WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 0, 0, hWnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                g_hInst, nullptr);
            ctx->cells[index] = combo;
            SetUIFont(combo);
            ui::GuardComboAgainstAccidentalChange(combo);
            int dayIndex = ctx->dayIndices[static_cast<size_t>(col)];
            size_t scheduleIndex = static_cast<size_t>(p * kScheduleMaxDays + dayIndex);
            ctx->cellMap[index] = static_cast<int>(scheduleIndex);
            const std::wstring& current = (scheduleIndex < g_config.scheduleCells.size())
                                          ? g_config.scheduleCells[scheduleIndex]
                                          : L"";
            FillScheduleCombo(combo, ctx->lectureNames, current);
        }
    }
    LayoutSchedule(hWnd, ctx);
}

static void RefreshScheduleLectureNames(ScheduleCtx* ctx) {
    if (!ctx) return;
    auto names = CollectLectureNames();
    if (names == ctx->lectureNames) return;
    ctx->lectureNames = std::move(names);
    for (size_t i = 0; i < ctx->cells.size(); ++i) {
        size_t scheduleIndex = static_cast<size_t>(ctx->cellMap[i]);
        const std::wstring& current = (scheduleIndex < g_config.scheduleCells.size())
                                      ? g_config.scheduleCells[scheduleIndex]
                                      : L"";
        FillScheduleCombo(ctx->cells[i], ctx->lectureNames, current);
    }
}

static LRESULT CALLBACK ScheduleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ScheduleCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        EnsureScheduleConfig();
        auto* newCtx = new ScheduleCtx();
        newCtx->periods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newCtx));
        InitScheduleUi(hWnd, newCtx);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_SIZE:
        if (ctx) {
            LayoutSchedule(hWnd, ctx);
        }
        return 0;
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (!ctx) break;
        if (id >= kScheduleCellBase &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            size_t index = static_cast<size_t>(id - kScheduleCellBase);
            if (index >= ctx->cellMap.size()) return 0;
            HWND combo = reinterpret_cast<HWND>(lParam);
            int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            std::wstring value;
            if (sel > 0) {
                size_t nameIndex = static_cast<size_t>(sel - 1);
                if (nameIndex < ctx->lectureNames.size()) {
                    value = ctx->lectureNames[nameIndex];
                }
            }
            size_t scheduleIndex = static_cast<size_t>(ctx->cellMap[index]);
            if (scheduleIndex < g_config.scheduleCells.size()) {
                g_config.scheduleCells[scheduleIndex] = std::move(value);
                PersistConfig();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_NCDESTROY: {
        if (ctx) {
            delete ctx;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        if (g_hScheduleWnd == hWnd) g_hScheduleWnd = nullptr;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void CalcScheduleWindowSize(int columns, int periods, int& outW, int& outH) {
    int clientW = kMargin * 2 + kRowHeaderWidth + kGap + columns * kCellWidth + (columns - 1) * kGap;
    int clientH = kMargin * 2 + kHeaderHeight + kGap + periods * kCellHeight + (periods - 1) * kGap;
    RECT rc{ 0, 0, clientW, clientH };
    DWORD style = WS_CAPTION | WS_POPUPWINDOW;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    outW = rc.right - rc.left;
    outH = rc.bottom - rc.top;
}

} // namespace

void RefreshScheduleWindowLectureNames() {
    if (!g_hScheduleWnd) return;
    auto* ctx = reinterpret_cast<ScheduleCtx*>(GetWindowLongPtrW(g_hScheduleWnd, GWLP_USERDATA));
    RefreshScheduleLectureNames(ctx);
}

void ShowScheduleWindow(HWND parent) {
    if (g_hScheduleWnd) {
        ShowWindow(g_hScheduleWnd, SW_SHOW);
        SetForegroundWindow(g_hScheduleWnd);
        return;
    }
    EnsureScheduleConfig();
    WNDCLASSW wc{};
    wc.lpfnWndProc = ScheduleWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                           : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kScheduleWndClass;
    RegisterClassW(&wc);

    int periods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
    int columns = static_cast<int>(BuildEnabledDayIndices(g_config.scheduleDayMask).size());
    int width = 0;
    int height = 0;
    CalcScheduleWindowSize(columns, periods, width, height);

    g_hScheduleWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        GetUiText().menuLectureSchedule.c_str(),
        WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        parent, nullptr, g_hInst, nullptr);
    if (g_hScheduleWnd) {
        ShowWindow(g_hScheduleWnd, SW_SHOW);
        UpdateWindow(g_hScheduleWnd);
    }
}

void ShowGlobalMemoWindow(HWND parent) {
    if (g_hGlobalMemoWnd) {
        ShowWindow(g_hGlobalMemoWnd, SW_SHOW);
        SetForegroundWindow(g_hGlobalMemoWnd);
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = GlobalMemoProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PdfNoteGlobalMemoWnd";
    RegisterClassW(&wc);
    g_hGlobalMemoWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName,
        IsEnglishUi() ? L"Global deadlines and memos" : L"全体メモ・締切管理",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 780, 530,
        parent, nullptr, g_hInst, nullptr);
}


