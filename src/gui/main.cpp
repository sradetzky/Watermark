#include "gui/controls.h"
#include "gui/dialogs.h"
#include "app/jobs.h"

#include <Windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace watermark::gui {
namespace {
constexpr UINT event_message = WM_APP + 1, done_message = WM_APP + 2;
struct Completion { app::Summary summary; std::string error; };
template<class T>
void post(HWND window, UINT message, T value) {
    auto packet = std::make_unique<T>(std::move(value));
    if (PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(packet.get()))) { packet.release(); }
}
std::wstring text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}
std::wstring number(double value) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(3) << value;
    return output.str();
}
class Window {
public:
    HWND handle = nullptr;
    ~Window() { if (worker_.joinable()) { worker_.join(); } if (font_) { DeleteObject(font_); } }
    void create() {
        make(tabs, WC_TABCONTROLW, L"", 0);
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = const_cast<wchar_t*>(L"Embed");
        TabCtrl_InsertItem(control(tabs), 0, &tab);
        tab.pszText = const_cast<wchar_t*>(L"Detect");
        TabCtrl_InsertItem(control(tabs), 1, &tab);
        label(identity, L"Watermark image");
        edit(identity); combo(identity_kind, {L"Watermark image", L"mark.json"}); button(identity_browse, L"Browse...");
        label(input, L"Input image or folder");
        edit(input); button(input_browse, L"File..."); button(input_folder, L"Folder...");
        label(key, L"Detection key");
        edit(key, ES_PASSWORD); combo(key_kind, {L"Passphrase", L"Key file (UTF-8)", L"Watermark identity"}, 2); button(key_browse, L"Browse...");
        label(output, L"Output folder");
        edit(output); button(output_browse, L"Browse...");
        label(strength, L"Strength"); combo(strength, {L"Low", L"Default", L"High"}, 1);
        label(format, L"Format"); combo(format, {L"PNG", L"JPEG"});
        label(quality, L"JPEG quality"); edit(quality, ES_NUMBER); SetWindowTextW(control(quality), L"90");
        make(recursive, L"BUTTON", L"Include subfolders", BS_AUTOCHECKBOX);
        make(force, L"BUTTON", L"Force rewrite / replace outputs", BS_AUTOCHECKBOX);
        make(visible, L"BUTTON", L"Visible watermark", BS_AUTOCHECKBOX);
        label(visible_position, L"Position");
        combo(visible_position, {L"Bottom right", L"Bottom left", L"Top right", L"Top left", L"Center"});
        label(visible_size, L"Size %"); edit(visible_size, ES_NUMBER); SetWindowTextW(control(visible_size), L"15");
        label(visible_opacity, L"Opacity %"); edit(visible_opacity, ES_NUMBER); SetWindowTextW(control(visible_opacity), L"50");
        label(visible_ink, L"Color"); combo(visible_ink, {L"Black", L"White"});
        button(run, L"Embed", BS_DEFPUSHBUTTON); button(cancel, L"Cancel"); button(export_csv, L"Export CSV...");
        make(progress, PROGRESS_CLASSW, L"", 0);
        make(status, L"STATIC", L"Ready", SS_LEFT, false);
        make(results, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, true, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(control(results), LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* columns[] = {L"File", L"Verdict", L"Bit accuracy", L"Confidence", L"Watermark match", L"Scale", L"Crop phase"};
        for (int i = 0; i < 7; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(columns[i]); column.cx = i == 0 ? 280 : 90;
            ListView_InsertColumn(control(results), i, &column);
        }
        make(log, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
             true, WS_EX_CLIENTEDGE);
        SendMessageW(control(log), EM_SETLIMITTEXT, 1024 * 1024, 0);
        dpi_ = GetDpiForWindow(handle);
        set_font(); refresh(); layout();
    }
    void command(int id, int notification) {
        if (id == cancel) { cancelled_.store(true); SetWindowTextW(control(status), L"Cancelling..."); return; }
        if (id == run) { start(); return; }
        if (id == export_csv) {
            const auto path = pick_path(handle, Picker::report);
            if (!path.empty()) { app::export_report(last_job_, path, last_csv_); append(L"Report saved: " + path.wstring()); }
            return;
        }
        if (id == visible) { refresh(); return; }
        if (id == key_kind && notification == CBN_SELCHANGE) {
            SetWindowTextW(control(key), L"");
            SendMessageW(control(key), EM_SETPASSWORDCHAR, selection(key_kind) == 0 ? L'\x25cf' : 0, 0);
            InvalidateRect(control(key), nullptr, TRUE);
            refresh(); return;
        }
        if ((id == format || id == identity_kind) && notification == CBN_SELCHANGE) {
            if (id == identity_kind) { SetWindowTextW(control(identity), L""); }
            refresh(); return;
        }
        int field = 0;
        bool folder = false;
        if (id == identity_browse) { field = identity; }
        else if (id == input_browse || id == input_folder) { field = input; folder = id == input_folder; }
        else if (id == output_browse) { field = output; folder = true; }
        else if (id == key_browse) { field = key; }
        if (field) {
            const auto picker = folder ? Picker::folder : field == key ? Picker::key :
                                field == identity && selection(identity_kind) == 1 ? Picker::manifest : Picker::image;
            const auto path = pick_path(handle, picker);
            if (!path.empty()) { SetWindowTextW(control(field), path.c_str()); }
        }
    }
    void change_page() {
        if (embedding() && selection(identity_kind) == 1) {
            SendMessageW(control(identity_kind), CB_SETCURSEL, 0, 0); SetWindowTextW(control(identity), L"");
        }
        refresh(); layout();
    }
    void receive(const app::Event& event) {
        append(app::wide(event.message));
        SendMessageW(control(progress), PBM_SETRANGE32, 0, static_cast<LPARAM>(event.total));
        SendMessageW(control(progress), PBM_SETPOS, event.completed, 0);
        const auto progress_text = std::to_wstring(event.completed) + L" / " + std::to_wstring(event.total) +
                                   L"  " + event.file.filename().wstring();
        SetWindowTextW(control(status), progress_text.c_str());
        if (event.detection || (!last_job_.embedding && event.error)) { result_row(event); }
    }
    void finish(const Completion& completion) {
        if (worker_.joinable()) { worker_.join(); }
        running_ = false;
        last_csv_ = completion.summary.csv;
        std::wstring message;
        if (!completion.error.empty()) { message = L"Failed: " + app::wide(completion.error); last_csv_.clear(); }
        else {
            const auto& summary = completion.summary;
            message = summary.cancelled ? L"Cancelled" : summary.errors ? L"Completed with errors" : L"Completed";
            message += L" — " + std::to_wstring(summary.completed) + L" / " + std::to_wstring(summary.total) +
                       L" files, " + std::to_wstring(summary.errors) + L" errors";
            if (last_job_.embedding) {
                message += L", " + std::to_wstring(summary.embedded) + L" embedded, " + std::to_wstring(summary.skipped) + L" skipped";
            }
        }
        SetWindowTextW(control(status), message.c_str()); append(message); refresh();
        if (closing_) { DestroyWindow(handle); }
    }
    void close() {
        if (running_) { closing_ = true; cancelled_.store(true); SetWindowTextW(control(status), L"Cancelling before closing..."); }
        else { DestroyWindow(handle); }
    }
    void error(const std::string& message) {
        const auto value = L"Error: " + app::wide(message);
        SetWindowTextW(control(status), value.c_str()); append(value);
    }
    void dpi_changed(UINT dpi, const RECT& rectangle) {
        dpi_ = dpi; set_font();
        SetWindowPos(handle, nullptr, rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                     rectangle.bottom - rectangle.top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
    }
    int scaled(int value) const { return MulDiv(value, dpi_, 96); }
    void layout() {
        RECT client{}; GetClientRect(handle, &client);
        const int width = MulDiv(client.right, 96, dpi_), height = MulDiv(client.bottom, 96, dpi_);
        move(tabs, 16, 12, width - 32, 36);
        for (int id : {identity, input, key, output}) {
            const int y = id == identity ? 60 : id == input ? 113 : id == key ? 166 : 219;
            MoveWindow(labels_.at(id), scaled(20), scaled(y), scaled(id == key ? width - 40 : 260), scaled(20), TRUE);
            const int reserve = id == input ? 176 : id == key || id == identity ? 250 : 90;
            move(id, 20, y + 21, width - 40 - reserve, 26);
            if (id == identity) { move(identity_kind, width - 260, y + 21, 145, 180); move(identity_browse, width - 105, y + 21, 85, 26); }
            if (id == input) { move(input_browse, width - 186, y + 21, 78, 26); move(input_folder, width - 100, y + 21, 80, 26); }
            if (id == key) { move(key_kind, selection(key_kind) == 2 ? 20 : width - 260, y + 21, selection(key_kind) == 2 ? 160 : 145, 180); move(key_browse, width - 105, y + 21, 85, 26); }
            if (id == output) { move(output_browse, width - 105, y + 21, 85, 26); }
        }
        for (const auto pair : {std::pair{strength, 20}, std::pair{format, 180}, std::pair{quality, 320}}) {
            MoveWindow(labels_.at(pair.first), scaled(pair.second), scaled(272), scaled(130), scaled(18), TRUE);
            move(pair.first, pair.second, 292, pair.first == quality ? 90 : 130, pair.first == quality ? 26 : 180);
        }
        move(recursive, embedding() ? 445 : 20, embedding() ? 272 : 219, 200, 24); move(force, 445, 297, 310, 24);
        move(visible, 20, 333, 180, 26);
        MoveWindow(labels_.at(visible_position), scaled(210), scaled(337), scaled(65), scaled(20), TRUE);
        move(visible_position, 280, 333, 140, 180);
        MoveWindow(labels_.at(visible_size), scaled(440), scaled(337), scaled(60), scaled(20), TRUE);
        move(visible_size, 505, 333, 60, 26);
        MoveWindow(labels_.at(visible_opacity), scaled(585), scaled(337), scaled(75), scaled(20), TRUE);
        move(visible_opacity, 665, 333, 60, 26);
        MoveWindow(labels_.at(visible_ink), scaled(20), scaled(374), scaled(50), scaled(20), TRUE);
        move(visible_ink, 75, 370, 130, 180);
        const int run_y = embedding() ? 415 : 260;
        move(run, 20, run_y, 110, 30); move(cancel, 140, run_y, 90, 30); move(export_csv, width - 145, run_y, 125, 30);
        move(progress, 245, run_y + 5, width - 410, 20); move(status, 20, run_y + 40, width - 40, 24);
        const int results_y = run_y + 72, result_height = std::max(90, height - results_y - 160);
        move(results, 20, results_y, width - 40, result_height);
        const int log_y = embedding() ? results_y : results_y + result_height + 10;
        move(log, 20, log_y, width - 40, std::max(80, height - log_y - 20));
        const int columns[] = {std::max(200, width - 625), 85, 95, 95, 130, 75, 100};
        for (int i = 0; i < 7; ++i) { ListView_SetColumnWidth(control(results), i, scaled(columns[i])); }
    }
private:
    std::map<int, HWND> controls_, labels_;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::thread worker_;
    std::atomic_bool cancelled_{false};
    bool running_ = false, closing_ = false;
    app::Job last_job_;
    std::string last_csv_;
    HWND control(int id) const { return controls_.at(id); }
    bool embedding() const { return TabCtrl_GetCurSel(control(tabs)) == 0; }
    int selection(int id) const { return static_cast<int>(SendMessageW(control(id), CB_GETCURSEL, 0, 0)); }
    void make(int id, const wchar_t* type, const wchar_t* title, DWORD style, bool tab_stop = true, DWORD extended = 0) {
        HWND child = CreateWindowExW(extended, type, title, WS_CHILD | WS_VISIBLE | (tab_stop ? WS_TABSTOP : 0) | style,
                                     0, 0, 0, 0, handle, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
        if (!child) { throw std::runtime_error("Cannot create window control."); }
        controls_[id] = child;
    }
    void label(int id, const wchar_t* title) {
        HWND child = CreateWindowExW(0, L"STATIC", title, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, handle, nullptr, nullptr, nullptr);
        if (!child) { throw std::runtime_error("Cannot create field label."); }
        labels_[id] = child;
    }
    void edit(int id, DWORD style = 0) { make(id, L"EDIT", L"", ES_AUTOHSCROLL | style, true, WS_EX_CLIENTEDGE); }
    void button(int id, const wchar_t* title, DWORD style = 0) { make(id, L"BUTTON", title, BS_PUSHBUTTON | style); }
    void combo(int id, std::initializer_list<const wchar_t*> entries, int selected = 0) {
        make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
        for (const auto entry : entries) { SendMessageW(control(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry)); }
        SendMessageW(control(id), CB_SETCURSEL, selected, 0);
    }
    void move(int id, int x, int y, int width, int height) {
        MoveWindow(control(id), scaled(x), scaled(y), scaled(width), scaled(height), TRUE);
    }
    void set_font() {
        const HFONT next = CreateFontW(-scaled(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!next) { throw std::runtime_error("Cannot create UI font."); }
        for (const auto& entry : controls_) { SendMessageW(entry.second, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE); }
        for (const auto& entry : labels_) { SendMessageW(entry.second, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE); }
        if (font_) { DeleteObject(font_); } font_ = next;
    }
    void refresh() {
        const bool embed_page = embedding();
        for (const auto& entry : controls_) { EnableWindow(entry.second, !running_); }
        EnableWindow(control(log), TRUE); EnableWindow(control(results), TRUE); EnableWindow(control(status), TRUE);
        EnableWindow(control(cancel), running_ && !closing_);
        EnableWindow(control(identity_kind), !running_ && !embed_page);
        EnableWindow(control(key_browse), !running_ && selection(key_kind) == 1);
        EnableWindow(control(key), !running_ && selection(key_kind) != 2);
        SetWindowTextW(labels_.at(key), selection(key_kind) == 2 ? L"Detection key — watermark identity; no passphrase needed" : L"Detection key");
        ShowWindow(control(key), selection(key_kind) == 2 ? SW_HIDE : SW_SHOW);
        ShowWindow(control(key_browse), selection(key_kind) == 2 ? SW_HIDE : SW_SHOW);
        EnableWindow(control(quality), !running_ && selection(format) == 1);
        EnableWindow(control(export_csv), !running_ && !last_job_.embedding && !last_csv_.empty());
        const bool show_visible = SendMessageW(control(visible), BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (int id : {visible_position, visible_size, visible_opacity, visible_ink}) {
            EnableWindow(control(id), !running_ && show_visible);
        }
        ShowWindow(control(export_csv), embed_page ? SW_HIDE : SW_SHOW);
        ShowWindow(control(results), embed_page ? SW_HIDE : SW_SHOW);
        for (int id : {output, output_browse, strength, format, quality, force, visible,
                       visible_position, visible_size, visible_opacity, visible_ink}) {
            ShowWindow(control(id), embed_page ? SW_SHOW : SW_HIDE);
        }
        for (int id : {output, strength, format, quality, visible_position, visible_size, visible_opacity, visible_ink}) {
            ShowWindow(labels_.at(id), embed_page ? SW_SHOW : SW_HIDE);
        }
        SetWindowTextW(labels_.at(identity), embed_page || selection(identity_kind) == 0 ? L"Watermark image" : L"Watermark manifest");
        SetWindowTextW(labels_.at(input), embed_page ? L"Image or folder to watermark" : L"Image or folder to inspect");
        SetWindowTextW(control(run), embed_page ? L"Embed" : L"Detect");
        layout();
    }
    void append(const std::wstring& message) {
        if (GetWindowTextLengthW(control(log)) > 900000) { SetWindowTextW(control(log), L"Earlier log entries omitted.\r\n"); }
        const auto line = message + L"\r\n";
        SendMessageW(control(log), EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(control(log), EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(control(log), EM_SCROLLCARET, 0, 0);
    }
    void result_row(const app::Event& event) {
        const auto filename = event.file.filename().wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT; item.iItem = ListView_GetItemCount(control(results)); item.pszText = const_cast<wchar_t*>(filename.c_str());
        const int row = ListView_InsertItem(control(results), &item);
        std::array<std::wstring, 6> values{};
        if (event.detection) {
            const auto& result = *event.detection;
            values = {app::wide(verdict_name(result.verdict)), number(result.bit_accuracy.value_or(0)), number(result.confidence),
                      result.hash_matches ? (*result.hash_matches ? L"Yes" : L"No") : L"—", number(result.scale),
                      std::to_wstring(result.offset_x) + L", " + std::to_wstring(result.offset_y)};
        } else { values[0] = L"Error"; }
        for (int i = 0; i < 6; ++i) { ListView_SetItemText(control(results), row, i + 1, values[i].data()); }
    }
    void start() {
        if (running_) { return; }
        app::Job job;
        job.embedding = embedding(); job.automatic_report = false;
        if (selection(identity_kind) == 0 || job.embedding) { job.source = text(control(identity)); }
        else { job.mark = text(control(identity)); }
        job.input = text(control(input)); job.output = text(control(output));
        job.key_from_source = selection(key_kind) == 2;
        if (selection(key_kind) == 1) { job.key_file = text(control(key)); }
        else if (!job.key_from_source) { job.parameters.passphrase = app::utf8(text(control(key))); }
        const int strength_value = selection(strength);
        job.parameters.strength = strength_value == 0 ? Strength::low : strength_value == 2 ? Strength::high : Strength::normal;
        job.format = selection(format) == 1 ? ImageFormat::jpeg : ImageFormat::png;
        if (job.embedding && job.format == ImageFormat::jpeg) {
            const auto value = text(control(quality));
            if (value.empty() || value.size() > 3 || value.find_first_not_of(L"0123456789") != std::wstring::npos) {
                throw std::invalid_argument("JPEG quality must be 1..100.");
            }
            job.jpeg_quality = std::stoi(value);
        }
        job.recursive = SendMessageW(control(recursive), BM_GETCHECK, 0, 0) == BST_CHECKED;
        job.force = SendMessageW(control(force), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (job.embedding && SendMessageW(control(visible), BM_GETCHECK, 0, 0) == BST_CHECKED) {
            job.visible_image = job.source;
            if (job.visible_image.empty()) { throw std::invalid_argument("Choose a transparent watermark PNG."); }
            job.visible.position = static_cast<Position>(selection(visible_position));
            const auto percent = [&](int id) {
                const auto value = text(control(id));
                if (value.empty() || value.size() > 3 || value.find_first_not_of(L"0123456789") != std::wstring::npos) {
                    throw std::invalid_argument("Visible size and opacity must be whole percentages.");
                }
                return std::stoi(value);
            };
            job.visible.size_percent = percent(visible_size); job.visible.opacity_percent = percent(visible_opacity);
            job.visible.ink = selection(visible_ink) == 0 ? VisibleInk::black : VisibleInk::white;
        }
        cancelled_.store(false);
        job.parameters.cancelled = [this] { return cancelled_.load(); };
        last_job_ = job; last_csv_.clear(); ListView_DeleteAllItems(control(results));
        SetWindowTextW(control(log), L""); SetWindowTextW(control(status), L"Preparing...");
        SendMessageW(control(progress), PBM_SETPOS, 0, 0);
        const HWND target = handle;
        worker_ = std::thread([job, target] {
            Completion completion;
            try { completion.summary = app::run_job(job, [target](const app::Event& event) { post(target, event_message, event); }); }
            catch (const OperationCancelled&) { completion.summary.cancelled = true; }
            catch (const std::exception& error) { completion.error = error.what(); }
            post(target, done_message, std::move(completion));
        });
        running_ = true; refresh();
    }
};
LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        window->handle = handle; SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    if (!window) { return DefWindowProcW(handle, message, wparam, lparam); }
    try {
        switch (message) {
        case WM_CREATE: window->create(); return 0;
        case WM_SIZE: window->layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {window->scaled(780), window->scaled(650)}; return 0;
        }
        case WM_DPICHANGED: window->dpi_changed(HIWORD(wparam), *reinterpret_cast<RECT*>(lparam)); return 0;
        case WM_COMMAND: window->command(LOWORD(wparam), HIWORD(wparam)); return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == tabs && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
                window->change_page();
            }
            return 0;
        case event_message: {
            std::unique_ptr<app::Event> event(reinterpret_cast<app::Event*>(lparam)); window->receive(*event); return 0;
        }
        case done_message: {
            std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(lparam)); window->finish(*completion); return 0;
        }
        case WM_CLOSE: window->close(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        if (message == WM_CREATE) { return -1; }
        window->error(error.what()); return 0;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}
} // namespace
} // namespace watermark::gui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace watermark::gui;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { return 1; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type); type.hInstance = instance; type.lpfnWndProc = window_proc;
    type.lpszClassName = L"WatermarkWindow"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    int exit_code = 1;
    if (RegisterClassExW(&type)) {
        Window window;
        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const UINT dpi = GetDpiForSystem();
        HWND handle = CreateWindowExW(0, type.lpszClassName, L"Watermark", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, std::min(MulDiv(920, dpi, 96), static_cast<int>(work.right - work.left)),
            std::min(MulDiv(760, dpi, 96), static_cast<int>(work.bottom - work.top)), nullptr, nullptr, instance, &window);
        if (handle) {
            ShowWindow(handle, show); UpdateWindow(handle);
            MSG message{};
            BOOL state;
            while ((state = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (!IsDialogMessageW(handle, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            exit_code = state == 0 ? static_cast<int>(message.wParam) : 1;
        }
    }
    CoUninitialize();
    return exit_code;
}
