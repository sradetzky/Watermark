#pragma once
#include <Windows.h>
#include <filesystem>

namespace watermark::gui {
enum class Picker { image, manifest, key, folder, report };
std::filesystem::path pick_path(HWND owner, Picker picker);
}
