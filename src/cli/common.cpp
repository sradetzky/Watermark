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
    const std::set<std::wstring> common = {L"--source", L"--key-file", L"--passphrase", L"--in"};
    const std::set<std::wstring> embed_values = {L"--out", L"--format", L"--quality", L"--strength"};
    const std::set<std::wstring> detect_values = {L"--mark", L"--report"};
    Options options;
    std::set<std::wstring> seen;
    for (int i = 1; i < argc; ++i) {
        const std::wstring name = argv[i];
        if (!seen.insert(name).second) { throw std::invalid_argument("Duplicate option: " + utf8(name)); }
        if (name == L"--help" || name == L"-h") { options.help = true; }
        else if (name == L"--recursive") { options.recursive = true; }
        else if (name == L"--force" && embedding) { options.force = true; }
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
    if (options.contains(L"--key-file") == options.contains(L"--passphrase")) {
        throw std::invalid_argument("Supply exactly one of --key-file and --passphrase.");
    }
    Parameters result;
    if (options.contains(L"--key-file")) {
        result.passphrase = app::read_key_file(options.required(L"--key-file"));
    } else { result.passphrase = utf8(options.get(L"--passphrase")); }
    if (result.passphrase.empty()) { throw std::invalid_argument("Passphrase must not be empty."); }
    const auto strength = options.get(L"--strength", L"default");
    if (strength == L"low") { result.strength = Strength::low; }
    else if (strength == L"high") { result.strength = Strength::high; }
    else if (strength != L"default") { throw std::invalid_argument("Strength must be low, default, or high."); }
    return result;
}
void help(bool embedding) {
    if (embedding) {
        std::cout << "wmembed --source logo.png --key-file secret.txt --in photos --out stamped\n"
                     "  --format png|jpeg       Output format (default png)\n"
                     "  --quality 1..100        JPEG quality (default 90)\n"
                     "  --strength low|default|high\n"
                     "  --force                 Rewrite an existing mark; replace existing output\n";
    } else {
        std::cout << "wmdetect (--source logo.png | --mark stamped/mark.json) --key-file secret.txt --in images\n"
                     "  --report report.csv     CSV destination (default report.csv for a directory)\n";
    }
    std::cout << "  --recursive             Include subdirectories (default one level)\n"
                 "  --passphrase TEXT       Alternative to a UTF-8 --key-file\n"
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
        job.input = options.required(L"--in");
        job.key_file = options.get(L"--key-file");
        job.source = options.get(L"--source");
        job.recursive = options.recursive;
        job.force = options.force;
        if (embedding) {
            job.source = options.required(L"--source");
            job.output = options.required(L"--out");
            const auto format = options.get(L"--format", L"png");
            if (format != L"png" && format != L"jpeg") { throw std::invalid_argument("Format must be png or jpeg."); }
            job.format = format == L"png" ? ImageFormat::png : ImageFormat::jpeg;
            const auto quality = options.get(L"--quality", L"90");
            if (quality.empty() || quality.size() > 3 || quality.find_first_not_of(L"0123456789") != std::wstring::npos) {
                throw std::invalid_argument("JPEG quality must be 1..100.");
            }
            job.jpeg_quality = std::stoi(quality);
        } else {
            if (options.contains(L"--source") == options.contains(L"--mark")) {
                throw std::invalid_argument("Supply exactly one of --source and --mark.");
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
