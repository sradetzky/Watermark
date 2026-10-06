#include "core/watermark.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
namespace wm = watermark;
namespace {
int failures = 0;
void require(bool condition, const std::string& name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
    if (!condition) { ++failures; }
}
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    int backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++backslashes; continue; }
        result.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'"';
}
int run(const fs::path& executable, const std::vector<std::wstring>& arguments, const fs::path& cwd) {
    std::wstring command = quote(executable.wstring());
    for (const auto& argument : arguments) { command += L" " + quote(argument); }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, cwd.c_str(), &startup, &process)) {
        throw std::runtime_error("Cannot start CLI test process.");
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1); // This test owns the process it just created.
        CloseHandle(process.hProcess);
        throw std::runtime_error("CLI test process exceeded timeout.");
    }
    DWORD code = 0;
    const BOOL success = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (!success) { throw std::runtime_error("Cannot read CLI exit code."); }
    return static_cast<int>(code);
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot read test output."); }
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        throw std::runtime_error("Cannot write fixture.");
    }
}
wm::Image fixture(int size, unsigned seed) {
    wm::Image image(size, size);
    std::mt19937 random(seed);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double value = 128 + 20 * std::sin(x * 0.1 + seed) + 15 * std::cos(y * 0.08) +
                                 static_cast<int>(random() % 17) - 8;
            const auto byte = static_cast<std::uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
            image.at(x, y) = {byte, byte, byte, 255};
        }
    }
    return image;
}
class TestDirectory {
public:
    TestDirectory() {
        path = fs::absolute(fs::current_path()) / (L"cli test-\u00e4-" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(GetTickCount64()));
        if (!fs::create_directory(path)) { throw std::runtime_error("Cannot create CLI test directory."); }
    }
    ~TestDirectory() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};
}
int main() {
    try {
        TestDirectory temporary;
        const auto& root = temporary.path;
        wchar_t executable_name[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, executable_name, 32768);
        if (!count || count == 32768) { throw std::runtime_error("Cannot resolve CLI executable location."); }
        const auto bin = fs::path(executable_name).parent_path();
        const auto embed = bin / "wmembed.exe", detect = bin / "wmdetect.exe";
        wm::Image silhouette(64, 96);
        for (auto& pixel : silhouette.pixels) { pixel = {0, 0, 0, 0}; }
        for (int y = 8; y < 88; ++y) {
            for (int x = 8; x < 56; ++x) { silhouette.at(x, y).a = 255; }
        }
        wm::save_image(silhouette, root / "silhouette.png");
        fs::create_directories(root / "photos" / "nested");
        wm::save_image(fixture(128, 91), root / "source.png");
        wm::save_image(fixture(512, 13), root / "photos" / "photo, one.png");
        wm::save_image(fixture(512, 29), root / "photos" / "nested" / "two.png");
        write(root / "photos" / "notes.txt", "Unsupported extension should be skipped.");
        write(root / "secret.txt", "\xEF\xBB\xBF" "key with spaces \xC3\xA4\r\n");
        const std::vector<std::wstring> base = {L"--source", L"source.png", L"--key-file", L"secret.txt",
                                               L"--in", L"photos", L"--out", L"stamped"};
        require(run(embed, {L"--help"}, root) == 0 && run(detect, {L"--help"}, root) == 0, "CLI help");
        require(run(embed, base, root) == 0, "Single-level folder embed with Unicode and spaced paths");
        const auto marked = root / "stamped" / "photo, one.png.png";
        require(fs::exists(marked) && !fs::exists(root / "stamped" / "nested"), "Default traversal is one level");
        require(wm::psnr(wm::load_image(root / "photos" / "photo, one.png"), wm::load_image(marked)) >= 40,
                "CLI default image quality");
        const auto original_output = read(marked);
        require(run(embed, base, root) == 1 && read(marked) == original_output, "Existing output is protected");
        auto recursive = base;
        recursive.insert(recursive.end(), {L"--recursive", L"--force", L"--format", L"jpeg", L"--quality", L"70"});
        require(run(embed, recursive, root) == 0 && fs::exists(root / "stamped" / "nested" / "two.png.jpg"),
                "Recursive JPEG folder embed");
        auto forced = base;
        forced.push_back(L"--force");
        require(run(embed, forced, root) == 0, "Explicit output replacement");
        const std::vector<std::wstring> detect_args = {L"--mark", L"stamped/mark.json", L"--key-file", L"secret.txt",
                                                      L"--in", L"stamped", L"--recursive", L"--report", L"results.csv"};
        require(run(detect, detect_args, root) == 0, "Detect via saved manifest");
        const auto report = read(root / "results.csv");
        require(report.find(",present,1.000000,") != std::string::npos &&
                report.find(",true,") != std::string::npos && report.find("photo, one.png.png\"") != std::string::npos &&
                report.find(",absent,") == std::string::npos, "CSV recovers payloads and quotes comma filenames");
        require(run(detect, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos"}, root) == 0 &&
                read(root / "report.csv").find(",absent,") != std::string::npos, "Absent is success; batch creates default report");
        require(run(detect, {L"--mark", L"stamped/mark.json", L"--passphrase", L"wrong", L"--in",
                            L"stamped/photo, one.png.png", L"--report", L"wrong.csv"}, root) == 0 &&
                read(root / "wrong.csv").find(",absent,") != std::string::npos, "Wrong key is successful absent result");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"stamped/photo, one.png.png",
                           L"--out", L"skipped"}, root) == 0 && !fs::exists(root / "skipped" / "photo, one.png.png.png"),
                "Already marked input is skipped");
        require(run(detect, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                            L"--report", L"photos/photo, one.png"}, root) == 2, "Report cannot overwrite an image");
        require(run(detect, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                            L"--report", L"secret.txt"}, root) == 2, "Report cannot overwrite key file");
        write(root / "stamped" / "photo, one.png.png", "protected key");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"stamped/photo, one.png.png",
                           L"--in", L"photos/photo, one.png", L"--out", L"stamped", L"--force"}, root) == 1 &&
                read(root / "stamped" / "photo, one.png.png") == "protected key", "Forced image output cannot overwrite key");
        require(run(embed, {L"--unknown"}, root) == 2 && run(embed, {L"--quality", L"70x"}, root) == 2,
                "Invalid arguments fail");
        write(root / "invalid.json", "{\"format_version\":2}");
        require(run(detect, {L"--mark", L"invalid.json", L"--key-file", L"secret.txt", L"--in", L"photos"}, root) == 2,
                "Unsupported manifest version rejected");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                L"--out", L"visible", L"--visible-image", L"silhouette.png", L"--visible-position", L"bottom-right",
                L"--visible-size", L"20", L"--visible-opacity", L"75", L"--visible-color", L"white"}, root) == 0, "CLI embeds visible silhouette");
        const auto visible_image = wm::load_image(root / "visible" / "photo, one.png.png");
        const auto original = wm::load_image(root / "photos" / "photo, one.png");
        require(visible_image.at(470, 460).r > original.at(470, 460).r + 40, "CLI places white silhouette at bottom right");
        require(run(detect, {L"--mark", L"visible/mark.json", L"--key-file", L"secret.txt", L"--in", L"visible",
                L"--report", L"visible.csv"}, root) == 0 && read(root / "visible.csv").find(",true,") != std::string::npos,
                "CLI detects visible mode with unchanged manifest");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                L"--out", L"invalid-visible", L"--visible-image", L"silhouette.png", L"--visible-position", L"nowhere"}, root) == 2,
                "CLI rejects invalid visible position");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                L"--out", L"invalid-visible", L"--visible-size", L"20"}, root) == 2, "CLI rejects visible settings without cutout");
        require(run(embed, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos/photo, one.png",
                L"--out", L"invalid-visible", L"--visible-image", L"source.png"}, root) == 2 &&
                !fs::exists(root / "invalid-visible" / "mark.json"), "CLI rejects opaque visible input before creating output");
        require(run(embed, {L"--watermark", L"silhouette.png", L"--in", L"photos/photo, one.png", L"--out", L"source-visible",
                L"--visible"}, root) == 0 &&
                read(root / "source-visible" / "mark.json").find("\"key_mode\": \"source-v1\"") != std::string::npos,
                "CLI uses one watermark image for visible artwork and detection identity");
        const auto subtle_image = wm::load_image(root / "source-visible" / "photo, one.png.png");
        require(std::abs(int(subtle_image.at(480, 470).r) - int(original.at(480, 470).r) / 2) <= 8,
                "CLI defaults to translucent black at 50 percent with small keyed changes");
        require(run(detect, {L"--mark", L"source-visible/mark.json", L"--in", L"source-visible", L"--report", L"source-visible.csv"}, root) == 0 &&
                read(root / "source-visible.csv").find(",present,1.000000,") != std::string::npos,
                "CLI detects source-key mark from manifest without a passphrase");
        fs::rename(root / "photos" / "photo, one.png", root / "unavailable-original.png");
        fs::rename(root / "source-visible" / "mark.json", root / "unavailable-manifest.json");
        require(run(detect, {L"--watermark", L"silhouette.png", L"--in", L"source-visible/photo, one.png.png", L"--report", L"source-identity.csv"}, root) == 0 &&
                read(root / "source-identity.csv").find(",true,") != std::string::npos,
                "CLI detects using only stamped image and watermark without original photo, manifest, or secret");
        fs::rename(root / "unavailable-original.png", root / "photos" / "photo, one.png");
        fs::rename(root / "unavailable-manifest.json", root / "source-visible" / "mark.json");
        require(run(detect, {L"--source", L"silhouette.png", L"--in", L"source-visible/photo, one.png.png", L"--report", L"legacy-alias.csv"}, root) == 0 &&
                read(root / "legacy-alias.csv").find(",true,") != std::string::npos, "CLI preserves --source alias for existing marks");
        require(run(detect, {L"--watermark", L"silhouette.png", L"--source", L"source.png", L"--in", L"photos"}, root) == 2 &&
                run(detect, {L"--watermark", L"silhouette.png", L"--mark", L"source-visible/mark.json", L"--in", L"photos"}, root) == 2,
                "CLI rejects ambiguous watermark identity options");
        require(run(embed, {L"--watermark", L"silhouette.png", L"--visible", L"--visible-image", L"silhouette.png",
                L"--in", L"photos/photo, one.png", L"--out", L"ambiguous-visible"}, root) == 2,
                "CLI rejects ambiguous visible artwork options");
        require(run(detect, {L"--source", L"source.png", L"--in", L"source-visible/photo, one.png.png", L"--report", L"other-source.csv"}, root) == 0 &&
                read(root / "other-source.csv").find(",absent,") != std::string::npos,
                "Another image cannot identify a source-key mark");
        require(run(detect, {L"--mark", L"source-visible/mark.json", L"--passphrase", L"wrong", L"--in", L"source-visible"}, root) == 2,
                "Source-key manifest rejects mixed private key mode");
        require(run(detect, {L"--mark", L"stamped/mark.json", L"--in", L"photos"}, root) == 2,
                "Legacy manifest still requires original secret");
        const auto private_manifest = read(root / "stamped" / "mark.json");
        require(run(embed, {L"--source", L"source.png", L"--in", L"photos/photo, one.png", L"--out", L"stamped"}, root) == 2 &&
                read(root / "stamped" / "mark.json") == private_manifest, "Changing key mode cannot replace existing manifest without force");
        auto unknown_mode = read(root / "source-visible" / "mark.json");
        unknown_mode.replace(unknown_mode.find("source-v1"), 9, "source-v9");
        write(root / "unknown-mode.json", unknown_mode);
        require(run(detect, {L"--mark", L"unknown-mode.json", L"--in", L"photos"}, root) == 2, "Reject unknown key derivation mode");
        fs::create_directory(root / "cutout-target");
        const auto cutout_destination = root / "cutout-target" / "photo, one.png.png";
        wm::save_image(silhouette, cutout_destination);
        const auto cutout_before = read(cutout_destination);
        require(run(embed, {L"--source", L"source.png", L"--in", L"photos/photo, one.png", L"--out", L"cutout-target",
                L"--visible-image", L"cutout-target/photo, one.png.png", L"--force"}, root) == 1 &&
                read(cutout_destination) == cutout_before, "Forced output cannot overwrite visible cutout source");
        write(root / "photos" / "broken.jpg", "not an image");
        require(run(detect, {L"--source", L"source.png", L"--key-file", L"secret.txt", L"--in", L"photos",
                            L"--report", L"errors.csv"}, root) == 1 &&
                read(root / "errors.csv").find(",error,") != std::string::npos, "Batch records corrupt-file errors");
        auto nested_output = base;
        nested_output.back() = L"photos/output";
        nested_output.push_back(L"--recursive");
        fs::remove(root / "photos" / "broken.jpg");
        require(run(embed, nested_output, root) == 0 && run(embed, nested_output, root) == 1 &&
                !fs::exists(root / "photos" / "output" / "output"), "Recursive traversal excludes its own output tree");
    } catch (const std::exception& error) {
        std::cerr << "CLI test error: " << error.what() << '\n'; return 1;
    }
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
