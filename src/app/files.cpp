#include "app/files.h"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace watermark::app {
namespace fs = std::filesystem;
std::string utf8(const std::wstring& value) {
    if (value.empty()) { return {}; }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) { throw std::runtime_error("Invalid Unicode argument."); }
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr)) {
        throw std::runtime_error("Cannot encode Unicode argument.");
    }
    return result;
}
std::string display(const fs::path& path) { return utf8(path.wstring()); }
std::wstring wide(const std::string& value) {
    if (value.empty()) { return {}; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) { throw std::runtime_error("Invalid UTF-8 text."); }
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length)) {
        throw std::runtime_error("Cannot decode UTF-8 text.");
    }
    return result;
}
std::string read_text(const fs::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { throw std::runtime_error("Cannot open " + display(path)); }
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > limit) {
        throw std::runtime_error("Text file exceeds size limit: " + display(path));
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!result.empty() && !input.read(result.data(), length)) {
        throw std::runtime_error("Cannot read " + display(path));
    }
    return result;
}

std::string read_key_file(const fs::path& path) {
    auto text = read_text(path, 1024 * 1024);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) { text.erase(0, 3); }
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
        if (!text.empty() && text.back() == '\r') { text.pop_back(); }
    }
    if (text.empty() || text.find_first_of("\r\n") != std::string::npos || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("Key file must contain one nonempty UTF-8 passphrase line.");
    }
    wide(text); // Validate the byte encoding without changing it.
    return text;
}
const char* strength_name(Strength strength) {
    return strength == Strength::low ? "low" : strength == Strength::high ? "high" : "default";
}
std::string hash_text(std::uint64_t hash) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}
std::string mark_json(const Payload& payload, Strength strength) {
    std::ostringstream output;
    output << "{\n  \"format_version\": 1,\n  \"source_hash\": \"" << hash_text(payload.source_hash)
           << "\",\n  \"tile_size\": 128,\n  \"coefficient_set\": \"mid8-v1\",\n  \"step\": 6.0,\n  \"strength\": \""
           << strength_name(strength) << "\"\n}\n";
    return output.str();
}
Payload read_mark(const fs::path& path) {
    const auto text = read_text(path, 16 * 1024);
    // A deliberately narrow, flat versioned schema. No user text is stored in this file.
    const std::regex field(R"json(\s*"([a-z_]+)"\s*:\s*("[a-zA-Z0-9.-]+"|[0-9]+(?:\.[0-9]+)?)\s*)json");
    std::map<std::string, std::string> fields;
    std::size_t position = text.find_first_not_of(" \t\r\n");
    const auto fail = [] { throw std::invalid_argument("Invalid or unsupported mark.json schema."); };
    if (position == std::string::npos || text[position++] != '{') { fail(); }
    while (true) {
        std::smatch match;
        const auto remaining = text.substr(position);
        if (!std::regex_search(remaining, match, field, std::regex_constants::match_continuous)) { fail(); }
        if (!fields.emplace(match[1].str(), match[2].str()).second) { fail(); }
        position += match.length();
        if (position >= text.size()) { fail(); }
        if (text[position] == '}') { ++position; break; }
        if (text[position++] != ',') { fail(); }
    }
    if (text.find_first_not_of(" \t\r\n", position) != std::string::npos || fields.size() != 6 ||
        fields["format_version"] != "1" || fields["tile_size"] != "128" ||
        fields["coefficient_set"] != "\"mid8-v1\"" || fields["step"] != "6.0" ||
        (fields["strength"] != "\"default\"" && fields["strength"] != "\"low\"" && fields["strength"] != "\"high\"")) {
        fail();
    }
    const auto hash = fields["source_hash"];
    if (!std::regex_match(hash, std::regex("\"[0-9a-fA-F]{16}\""))) { fail(); }
    return Payload{std::stoull(hash.substr(1, 16), nullptr, 16)};
}

bool supported(const fs::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    static const std::set<std::wstring> extensions = {L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".webp"};
    return extensions.count(extension) != 0;
}
bool same_path(const fs::path& a, const fs::path& b) {
    std::error_code error;
    if (fs::equivalent(a, b, error) && !error) { return true; }
    const auto left = fs::weakly_canonical(a).wstring(), right = fs::weakly_canonical(b).wstring();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::vector<fs::path> input_files(const fs::path& input, bool recursive, const fs::path& excluded,
                                const Progress& progress, const std::function<bool()>& cancelled) {
    std::vector<fs::path> files;
    const auto add = [&](const fs::directory_entry& entry) {
        if (cancelled && cancelled()) { throw OperationCancelled(); }
        if (!entry.is_regular_file()) { return; }
        if (supported(entry.path())) { files.push_back(entry.path()); }
        else if (progress) { progress({entry.path(), "SKIP unsupported " + display(entry.path())}); }
    };
    if (fs::is_regular_file(input)) {
        if (!supported(input)) { throw std::invalid_argument("Unsupported input extension: " + display(input)); }
        files.push_back(input);
    } else if (fs::is_directory(input)) {
        if (recursive) {
            for (fs::recursive_directory_iterator iterator(input), end; iterator != end; ++iterator) {
                if (cancelled && cancelled()) { throw OperationCancelled(); }
                if (iterator->is_directory() && !excluded.empty() && same_path(iterator->path(), excluded)) {
                    iterator.disable_recursion_pending();
                } else { add(*iterator); }
            }
        } else { for (const auto& entry : fs::directory_iterator(input)) { add(entry); } }
    } else { throw std::invalid_argument("Input is not a file or directory: " + display(input)); }
    std::sort(files.begin(), files.end());
    if (files.empty()) { throw std::invalid_argument("No supported images found."); }
    return files;
}

class TemporaryOutput {
public:
    explicit TemporaryOutput(const fs::path& destination) : destination_(destination) {
        path = destination;
        path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        if (fs::exists(path)) { throw std::runtime_error("Temporary output already exists."); }
    }
    ~TemporaryOutput() { std::error_code error; fs::remove(path, error); }
    void commit(bool replace) {
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(path.c_str(), destination_.c_str(), flags)) {
            throw std::runtime_error("Cannot commit output: " + display(destination_));
        }
    }
    fs::path path;
private:
    fs::path destination_;
};
void write_text(const fs::path& path, const std::string& text, bool replace) {
    TemporaryOutput temporary(path);
    {
        std::ofstream output(temporary.path, std::ios::binary);
        if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            throw std::runtime_error("Cannot write " + display(path));
        }
        output.close();
        if (!output) { throw std::runtime_error("Cannot close " + display(path)); }
    }
    temporary.commit(replace);
}
void write_image(const Image& image, const fs::path& path, ImageFormat format, int quality, bool replace) {
    TemporaryOutput temporary(path);
    save_image(image, temporary.path, format, quality);
    temporary.commit(replace);
}
std::string csv(const std::string& value) {
    std::string escaped = "\"";
    for (const char c : value) { if (c == '"') { escaped += '"'; } escaped += c; }
    return escaped + '"';
}

} // namespace watermark::app
