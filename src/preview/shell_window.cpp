#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "shell_window.hpp"

#include "product_version.hpp"

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <cwchar>
#include <fstream>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace skeleton_repair {
namespace {

constexpr const wchar_t* kWindowClass = L"DeadlockModelPreview";
constexpr int kMinWidth = 640;
constexpr int kMinHeight = 480;

struct Placement {
    int X = 100;
    int Y = 100;
    int Width = 1440;
    int Height = 960;
    bool Maximized = false;
};

RECT PrimaryWorkArea() {
    RECT work{0, 0, 0, 0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

// `screen` is in screen coordinates. A sliver on a monitor counts as visible.
bool MeetsAMonitor(RECT screen) {
    struct Probe {
        RECT Target;
        bool Hit = false;
    } probe{screen};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT monitor, LPARAM param) -> BOOL {
        auto* probe = reinterpret_cast<Probe*>(param);
        RECT overlap{};
        if (IntersectRect(&overlap, &probe->Target, monitor) && overlap.right - overlap.left >= 80 && overlap.bottom - overlap.top >= 48) {
            probe->Hit = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&probe));
    return probe.Hit;
}

// The saved placement, relative to the primary work area. A missing, damaged, tiny or off-screen one gives the default.
Placement LoadPlacement(const std::filesystem::path& path) {
    Placement place;
    std::ifstream in(path);
    int maximized = 0;
    if (!in || !(in >> place.X >> place.Y >> place.Width >> place.Height >> maximized)) return {};
    if (place.Width < kMinWidth || place.Height < kMinHeight) return {};
    const RECT work = PrimaryWorkArea();
    const RECT screen{place.X + work.left, place.Y + work.top, place.X + work.left + place.Width, place.Y + work.top + place.Height};
    if (!MeetsAMonitor(screen)) return {};
    place.Maximized = maximized != 0;
    return place;
}

HICON LoadAppIcon(HINSTANCE instance, int width_metric, int height_metric) {
    // Resource 1 is the ICON in cmake/app.rc.in. Each executable that opens this window embeds that ICO, and
    // LoadImage selects the frame for this size.
    return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(width_metric),
        GetSystemMetrics(height_metric), LR_DEFAULTCOLOR));
}

}  // namespace

struct ShellWindow::State {
    std::filesystem::path PlacementFile;
    HINSTANCE Instance = nullptr;
    HICON Icon = nullptr;
    HICON IconSmall = nullptr;
    HBRUSH Background = nullptr;
    bool Registered = false;
    HWND Window = nullptr;
    bool Maximized = false;
    std::vector<std::filesystem::path> Dropped;
    bool Resized = false;

    void SavePlacement() const {
        if (PlacementFile.empty() || Window == nullptr) return;
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(Window, &placement)) return;
        const int width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
        const int height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
        if (width < kMinWidth || height < kMinHeight) return;
        std::ofstream out(PlacementFile, std::ios::trunc);
        if (!out) return;
        out << placement.rcNormalPosition.left << ' ' << placement.rcNormalPosition.top << ' ' << width << ' ' << height << ' '
            << (placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0) << '\n';
    }

    void Release() {
        if (Window != nullptr) DestroyWindow(Window);
        if (Registered) UnregisterClassW(kWindowClass, Instance);
        if (Background != nullptr) DeleteObject(Background);
        if (Icon != nullptr) DestroyIcon(Icon);
        if (IconSmall != nullptr) DestroyIcon(IconSmall);
        Window = nullptr;
        Registered = false;
        Background = nullptr;
        Icon = IconSmall = nullptr;
    }

    static LRESULT WINAPI Procedure(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            auto* state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            state->Window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
        // Messages such as WM_GETMINMAXINFO arrive before WM_NCCREATE, with no state yet.
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && state != nullptr; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                state->Dropped.emplace_back(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {1000, 680};
            return 0;
        case WM_SIZE:
            if (state != nullptr && wparam != SIZE_MINIMIZED) state->Resized = true;
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            if (state != nullptr) state->SavePlacement();
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) state->Window = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
};

ShellWindow::ShellWindow(const std::filesystem::path& placement_file) : state_(std::make_unique<State>()) {
    State& state = *state_;
    state.PlacementFile = placement_file;
    try {
        const Placement placement = LoadPlacement(placement_file);
        const RECT work = PrimaryWorkArea();
        ImGui_ImplWin32_EnableDpiAwareness();
        state.Instance = GetModuleHandleW(nullptr);
        state.Icon = LoadAppIcon(state.Instance, SM_CXICON, SM_CYICON);
        state.IconSmall = LoadAppIcon(state.Instance, SM_CXSMICON, SM_CYSMICON);
        // Matches the workspace clear so a resize does not flash white or the old grey.
        state.Background = CreateSolidBrush(RGB(0x14, 0x1B, 0x23));
        WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, &State::Procedure, 0, 0, state.Instance, state.Icon, nullptr, state.Background,
            nullptr, kWindowClass, state.IconSmall};
        state.Registered = RegisterClassExW(&wc) != 0;
        CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, SR_PRODUCT_TITLE_W, WS_OVERLAPPEDWINDOW, placement.X + work.left,
            placement.Y + work.top, placement.Width, placement.Height, nullptr, nullptr, state.Instance, &state);
        if (state.Window == nullptr) throw std::runtime_error("Could not create the model tools window.");
        state.Maximized = placement.Maximized;
        SendMessageW(state.Window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(state.Icon));
        SendMessageW(state.Window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(state.IconSmall));
    } catch (...) {
        state.Release();
        throw;
    }
}

ShellWindow::~ShellWindow() {
    state_->Release();
}

HWND__* ShellWindow::Handle() const { return state_->Window; }
HINSTANCE__* ShellWindow::Instance() const { return state_->Instance; }

void ShellWindow::Show() {
    ShowWindow(state_->Window, state_->Maximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(state_->Window);
}

bool ShellWindow::PumpMessages() {
    bool open = true;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) open = false;
    }
    return open;
}

bool ShellWindow::Minimized() const {
    return state_->Window != nullptr && IsIconic(state_->Window);
}

std::pair<int, int> ShellWindow::ClientSize() const {
    RECT rect{};
    if (state_->Window != nullptr) GetClientRect(state_->Window, &rect);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

std::vector<std::filesystem::path> ShellWindow::TakeDropped() {
    return std::exchange(state_->Dropped, {});
}

bool ShellWindow::TakeResized() {
    return std::exchange(state_->Resized, false);
}

std::vector<std::filesystem::path> AskForFiles(HWND__* owner, const wchar_t* filter, const wchar_t* title, bool multiple) {
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrTitle = title;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (multiple) dialog.Flags |= OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) return {};
    // One file is a full path. Several are the folder, then each name, each ending in a null.
    const std::filesystem::path first(buffer.data());
    const wchar_t* next = buffer.data() + wcslen(buffer.data()) + 1;
    if (*next == L'\0') return {first};
    std::vector<std::filesystem::path> files;
    for (; *next != L'\0'; next += wcslen(next) + 1) files.push_back(first / next);
    return files;
}

std::optional<std::filesystem::path> AskForFolder(HWND__* owner, const wchar_t* title) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) return std::nullopt;
    wchar_t buffer[MAX_PATH] = {};
    const BOOL found = SHGetPathFromIDListW(item, buffer);
    ILFree(item);
    if (!found || buffer[0] == L'\0') return std::nullopt;
    return std::filesystem::path(buffer);
}

}  // namespace skeleton_repair
