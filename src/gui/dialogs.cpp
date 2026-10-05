#include "gui/dialogs.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <stdexcept>

namespace watermark::gui {
using Microsoft::WRL::ComPtr;
std::filesystem::path pick_path(HWND owner, Picker picker) {
    const bool folder = picker == Picker::folder, save = picker == Picker::report;
    ComPtr<IFileDialog> dialog;
    HRESULT result;
    if (save) {
        ComPtr<IFileSaveDialog> save_dialog;
        result = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&save_dialog));
        if (SUCCEEDED(result)) { dialog = save_dialog; }
    } else {
        ComPtr<IFileOpenDialog> open_dialog;
        result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&open_dialog));
        if (SUCCEEDED(result)) { dialog = open_dialog; }
    }
    if (FAILED(result)) { throw std::runtime_error("Cannot create the Windows file picker."); }
    DWORD flags = 0;
    result = dialog->GetOptions(&flags);
    if (SUCCEEDED(result)) {
        flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (folder) { flags |= FOS_PICKFOLDERS; }
        result = dialog->SetOptions(flags);
    }
    if (FAILED(result)) { throw std::runtime_error("Cannot configure the file picker."); }
    const COMDLG_FILTERSPEC csv_filters[] = {{L"CSV report", L"*.csv"}};
    const COMDLG_FILTERSPEC json_filters[] = {{L"Watermark manifest", L"*.json"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC key_filters[] = {{L"UTF-8 key file", L"*.txt"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC image_filters[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp"}, {L"All files", L"*.*"}};
    if (!folder) {
        const auto* filters = save ? csv_filters : picker == Picker::manifest ? json_filters :
                              picker == Picker::key ? key_filters : image_filters;
        result = dialog->SetFileTypes(save ? 1 : 2, filters);
        if (FAILED(result)) { throw std::runtime_error("Cannot set file picker filters."); }
        if (save) {
            dialog->SetDefaultExtension(L"csv");
            dialog->SetFileName(L"report.csv");
        }
    }
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { return {}; }
    if (FAILED(result)) { throw std::runtime_error("File picker failed."); }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) { throw std::runtime_error("Cannot read selected path."); }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { throw std::runtime_error("Cannot read selected filename."); }
    const std::filesystem::path selected(path);
    CoTaskMemFree(path);
    return selected;
}
} // namespace watermark::gui
