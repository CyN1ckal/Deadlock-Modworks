// Win32 window for the analyze-then-repair workflow. Work runs on one background thread;
// it hands log lines, progress and finished archive reports back through a queue.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "main_window.hpp"

#include "cancellation.hpp"
#include "error.hpp"
#include "game_folder.hpp"
#include "report.hpp"
#include "strings.hpp"
#include "updater.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace skeleton_repair {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"DeadlockSkeletonRepairWindow";
constexpr UINT kWorkerMessage = WM_APP + 1;

enum ControlId : int {
    IdGameLabel = 100,
    IdGame,
    IdGameBrowse,
    IdModsLabel,
    IdMods,
    IdModsBrowse,
    IdHint,
    IdAnalyze,
    IdRepair,
    IdExport,
    IdCancel,
    IdProgress,
    IdStatus,
    IdResults,
    IdLog,
    IdCredit,
};

struct WorkerEvent {
    enum class Kind { Log, Progress, Archive, Finished };
    Kind Type = Kind::Log;
    std::string Text;
    UpdateProgress Progress;
    ArchiveReport Archive;
    bool Cancelled = false;
};

struct ResultRow {
    ModelOutcome Outcome = ModelOutcome::Failed;
    std::wstring Detail;
};

std::wstring ResultName(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return L"Repairable";
    case ModelOutcome::NeedsReview: return L"Needs review";
    case ModelOutcome::Updated: return L"Updated";
    case ModelOutcome::Current: return L"Current";
    case ModelOutcome::Skipped: return L"Skipped";
    case ModelOutcome::Failed: return L"Failed";
    }
    return L"Failed";
}

COLORREF ResultColor(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Updated:
    case ModelOutcome::Repairable: return RGB(0, 100, 0);
    case ModelOutcome::NeedsReview:
    case ModelOutcome::Skipped: return RGB(145, 83, 0);
    case ModelOutcome::Failed: return RGB(139, 0, 0);
    case ModelOutcome::Current: break;
    }
    return CLR_DEFAULT;
}

std::string FileName(const std::string& path) {
    return Narrow(fs::path(Widen(path)).filename().wstring());
}

std::wstring GetText(HWND control) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
    return text;
}

std::optional<std::wstring> ItemPath(IShellItem* item) {
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return std::nullopt;
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

void StartIn(IFileDialog* dialog, const std::wstring& folder) {
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) {
        return;
    }
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dialog->SetFolder(item.Get());
    }
}

class MainWindow {
public:
    ~MainWindow() {
        if (cancellation_) {
            cancellation_->Cancel();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        DeleteObject(font_);
        DeleteObject(mono_font_);
    }

    int Run(const std::vector<std::string>& initial_mods) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = &MainWindow::WindowProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        window_class.lpszClassName = kWindowClass;
        RegisterClassExW(&window_class);

        hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, L"Deadlock Skeleton Repair",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 780, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            return 2;
        }
        CenterOnMonitor();

