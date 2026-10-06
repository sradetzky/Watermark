#include "cli/common.h"
#include "app/jobs.h"

#include <iostream>
#include <map>
#include <set>

namespace watermark::cli {
namespace {
namespace fs = std::filesystem;
using app::utf8;
struct Options {
    std::map<std::wstring, std::wstring> values;
    bool recursive = false;
    bool force = false;
    bool visible = false;
    bool help = false;
    bool contains(const std::wstring& name) const { return values.count(name) != 0; }
    std::wstring get(const std::wstring& name, const std::wstring& fallback = L"") const {
        const auto value = values.find(name);
        return value == values.end() ? fallback : value->second;
    }
    fs::path required(const std::wstring& name) const {
        const auto value = get(name);
        if (value.empty()) { throw std::invalid_argument("Required option: " + utf8(name)); }
        return fs::path(value);
    }
};
Options parse(int argc, wchar_t** argv, bool embedding) {
    const std::set<std::wstring> common = {L"--watermark", L"--source", L"--key-file", L"--passphrase", L"--in"};
    const std::set<std::wstring> embed_values = {L"--out", L"--format", L"--quality", L"--strength",
        L"--visible-image", L"--visible-position", L"--visible-size", L"--visible-opacity", L"--visible-color"};
    const std::set<std::wstring> detect_values = {L"--mark", L"--report"};
    Options options;
    std::set<std::wstring> seen;
    for (int i = 1; i < argc; ++i) {
        const std::wstring name = argv[i];
        if (!seen.insert(name).second) { throw std::invalid_argument("Duplicate option: " + utf8(name)); }
        if (name == L"--help" || name == L"-h") { options.help = true; }
        else if (name == L"--recursive") { options.recursive = true; }
        else if (name == L"--force" && embedding) { options.force = true; }
        else if (name == L"--visible" && embedding) { options.visible = true; }
        else if (common.count(name) || (embedding ? embed_values : detect_values).count(name)) {
            if (++i >= argc || std::wstring(argv[i]).empty() || std::wstring(argv[i]).rfind(L"--", 0) == 0) {
                throw std::invalid_argument("Missing value for " + utf8(name));
            }
            options.values[name] = argv[i];
        } else { throw std::invalid_argument("Unknown option: " + utf8(name)); }
    }
    return options;
}
Parameters parameters(const Options& options) {
    if (options.contains(L"--key-file") && options.contains(L"--passphrase")) {
        throw std::invalid_argument("Supply only one of --key-file and --passphrase.");
    }
    Parameters result;
    if (options.contains(L"--key-file")) {
        result.passphrase = app::read_key_file(options.required(L"--key-file"));
    } else if (options.contains(L"--passphrase")) { result.passphrase = utf8(options.get(L"--passphrase")); }
    const auto strength = options.get(L"--strength", L"default");
    if (strength == L"low") { result.strength = Strength::low; }
    else if (strength == L"high") { result.strength = Strength::high; }
    else if (strength != L"default") { throw std::invalid_argument("Strength must be low, default, or high."); }
    return result;
}
void help(bool embedding) {
    if (embedding) {
        std::cout << "wmembed --watermark logo.png --in photos --out stamped\n"
                     "  --format png|jpeg       Output format (default png)\n"
                     "  --quality 1..100        JPEG quality (default 90)\n"
                     "  --strength low|default|high\n"
                     "  --visible               Also stamp the watermark image (transparent PNG)\n"
                     "  --visible-image PNG     Alternative visible artwork (legacy option)\n"
                     "  --visible-position bottom-right|bottom-left|top-right|top-left|center\n"
                     "  --visible-size 1..50     Percent of short side (longest stamp edge; default 15)\n"
                     "  --visible-opacity 1..100 Percent opacity (default 50)\n"
                     "  --visible-color black|white (default black)\n"
                     "  --force                 Rewrite an existing mark; replace existing output\n";
    } else {
        std::cout << "wmdetect --watermark logo.png --in images\n"
                     "  Only the image to inspect and watermark artwork are needed; no original photo\n"
                     "  --mark mark.json        Alternative to the watermark image\n"
                     "  --report report.csv     CSV destination (default report.csv for a directory)\n";
    }
    std::cout << "  --recursive             Include subdirectories (default one level)\n"
                 "  --source IMAGE          Legacy alias for --watermark\n"
                 "  Default: public key derived from watermark identity; no passphrase needed\n"
                 "  --key-file FILE         Use a private UTF-8 key (required for legacy marks)\n"
                 "  --passphrase TEXT       Alternative to --key-file\n"
                 "  --help                  Show usage\n"
                 "Exit 0: completed (absent is a valid result). Exit 1: file/operation failure. Exit 2: invalid arguments.\n";
}
}

int run(int argc, wchar_t** argv, bool embedding) {
    try {
        const auto options = parse(argc, argv, embedding);
        if (options.help) { help(embedding); return 0; }
        app::Job job;
        job.embedding = embedding;
        job.parameters = parameters(options);
        job.key_from_source = !options.contains(L"--key-file") && !options.contains(L"--passphrase");
        job.input = options.required(L"--in");
        job.key_file = options.get(L"--key-file");
        if (options.contains(L"--watermark") && options.contains(L"--source")) {
            throw std::invalid_argument("Supply only one of --watermark and its --source alias.");
        }
        job.source = options.get(options.contains(L"--source") ? L"--source" : L"--watermark");
        job.recursive = options.recursive;
        job.force = options.force;
        if (embedding) {
            if (job.source.empty()) { throw std::invalid_argument("Required option: --watermark"); }
            job.output = options.required(L"--out");
            const auto format = options.get(L"--format", L"png");
            if (format != L"png" && format != L"jpeg") { throw std::invalid_argument("Format must be png or jpeg."); }
            job.format = format == L"png" ? ImageFormat::png : ImageFormat::jpeg;
            const auto quality = options.get(L"--quality", L"90");
            if (quality.empty() || quality.size() > 3 || quality.find_first_not_of(L"0123456789") != std::wstring::npos) {
                throw std::invalid_argument("JPEG quality must be 1..100.");
            }
            job.jpeg_quality = std::stoi(quality);
            job.visible_image = options.get(L"--visible-image");
            if (options.visible) {
                if (!job.visible_image.empty()) { throw std::invalid_argument("Supply only one of --visible and --visible-image."); }
                job.visible_image = job.source;
            }
            if (job.visible_image.empty() && (options.contains(L"--visible-position") ||
                options.contains(L"--visible-size") || options.contains(L"--visible-opacity") || options.contains(L"--visible-color"))) {
                throw std::invalid_argument("Visible settings require --visible or --visible-image.");
            }
            const std::map<std::wstring, Position> positions = {
                {L"bottom-right", Position::bottom_right}, {L"bottom-left", Position::bottom_left},
                {L"top-right", Position::top_right}, {L"top-left", Position::top_left}, {L"center", Position::center}};
            const auto position = positions.find(options.get(L"--visible-position", L"bottom-right"));
            if (position == positions.end()) { throw std::invalid_argument("Invalid visible watermark position."); }
            job.visible.position = position->second;
            const auto color = options.get(L"--visible-color", L"black");
            if (color != L"black" && color != L"white") { throw std::invalid_argument("Visible color must be black or white."); }
            job.visible.ink = color == L"black" ? VisibleInk::black : VisibleInk::white;
            const auto percent = [&](const std::wstring& name, const std::wstring& fallback) {
                const auto value = options.get(name, fallback);
                if (value.empty() || value.size() > 3 || value.find_first_not_of(L"0123456789") != std::wstring::npos) {
                    throw std::invalid_argument("Invalid percentage: " + utf8(name));
                }
                return std::stoi(value);
            };
            job.visible.size_percent = percent(L"--visible-size", L"15");
            job.visible.opacity_percent = percent(L"--visible-opacity", L"50");
        } else {
            if ((!job.source.empty()) == options.contains(L"--mark")) {
                throw std::invalid_argument("Supply exactly one of --watermark and --mark.");
            }
            job.mark = options.get(L"--mark");
            job.report = options.get(L"--report");
        }
        const auto summary = app::run_job(job, [](const app::Event& event) {
            if (event.message.rfind("Processing ", 0) == 0) { return; }
            (event.error ? std::cerr : std::cout) << event.message << '\n';
        });
        if (embedding) {
            std::cout << "Embedded=" << summary.embedded << " skipped=" << summary.skipped << " errors=" << summary.errors << '\n';
        }
        return summary.errors || summary.cancelled ? 1 : 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Argument error: " << error.what() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'; return 1;
    }
}
int run_embed(int argc, wchar_t** argv) { return run(argc, argv, true); }
int run_detect(int argc, wchar_t** argv) { return run(argc, argv, false); }
} // namespace watermark::cli
