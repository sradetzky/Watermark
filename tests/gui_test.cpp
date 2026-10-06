#include "app/jobs.h"
#include "gui/controls.h"
#include <Windows.h>
#include <commctrl.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace wm = watermark;
namespace ids = watermark::gui;
namespace {
void require(bool condition, const char* name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
    if (!condition) { throw std::runtime_error(name); }
}
template<class Condition>
void until(Condition condition, DWORD timeout = 60000) {
    const auto start = GetTickCount64();
    while (!condition()) {
        if (GetTickCount64() - start > timeout) { throw std::runtime_error("GUI test timed out."); }
        Sleep(20);
    }
}
class GuiProcess {
public:
    GuiProcess(const fs::path& exe, const fs::path& directory) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process_)) {
            throw std::runtime_error("Cannot launch GUI test process.");
        }
        CloseHandle(process_.hThread);
        until([&] {
            EnumWindows([](HWND window, LPARAM context) -> BOOL {
                auto& process = *reinterpret_cast<GuiProcess*>(context);
                DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                wchar_t name[128]{}; GetClassNameW(window, name, 128);
                if (pid == process.process_.dwProcessId && std::wstring(name) == L"WatermarkWindow") {
                    process.window = window; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            return window != nullptr;
        }, 10000);
        if (WaitForInputIdle(process_.hProcess, 10000) != 0) { throw std::runtime_error("GUI did not finish initializing."); }
        until([&] { return GetDlgItem(window, ids::run) != nullptr; }, 10000);
    }
    ~GuiProcess() {
        if (window && IsWindow(window)) { PostMessageW(window, WM_CLOSE, 0, 0); }
        if (WaitForSingleObject(process_.hProcess, 10000) != WAIT_OBJECT_0) { TerminateProcess(process_.hProcess, 1); }
        CloseHandle(process_.hProcess);
    }
    HWND control(int id) const { return GetDlgItem(window, id); }
    void set(int id, const std::wstring& value) { SendMessageW(control(id), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())); }
    std::wstring text(int id) const {
        const auto length = SendMessageW(control(id), WM_GETTEXTLENGTH, 0, 0);
        std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
        SendMessageW(control(id), WM_GETTEXT, result.size(), reinterpret_cast<LPARAM>(result.data()));
        result.resize(length); return result;
    }
    void click(int id) { SendMessageW(control(id), BM_CLICK, 0, 0); }
    void selection(int id, int value) {
        SendMessageW(control(id), CB_SETCURSEL, value, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(control(id)));
    }
    void done() { until([&] { return IsWindowEnabled(control(ids::run)); }); }
    void detect_page() {
        SendMessageW(control(ids::tabs), WM_KEYDOWN, VK_RIGHT, 0);
        until([&] { return text(ids::run) == L"Detect"; }, 10000);
    }
    bool exited() const { return WaitForSingleObject(process_.hProcess, 10000) == WAIT_OBJECT_0; }
    void screenshot(const fs::path& path) {
        // Paint only this test-owned window outside the visible desktop, without activation.
        SetWindowPos(window, nullptr, -10000, -10000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        RECT bounds{}; GetClientRect(window, &bounds);
        HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = bounds.right;
        info.bmiHeader.biHeight = -bounds.bottom; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !memory) { throw std::runtime_error("Cannot capture GUI test window."); }
        HGDIOBJ previous = SelectObject(memory, bitmap);
        FillRect(memory, &bounds, GetSysColorBrush(COLOR_WINDOW));
        const BOOL printed = PrintWindow(window, memory, PW_CLIENTONLY | 2);
        ShowWindow(window, SW_HIDE);
        wm::Image image(bounds.right, bounds.bottom);
        const auto* bytes = static_cast<const unsigned char*>(pixels);
        for (std::size_t i = 0; i < image.pixels.size(); ++i) { image.pixels[i] = {bytes[i * 4 + 2], bytes[i * 4 + 1], bytes[i * 4], 255}; }
        SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
        bool rendered = false;
        for (const auto& pixel : image.pixels) {
            if (pixel.r < 100 && pixel.g < 100 && pixel.b < 100) { rendered = true; break; }
        }
        require(printed && rendered, "GUI render capture includes controls"); wm::save_image(image, path);
    }
    HWND window = nullptr;
private:
    PROCESS_INFORMATION process_{};
};
class Fixtures {
public:
    Fixtures() {
        path = fs::absolute(fs::current_path()) / ("gui-test-" + std::to_string(GetCurrentProcessId()));
        if (!fs::create_directory(path)) { throw std::runtime_error("Cannot create GUI test directory."); }
        wm::Image host(512, 512), source(128, 128);
        for (int y = 0; y < 512; ++y) {
            for (int x = 0; x < 512; ++x) {
                const auto value = static_cast<std::uint8_t>(128 + 30 * std::sin(x * 0.13) + 25 * std::cos(y * 0.11) + ((x * 137 + y * 197) % 13));
                host.at(x, y) = {value, value, value, 255};
            }
        }
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 128; ++x) { source.at(x, y) = {static_cast<std::uint8_t>((x * y + x) % 256), 100, 140, 255}; }
        }
        wm::save_image(host, path / "host.png"); wm::save_image(source, path / "source.png");
        wm::Image silhouette(64, 96);
        for (auto& pixel : silhouette.pixels) { pixel = {0, 0, 0, 0}; }
        for (int y = 8; y < 88; ++y) {
            for (int x = 8; x < 56; ++x) { silhouette.at(x, y).a = 255; }
        }
        wm::save_image(silhouette, path / "silhouette.png");
    }
    ~Fixtures() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};
}
int main(int argc, char** argv) {
    const bool capture = argc == 2 && std::string(argv[1]) == "--capture";
    if (argc > 1 && !capture) { std::cerr << "Usage: watermark_gui_test [--capture]\n"; return 2; }
    try {
        Fixtures fixtures;
        const auto& root = fixtures.path;
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        GuiProcess gui(fs::path(executable).parent_path() / "wmgui.exe", root);
        require(!IsWindowEnabled(gui.control(ids::key)) && !(GetWindowLongW(gui.control(ids::key), GWL_STYLE) & WS_VISIBLE),
                "GUI defaults to watermark identity without showing a secret field");
        gui.selection(ids::key_kind, 0);
        require(SendMessageW(gui.control(ids::key), EM_GETPASSWORDCHAR, 0, 0) != 0, "Passphrase is masked");
        gui.set(ids::identity, (root / "silhouette.png").wstring()); gui.set(ids::input, (root / "host.png").wstring());
        gui.set(ids::output, (root / "stamped").wstring()); gui.set(ids::key, L"GUI secret");
        require(!IsWindowEnabled(gui.control(ids::visible_position)), "Visible controls disabled by default");
        gui.click(ids::visible);
        require(IsWindowEnabled(gui.control(ids::visible_position)), "Visibility checkbox enables silhouette settings");
        require(gui.text(ids::visible_opacity) == L"50" && SendMessageW(gui.control(ids::visible_ink), CB_GETCURSEL, 0, 0) == 0,
                "GUI defaults to black at 50 percent opacity");
        gui.selection(ids::visible_position, 1); gui.set(ids::visible_size, L"20"); gui.set(ids::visible_opacity, L"50");
        gui.selection(ids::key_kind, 2);
        gui.set(ids::output, (root / "source-key").wstring());
        if (capture) { gui.screenshot(fs::absolute("wmgui-embed.png")); }
        gui.click(ids::run); gui.done();
        require(gui.text(ids::status).rfind(L"Completed", 0) == 0 && fs::exists(root / "source-key" / "host.png.png"),
                "GUI embeds visible mark without a passphrase");
        require(wm::load_image(root / "source-key" / "host.png.png").at(40, 460).r <
                wm::load_image(root / "host.png").at(40, 460).r - 10, "GUI applies translucent black silhouette");
        gui.detect_page();
        gui.set(ids::input, (root / "source-key" / "host.png.png").wstring());
        fs::rename(root / "host.png", root / "unavailable-original.png");
        fs::rename(root / "source-key" / "mark.json", root / "unavailable-manifest.json");
        gui.click(ids::run); gui.done();
        require(gui.text(ids::log).find(L"match=true") != std::wstring::npos,
                "GUI detects with only stamped image and watermark artwork, without original photo or manifest");
        if (capture) { gui.screenshot(fs::absolute("wmgui-detect.png")); }
        gui.set(ids::identity, (root / "source.png").wstring()); gui.click(ids::run); gui.done();
        require(gui.text(ids::log).find(L"absent ") != std::wstring::npos && gui.text(ids::log).find(L"match=true") == std::wstring::npos,
                "GUI rejects a different watermark artwork");
        gui.set(ids::identity, (root / "silhouette.png").wstring()); gui.set(ids::input, (root / "unavailable-original.png").wstring());
        gui.click(ids::run); gui.done();
        require(gui.text(ids::log).find(L"absent ") != std::wstring::npos, "GUI rejects an unmarked photo");
        fs::rename(root / "unavailable-original.png", root / "host.png");
        fs::rename(root / "unavailable-manifest.json", root / "source-key" / "mark.json");
        gui.selection(ids::identity_kind, 1);
        gui.set(ids::identity, (root / "source-key" / "mark.json").wstring()); gui.set(ids::input, (root / "source-key").wstring());
        gui.click(ids::run); gui.done();
        require(gui.text(ids::log).find(L"match=true") != std::wstring::npos, "GUI detects source-key manifest without a passphrase");
        SendMessageW(gui.control(ids::tabs), WM_KEYDOWN, VK_LEFT, 0);
        until([&] { return gui.text(ids::run) == L"Embed"; }, 10000);
        gui.set(ids::identity, (root / "silhouette.png").wstring()); gui.set(ids::input, (root / "host.png").wstring());
        gui.set(ids::output, (root / "stamped").wstring()); gui.selection(ids::key_kind, 0); gui.set(ids::key, L"GUI secret");
        gui.selection(ids::visible_ink, 1);
        gui.set(ids::visible_opacity, L"75");
        gui.click(ids::run);
        require(!IsWindowEnabled(gui.control(ids::run)) && IsWindowEnabled(gui.control(ids::cancel)), "Worker disables editing and enables cancel");
        gui.done();
        require(gui.text(ids::status).rfind(L"Completed", 0) == 0 && fs::exists(root / "stamped" / "host.png.png"), "GUI embeds an image");
        const auto payload = wm::payload_from_source(wm::load_image(root / "silhouette.png"));
        require(wm::detect(wm::load_image(root / "stamped" / "host.png.png"), payload, {"GUI secret"}).hash_matches == true,
                "GUI output contains expected mark");
        require(wm::load_image(root / "stamped" / "host.png.png").at(40, 460).r >
                wm::load_image(root / "host.png").at(40, 460).r + 40, "GUI places visible silhouette at selected bottom-left position");
        gui.detect_page(); gui.selection(ids::identity_kind, 1);
        gui.set(ids::identity, (root / "stamped" / "mark.json").wstring()); gui.set(ids::input, (root / "stamped").wstring());
        gui.click(ids::run); gui.done();
        require(gui.text(ids::status).rfind(L"Completed", 0) == 0 &&
                SendMessageW(gui.control(ids::results), LVM_GETITEMCOUNT, 0, 0) == 1 && IsWindowEnabled(gui.control(ids::export_csv)),
                "GUI detects via manifest and enables CSV export");
        require(gui.text(ids::log).find(L"match=true") != std::wstring::npos, "GUI log shows source match");
        require(!fs::exists(root / "report.csv"), "GUI report export is explicit");
        wm::app::Job job;
        job.embedding = false; job.automatic_report = false; job.mark = root / "stamped" / "mark.json";
        job.input = root / "stamped"; job.parameters.passphrase = "GUI secret";
        const auto result = wm::app::run_job(job);
        wm::app::export_report(job, root / "export.csv", result.csv);
        require(fs::exists(root / "export.csv") && result.csv.find(",present,1.000000,") != std::string::npos, "Shared GUI CSV export");
        {
            std::ofstream key_file(root / "key.txt", std::ios::binary);
            key_file << "\xEF\xBB\xBF" "GUI secret\r\n";
        }
        gui.selection(ids::key_kind, 1);
        require(gui.text(ids::key).empty() && SendMessageW(gui.control(ids::key), EM_GETPASSWORDCHAR, 0, 0) == 0,
                "Key mode clears the secret before displaying paths");
        gui.set(ids::key, (root / "key.txt").wstring()); gui.click(ids::run); gui.done();
        require(gui.text(ids::log).find(L"match=true") != std::wstring::npos, "GUI uses UTF-8 key file");
        gui.set(ids::identity, (root / "missing.json").wstring()); gui.click(ids::run); gui.done();
        require(gui.text(ids::status).rfind(L"Failed", 0) == 0 && !IsWindowEnabled(gui.control(ids::export_csv)),
                "GUI failure restores controls and prevents stale export");
        gui.set(ids::identity, (root / "stamped" / "mark.json").wstring());
        gui.selection(ids::key_kind, 0);
        gui.set(ids::key, L"wrong key"); gui.click(ids::run); gui.click(ids::cancel); gui.done();
        require(gui.text(ids::status).rfind(L"Cancelled", 0) == 0 && IsWindowEnabled(gui.control(ids::run)), "GUI cancels and restores controls");
        gui.click(ids::run); SendMessageW(gui.window, WM_CLOSE, 0, 0);
        require(gui.exited(), "GUI cancels worker before closing");
    } catch (const std::exception& error) {
        std::cerr << "GUI test error: " << error.what() << '\n'; return 1;
    }
    return 0;
}