        try {
            if (const auto game = FindGameFolder()) {
                SetWindowTextW(game_, Widen(*game).c_str());
            }
        } catch (const std::exception&) {
            SetStatus(L"Automatic game detection was unavailable. Choose the citadel folder manually.");
        }
        if (!initial_mods.empty()) {
            std::vector<std::wstring> files;
            for (const std::string& mod : initial_mods) {
                files.push_back(Widen(mod));
            }
            SelectMods(files);
        }

        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        SetFocus(analyze_);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self != nullptr ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {Scale(840), Scale(580)};
            return 0;
        }
        case WM_DPICHANGED: {
            const UINT old_dpi = dpi_;
            dpi_ = HIWORD(wparam);
            ApplyFonts();
            for (int column = 0; column < 4; ++column) {
                const int width = ListView_GetColumnWidth(results_, column);
                ListView_SetColumnWidth(results_, column, MulDiv(width, static_cast<int>(dpi_), static_cast<int>(old_dpi)));
            }
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_NOTIFY:
            return OnNotify(reinterpret_cast<NMHDR*>(lparam));
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            if (!running_) {
                std::vector<std::wstring> files;
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i) {
                    std::wstring file(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                    file.resize(DragQueryFileW(drop, i, file.data(), static_cast<UINT>(file.size())));
                    files.push_back(std::move(file));
                }
                SelectMods(files);
            }
            DragFinish(drop);
            return 0;
        }
        case kWorkerMessage:
            DrainEvents();
            return 0;
        case WM_CLOSE:
            if (running_) {
                cancellation_->Cancel();
                SetStatus(L"Cancelling. Close the window after the operation stops.");
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    HWND AddControl(const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD ex_style = 0) {
        return CreateWindowExW(ex_style, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }

    void CreateControls() {
        dpi_ = GetDpiForWindow(hwnd_);
        game_label_ = AddControl(WC_STATICW, L"Citadel folder", SS_NOPREFIX, IdGameLabel);
        game_ = AddControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IdGame, WS_EX_CLIENTEDGE);
        game_browse_ = AddControl(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IdGameBrowse);
        mods_label_ = AddControl(WC_STATICW, L"Mod VPKs", SS_NOPREFIX, IdModsLabel);
        mods_ = AddControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, IdMods, WS_EX_CLIENTEDGE);
        mods_browse_ = AddControl(WC_BUTTONW, L"Choose VPKs...", WS_TABSTOP | BS_PUSHBUTTON, IdModsBrowse);
        hint_ = AddControl(WC_STATICW,
            L"Analyze first. Repairs replace each supported VPK and create an original backup. Mods needing review are left untouched.",
            SS_NOPREFIX, IdHint);
        analyze_ = AddControl(WC_BUTTONW, L"1. Analyze mods", WS_TABSTOP | BS_PUSHBUTTON, IdAnalyze);
        repair_ = AddControl(WC_BUTTONW, L"2. Repair supported mods", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdRepair);
        export_ = AddControl(WC_BUTTONW, L"Save report", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdExport);
        cancel_ = AddControl(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdCancel);
        progress_ = AddControl(PROGRESS_CLASSW, L"", 0, IdProgress);
        status_ = AddControl(WC_STATICW, L"Choose or drop mod VPKs, then analyze them.", SS_NOPREFIX, IdStatus);
        results_ = AddControl(WC_LISTVIEWW, L"",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IdResults, WS_EX_CLIENTEDGE);
        log_ = AddControl(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IdLog,
            WS_EX_CLIENTEDGE);
        credit_ = AddControl(WC_LINK,
            L"Powered by <a href=\"https://s2v.app\">Source 2 Viewer / ValveResourceFormat</a>", WS_TABSTOP, IdCredit);

        SendMessageW(log_, EM_SETLIMITTEXT, 0, 0);
        ListView_SetExtendedListViewStyle(results_,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_LABELTIP);
        const std::pair<const wchar_t*, int> columns[] = {{L"VPK", 140}, {L"Resource", 310}, {L"Result", 110}, {L"Detail", 460}};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(columns[i].first);
            column.cx = Scale(columns[i].second);
            ListView_InsertColumn(results_, i, &column);
        }
        ApplyFonts();
    }

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    void ApplyFonts() {
        const HFONT old_font = font_;
        const HFONT old_mono = mono_font_;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW mono{};
        mono.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        mono.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(mono.lfFaceName, L"Consolas");
        mono_font_ = CreateFontIndirectW(&mono);
        for (HWND control = GetWindow(hwnd_, GW_CHILD); control != nullptr; control = GetWindow(control, GW_HWNDNEXT)) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control == log_ ? mono_font_ : font_), TRUE);
        }
        DeleteObject(old_font);
        DeleteObject(old_mono);
    }

    void CenterOnMonitor() {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& area = monitor.rcWork;
        const int width = std::min(Scale(1100), static_cast<int>(area.right - area.left));
        const int height = std::min(Scale(780), static_cast<int>(area.bottom - area.top));
        SetWindowPos(hwnd_, nullptr, area.left + (area.right - area.left - width) / 2,
            area.top + (area.bottom - area.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int LineHeight() const {
        const HDC dc = GetDC(hwnd_);
        const HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        return metrics.tmHeight;
    }

    int WrappedHeight(HWND control, int width) const {
        const std::wstring text = GetText(control);
        const HDC dc = GetDC(hwnd_);
        const HGDIOBJ old = SelectObject(dc, font_);
        RECT bounds{0, 0, width, 0};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        return std::max(static_cast<int>(bounds.bottom), LineHeight());
    }

    void Layout() {
        if (results_ == nullptr) {
            return;
        }
        RECT client;
        GetClientRect(hwnd_, &client);
        const int pad = Scale(12);
        const int gap = Scale(6);
        const int width = std::max(0, static_cast<int>(client.right) - pad * 2);
        const int label_width = Scale(110);
        const int browse_width = Scale(110);
        const int row = Scale(32);
        const int line = LineHeight();
        const int edit_height = line + Scale(8);
        const int button_height = Scale(28);
        int y = pad;

        for (const auto& [label, edit, browse] : {std::tuple{game_label_, game_, game_browse_}, std::tuple{mods_label_, mods_, mods_browse_}}) {
            MoveWindow(label, pad, y + (row - line) / 2, label_width, line, TRUE);
            MoveWindow(edit, pad + label_width, y + (row - edit_height) / 2,
                std::max(0, width - label_width - browse_width - gap), edit_height, TRUE);
            MoveWindow(browse, pad + width - browse_width, y + (row - button_height) / 2, browse_width, button_height, TRUE);
            y += row;
        }

        y += Scale(8);
        const int hint_height = WrappedHeight(hint_, width);
        MoveWindow(hint_, pad, y, width, hint_height, TRUE);
        y += hint_height + gap;

        int x = pad;
        for (HWND button : {analyze_, repair_, export_, cancel_}) {
            SIZE ideal{};
            Button_GetIdealSize(button, &ideal);
            const int button_width = std::max(static_cast<int>(ideal.cx) + Scale(16), Scale(75));
            MoveWindow(button, x, y, button_width, button_height, TRUE);
            x += button_width + gap;
        }
        MoveWindow(progress_, x + Scale(4), y + (button_height - Scale(20)) / 2, Scale(180), Scale(20), TRUE);
        y += button_height + gap;

        const int status_height = WrappedHeight(status_, width);
        MoveWindow(status_, pad, y, width, status_height, TRUE);
        y += status_height + Scale(8);

        const int credit_height = line + Scale(2);
        const int credit_top = static_cast<int>(client.bottom) - pad - credit_height;
        MoveWindow(credit_, pad, credit_top, width, credit_height, TRUE);

        const int remaining = std::max(0, credit_top - gap - y - gap);
        const int results_height = remaining * 62 / 100;
        MoveWindow(results_, pad, y, width, results_height, TRUE);
        MoveWindow(log_, pad, y + results_height + gap, width, remaining - results_height, TRUE);
    }

    void SetStatus(const std::wstring& text) {
        SetWindowTextW(status_, text.c_str());
        Layout();
    }

    void Log(const std::string& message) {
        const std::wstring line = Widen(message) + L"\r\n";
        const int end = GetWindowTextLengthW(log_);
        SendMessageW(log_, EM_SETSEL, end, end);
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    }

    void OnCommand(int id, int code) {
        if (id == IdGame && code == EN_CHANGE) {
            if (!running_) {
                InvalidateAnalysis();
            }
            return;
        }
        if (code != BN_CLICKED) {
            return;
        }
        switch (id) {
        case IdGameBrowse: BrowseGame(); break;
        case IdModsBrowse: BrowseMods(); break;
        case IdAnalyze: Start(false); break;
        case IdRepair: Start(true); break;
        case IdExport: ExportReport(); break;
        case IdCancel:
            if (cancellation_) {
                cancellation_->Cancel();
                SetStatus(L"Cancelling after the current operation...");
            }
            break;
        default: break;
        }
    }

    LRESULT OnNotify(NMHDR* header) {
        if (header->hwndFrom == credit_ && (header->code == NM_CLICK || header->code == NM_RETURN)) {
            ShellExecuteW(hwnd_, L"open", reinterpret_cast<NMLINK*>(header)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (header->hwndFrom != results_) {
            return 0;
        }
        switch (header->code) {
        case NM_CUSTOMDRAW: {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec < rows_.size()) {
                const COLORREF color = ResultColor(rows_[draw->nmcd.dwItemSpec].Outcome);
                if (color != CLR_DEFAULT) {
                    draw->clrText = color;
                }
            }
            return CDRF_DODEFAULT;
        }
        case LVN_GETINFOTIPW: {
            auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(header);
            if (tip->iItem >= 0 && static_cast<size_t>(tip->iItem) < rows_.size() && tip->cchTextMax > 0) {
                wcsncpy_s(tip->pszText, tip->cchTextMax, rows_[tip->iItem].Detail.c_str(), _TRUNCATE);
            }
            return 0;
        }
        case LVN_ITEMCHANGED: {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
            if ((change->uNewState & LVIS_SELECTED) != 0 && (change->uOldState & LVIS_SELECTED) == 0
                && change->iItem >= 0 && static_cast<size_t>(change->iItem) < rows_.size()) {
                SetStatus(rows_[change->iItem].Detail);
            }
            return 0;
        }
        default:
            return 0;
        }
    }

    void BrowseGame() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            return;
        }
        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"Select Deadlock's game/citadel folder");
        StartIn(dialog.Get(), GetText(game_));
        ComPtr<IShellItem> item;
        if (FAILED(dialog->Show(hwnd_)) || FAILED(dialog->GetResult(&item))) {
            return;
        }
        if (const auto path = ItemPath(item.Get())) {
            SetWindowTextW(game_, path->c_str());
        }
    }

    void BrowseMods() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            return;
        }
        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"Choose one or more mod VPKs");
        const COMDLG_FILTERSPEC filter{L"VPK archives (*.vpk)", L"*.vpk"};
        dialog->SetFileTypes(1, &filter);
        const std::wstring game = GetText(game_);
        if (!game.empty()) {
            StartIn(dialog.Get(), (fs::path(game) / L"addons").wstring());
        }
        ComPtr<IShellItemArray> items;
        if (FAILED(dialog->Show(hwnd_)) || FAILED(dialog->GetResults(&items))) {
            return;
        }
        DWORD count = 0;
        items->GetCount(&count);
        std::vector<std::wstring> files;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(items->GetItemAt(i, &item))) {
                if (const auto path = ItemPath(item.Get())) {
                    files.push_back(*path);
                }
            }
        }
        SelectMods(files);
    }

    void SelectMods(const std::vector<std::wstring>& files) {
        std::vector<std::string> paths;
        for (const std::wstring& file : files) {
            const fs::path path(file);
            if (!EqualsIgnoreCase(Narrow(path.extension().wstring()), ".vpk")) {
                continue;
            }
            std::error_code error;
            const fs::path full = fs::absolute(path, error);
            const std::string narrow = Narrow((error ? path : full).lexically_normal().wstring());
            if (std::none_of(paths.begin(), paths.end(), [&](const std::string& other) { return EqualsIgnoreCase(other, narrow); })) {
                paths.push_back(narrow);
            }
        }
        if (paths.empty()) {
            return;
        }
        mods_list_ = paths;
        std::string text = paths.front();
        if (paths.size() > 1) {
            text = std::to_string(paths.size()) + " VPKs: ";
            for (size_t i = 0; i < paths.size(); ++i) {
                text += (i == 0 ? "" : ", ") + FileName(paths[i]);
            }
        }
        SetWindowTextW(mods_, Widen(text).c_str());
        InvalidateAnalysis();
        SetStatus(std::to_wstring(paths.size()) + L" VPK(s) selected. Click Analyze mods.");
    }

    void InvalidateAnalysis() {
        reports_.clear();
        RenderReports();
        EnableWindow(repair_, FALSE);
        EnableWindow(export_, FALSE);
        analyzed_game_.reset();
    }

    void SetBusy(bool busy) {
        for (HWND control : {game_, game_browse_, mods_, mods_browse_, analyze_}) {
            EnableWindow(control, !busy);
        }
        EnableWindow(cancel_, busy);
        if (busy) {
            EnableWindow(repair_, FALSE);
            EnableWindow(export_, FALSE);
            SetFocus(cancel_);
        }
    }

    void Start(bool repair) {
        if (running_) {
            return;
        }
        const std::wstring game_text = GetText(game_);
        std::error_code error;
        if (game_text.empty() || !fs::is_regular_file(fs::path(game_text) / L"pak01_dir.vpk", error) || mods_list_.empty()) {
            SetStatus(L"Choose the citadel folder containing pak01_dir.vpk and at least one mod VPK.");
            return;
        }
        const std::string game = Narrow(fs::absolute(fs::path(game_text), error).lexically_normal().wstring());
        std::vector<std::string> targets;
        if (repair) {
            for (const ArchiveReport& archive : reports_) {
                if (archive.Report.CanRepair()) {
                    targets.push_back(archive.ModPath);
                }
            }
            if (analyzed_game_ != game || targets.empty()) {
                return;
            }
        } else {
            targets = mods_list_;
            reports_.clear();
            SetWindowTextW(log_, L"");
        }
        RenderReports();

        running_ = true;
        running_game_ = game;
        cancellation_.emplace();
        SetBusy(true);
        worker_ = std::thread([this, game, targets, repair, token = cancellation_->Token()] {
            RunWorker(game, targets, repair, token);
        });
    }

    void Post(WorkerEvent event) {
        {
            std::lock_guard lock(events_mutex_);
            events_.push_back(std::move(event));
        }
        PostMessageW(hwnd_, kWorkerMessage, 0, 0);
    }

    void PostLog(std::string text) {
        WorkerEvent event;
        event.Type = WorkerEvent::Kind::Log;
        event.Text = std::move(text);
        Post(std::move(event));
    }

    // Runs on the worker thread. Touches no window state directly.
    void RunWorker(const std::string& game, const std::vector<std::string>& targets, bool repair, CancellationToken token) {
        bool cancelled = false;
        try {
            for (const std::string& mod : targets) {
                token.ThrowIfCancelled();
                PostLog((repair ? "Repairing " : "Analyzing ") + mod);
                const auto progress = [this](const UpdateProgress& update) {
                    WorkerEvent event;
                    event.Type = WorkerEvent::Kind::Progress;
                    event.Progress = update;
                    Post(std::move(event));
                };
                UpdateReport report;
                std::optional<std::string> failure;
                try {
                    report = repair ? HeroModelUpdater::Update(game, mod, std::nullopt, true, true, progress, token)
                                    : HeroModelUpdater::Analyze(game, mod, progress, token);
                } catch (const OperationCancelled&) {
                    throw;
                } catch (const std::exception& ex) {
                    failure = ex.what();
                } catch (...) {
                    failure = "Unexpected error.";
                }
                if (failure) {
                    report = UpdateReport{};
                    ModelResult failed;
                    failed.Model = FileName(mod);
                    failed.Outcome = ModelOutcome::Failed;
                    failed.Detail = *failure;
                    report.Results.push_back(std::move(failed));
                    PostLog(*failure);
                }
                WorkerEvent event;
                event.Type = WorkerEvent::Kind::Archive;
                event.Archive = ArchiveReport{mod, std::move(report)};
                Post(std::move(event));
            }
        } catch (const OperationCancelled&) {
            cancelled = true;
        }
        WorkerEvent finished;
        finished.Type = WorkerEvent::Kind::Finished;
        finished.Cancelled = cancelled;
        Post(std::move(finished));
    }

    void DrainEvents() {
        std::deque<WorkerEvent> events;
        {
            std::lock_guard lock(events_mutex_);
            events.swap(events_);
        }
        for (WorkerEvent& event : events) {
            switch (event.Type) {
            case WorkerEvent::Kind::Log:
                Log(event.Text);
                break;
            case WorkerEvent::Kind::Progress:
                if (running_) {
                    ShowProgress(event.Progress);
                }
                break;
            case WorkerEvent::Kind::Archive:
                OnArchive(std::move(event.Archive));
                break;
            case WorkerEvent::Kind::Finished:
                OnFinished(event.Cancelled);
                break;
            }
        }
    }

    void SetMarquee(bool marquee) {
        const LONG_PTR style = GetWindowLongPtrW(progress_, GWL_STYLE);
        const bool current = (style & PBS_MARQUEE) != 0;
        if (current == marquee) {
            return;
        }
        if (!marquee) {
            SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        }
        SetWindowLongPtrW(progress_, GWL_STYLE, marquee ? style | PBS_MARQUEE : style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        if (marquee) {
            SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 30);
        }
    }

    void ShowProgress(const UpdateProgress& update) {
        SetMarquee(update.Total == 0);
        const int maximum = std::max(1, update.Total);
        SendMessageW(progress_, PBM_SETRANGE32, 0, maximum);
        SendMessageW(progress_, PBM_SETPOS, std::clamp(update.Completed, 0, maximum), 0);
        SetStatus(Widen(update.Message));
        Log(update.Message);
    }

    void OnArchive(ArchiveReport archive) {
        std::erase_if(reports_, [&](const ArchiveReport& other) { return EqualsIgnoreCase(other.ModPath, archive.ModPath); });
        for (const ModelResult& result : archive.Report.Results) {
            Log(Narrow(ResultName(result.Outcome)) + ": " + result.Model + ": " + result.Detail);
            if (result.Warnings) {
                for (const std::string& warning : *result.Warnings) {
                    Log("Note: " + warning);
                }
            }
        }
        if (archive.Report.BackupPath) {
            Log("Original backup: " + *archive.Report.BackupPath);
        }
        reports_.push_back(std::move(archive));
        RenderReports();
    }

    void OnFinished(bool cancelled) {
        if (worker_.joinable()) {
            worker_.join();
        }
        if (cancelled) {
            analyzed_game_.reset();
            SetStatus(L"Cancelled. Completed repairs and their backup paths remain in the report. Analyze again to continue.");
        } else {
            analyzed_game_ = running_game_;
            const auto count = [&](auto predicate) { return std::count_if(reports_.begin(), reports_.end(), predicate); };
            const auto ready = count([](const ArchiveReport& r) { return r.Report.CanRepair(); });
            const auto blocked = count([](const ArchiveReport& r) { return r.Report.HasBlockers(); });
            const auto updated = count([](const ArchiveReport& r) { return r.Report.WroteFile; });
            SetStatus(std::to_wstring(updated) + L" repaired; " + std::to_wstring(ready) + L" repairable; " + std::to_wstring(blocked)
                + L" need review or are unsupported. Select a result for details. Test repaired mods in-game.");
        }

        running_ = false;
        cancellation_.reset();
        SetBusy(false);
        const bool can_repair = analyzed_game_ == running_game_
            && std::any_of(reports_.begin(), reports_.end(), [](const ArchiveReport& r) { return r.Report.CanRepair(); });
        EnableWindow(repair_, can_repair);
        EnableWindow(export_, !reports_.empty());
        SetMarquee(false);
        RenderReports();
        SetFocus(can_repair ? repair_ : analyze_);
    }

    void RenderReports() {
        SendMessageW(results_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(results_);
        rows_.clear();
        for (const ArchiveReport& archive : reports_) {
            for (const ModelResult& result : archive.Report.Results) {
                std::string detail = result.Detail;
                if (result.Warnings && !result.Warnings->empty()) {
                    detail += " Notes:";
                    for (const std::string& warning : *result.Warnings) {
                        detail += " " + warning;
                    }
                }
                ResultRow row{result.Outcome, Widen(detail)};
                std::wstring cells[] = {Widen(FileName(archive.ModPath)), Widen(result.Model), ResultName(result.Outcome), row.Detail};
                const int index = static_cast<int>(rows_.size());
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = index;
                item.pszText = cells[0].data();
                ListView_InsertItem(results_, &item);
                for (int column = 1; column < 4; ++column) {
                    ListView_SetItemText(results_, index, column, cells[column].data());
                }
                rows_.push_back(std::move(row));
            }
        }
        SendMessageW(results_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(results_, nullptr, TRUE);
    }

    void ExportReport() {
        ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            return;
        }
        dialog->SetTitle(L"Save analysis and repair report");
        const COMDLG_FILTERSPEC filter{L"JSON report (*.json)", L"*.json"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetDefaultExtension(L"json");
        dialog->SetFileName(L"deadlock-mod-report.json");
        ComPtr<IShellItem> item;
        if (FAILED(dialog->Show(hwnd_)) || FAILED(dialog->GetResult(&item))) {
            return;
        }
        const auto path = ItemPath(item.Get());
        if (!path) {
            return;
        }
        try {
            const std::string full = ReportExporter::Write(Narrow(*path), Narrow(GetText(game_)), reports_);
            SetStatus(L"Saved report: " + Widen(full));
        } catch (const std::exception& ex) {
            SetStatus(L"Could not save report: " + Widen(ex.what()));
        }
    }

    HWND hwnd_ = nullptr;
    HWND game_label_ = nullptr;
    HWND game_ = nullptr;
    HWND game_browse_ = nullptr;
    HWND mods_label_ = nullptr;
    HWND mods_ = nullptr;
    HWND mods_browse_ = nullptr;
    HWND hint_ = nullptr;
    HWND analyze_ = nullptr;
    HWND repair_ = nullptr;
    HWND export_ = nullptr;
    HWND cancel_ = nullptr;
    HWND progress_ = nullptr;
    HWND status_ = nullptr;
    HWND results_ = nullptr;
    HWND log_ = nullptr;
    HWND credit_ = nullptr;
    HFONT font_ = nullptr;
    HFONT mono_font_ = nullptr;
    UINT dpi_ = 96;

    std::vector<std::string> mods_list_;
    std::vector<ArchiveReport> reports_;
    std::vector<ResultRow> rows_;
    std::optional<std::string> analyzed_game_;
    std::string running_game_;
    bool running_ = false;
    std::optional<CancellationSource> cancellation_;
    std::thread worker_;
    std::mutex events_mutex_;
    std::deque<WorkerEvent> events_;
};

}  // namespace

int RunMainWindow(const std::vector<std::string>& initial_mods) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof(controls),
        ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);
    int exit_code = 0;
    {
        MainWindow window;
        exit_code = window.Run(initial_mods);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return exit_code;
}

}  // namespace skeleton_repair
