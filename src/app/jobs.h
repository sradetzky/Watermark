#pragma once

#include "core/watermark.h"

namespace watermark::app {
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::string read_key_file(const std::filesystem::path& path);

struct Job {
    bool embedding = true;
    std::filesystem::path source, mark, input, output, report, key_file;
    Parameters parameters;
    ImageFormat format = ImageFormat::png;
    int jpeg_quality = 90;
    bool recursive = false;
    bool force = false;
    bool automatic_report = true; // GUI exports the completed result explicitly.
};
struct Event {
    std::filesystem::path file;
    std::string message;
    std::optional<DetectionResult> detection;
    bool error = false;
    std::size_t completed = 0, total = 0;
};
struct Summary {
    std::size_t completed = 0, total = 0, embedded = 0, skipped = 0, errors = 0;
    bool cancelled = false;
    std::string csv;
};
using Progress = std::function<void(const Event&)>;
Summary run_job(const Job& job, const Progress& progress = {});
// Export uses the same input/key/source protections as automatic batch reports.
void export_report(const Job& job, const std::filesystem::path& destination, const std::string& csv);
} // namespace watermark::app
