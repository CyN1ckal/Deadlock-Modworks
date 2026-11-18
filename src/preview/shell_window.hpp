#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

struct HWND__;
struct HINSTANCE__;

namespace skeleton_repair {

// The model tools' top-level Win32 window: its class, icons and saved placement, and the messages the frame loop
// reads (dropped files, resizes, quit). The placement is kept in its own file, so resetting the ImGui layout does not
// move the window. Everything it creates is released with it.
class ShellWindow {
public:
    // Creates the window, hidden, at the placement saved in `placement_file`. Throws when it cannot be created.
    explicit ShellWindow(const std::filesystem::path& placement_file);
    ~ShellWindow();
    ShellWindow(const ShellWindow&) = delete;
    ShellWindow& operator=(const ShellWindow&) = delete;

    HWND__* Handle() const;
    HINSTANCE__* Instance() const;
    // Shows the window, maximized when it was saved that way.
    void Show();
    // Dispatches every waiting message. False once the window has been closed.
    bool PumpMessages();
    bool Minimized() const;
    std::pair<int, int> ClientSize() const;
    // Files dropped on the window since the last call.
    std::vector<std::filesystem::path> TakeDropped();
    // True once after the client area changed size.
    bool TakeResized();

    // What the window procedure updates. Defined in shell_window.cpp.
    struct State;

private:
    std::unique_ptr<State> state_;
};

// Win32 open dialogs owned by `owner`. Empty when the user cancels.
std::vector<std::filesystem::path> AskForFiles(HWND__* owner, const wchar_t* filter, const wchar_t* title, bool multiple = false);
std::optional<std::filesystem::path> AskForFolder(HWND__* owner, const wchar_t* title);

}  // namespace skeleton_repair
