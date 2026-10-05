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
