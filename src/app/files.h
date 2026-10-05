#pragma once
#include "app/jobs.h"

namespace watermark::app {
std::string display(const std::filesystem::path& path);
std::string mark_json(const Payload& payload, Strength strength);
Payload read_mark(const std::filesystem::path& path);
std::string hash_text(std::uint64_t hash);
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);
std::vector<std::filesystem::path> input_files(const std::filesystem::path& input, bool recursive,
                                             const std::filesystem::path& excluded, const Progress& progress,
                                             const std::function<bool()>& cancelled = {});
std::string csv(const std::string& value);
void write_text(const std::filesystem::path& path, const std::string& text, bool replace);
void write_image(const Image& image, const std::filesystem::path& path, ImageFormat format, int quality, bool replace);
}
