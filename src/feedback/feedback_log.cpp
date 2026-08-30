#include "feedback_log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "core/bopomofo.hpp"
#include "jsoncons/json.hpp"
#include "utf8cpp/utf8/cpp20.h"

namespace tsf {
namespace {

constexpr wchar_t feedback_mutex_name[] = L"Local\\LlavonImeFeedbackLog";

std::string to_utf8(std::u16string_view value) {
    return utf8::utf16to8(value);
}

std::optional<std::u16string> normalize_nfc(std::u16string_view value) {
    if (value.empty()) {
        return std::u16string{};
    }

    const auto source = reinterpret_cast<const wchar_t*>(value.data());
    const int source_length = static_cast<int>(value.size());
    const int required = NormalizeString(NormalizationC, source, source_length, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }

    std::u16string normalized(static_cast<size_t>(required), u'\0');
    const int copied = NormalizeString(NormalizationC, source, source_length,
                                       reinterpret_cast<wchar_t*>(normalized.data()), required);
    if (copied <= 0) {
        return std::nullopt;
    }
    normalized.resize(static_cast<size_t>(copied));
    return normalized;
}

std::optional<size_t> unicode_scalar_count(std::u16string_view value) {
    size_t count = 0;
    for (size_t index = 0; index < value.size(); ++index) {
        const char16_t unit = value[index];
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (index + 1 >= value.size() || value[index + 1] < 0xdc00 ||
                value[index + 1] > 0xdfff) {
                return std::nullopt;
            }
            ++index;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            return std::nullopt;
        }
        ++count;
    }
    return count;
}

bool valid_bopomofo_syllable(std::u16string_view syllable) {
    if (syllable.empty() || syllable.size() > 3) {
        return false;
    }

    size_t index = 0;
    if (index < syllable.size() && Bopomofo::initial.contains(syllable[index])) {
        ++index;
    }
    if (index < syllable.size() && Bopomofo::medial.contains(syllable[index])) {
        ++index;
    }
    if (index < syllable.size() && Bopomofo::final.contains(syllable[index])) {
        ++index;
    }
    return index == syllable.size();
}

void escape_json_line_separators(std::string& value) {
    constexpr std::string_view line_separator = "\xe2\x80\xa8";
    constexpr std::string_view paragraph_separator = "\xe2\x80\xa9";
    const auto replace_all = [&value](std::string_view target, std::string_view replacement) {
        size_t offset = 0;
        while ((offset = value.find(target, offset)) != std::string::npos) {
            value.replace(offset, target.size(), replacement);
            offset += replacement.size();
        }
    };
    replace_all(line_separator, "\\u2028");
    replace_all(paragraph_separator, "\\u2029");
}

class MutexLock final {
public:
    MutexLock() noexcept : mutex_(CreateMutexW(nullptr, FALSE, feedback_mutex_name)) {
        if (mutex_) {
            const DWORD result = WaitForSingleObject(mutex_, 2000);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }

    ~MutexLock() {
        if (locked_) {
            ReleaseMutex(mutex_);
        }
        if (mutex_) {
            CloseHandle(mutex_);
        }
    }

    bool locked() const noexcept {
        return locked_;
    }

private:
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
};

}  // namespace

std::filesystem::path FeedbackLog::path() noexcept {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                    &local_app_data)) ||
        !local_app_data) {
        if (local_app_data) {
            CoTaskMemFree(local_app_data);
        }
        return {};
    }

    try {
        const std::filesystem::path result =
            std::filesystem::path(local_app_data) / L"Llavon IME" / L"feedback.jsonl";
        CoTaskMemFree(local_app_data);
        return result;
    } catch (...) {
        CoTaskMemFree(local_app_data);
        return {};
    }
}

std::optional<std::string> FeedbackLog::serialize(const FeedbackRecord& record) noexcept {
    try {
        auto context = normalize_nfc(record.context);
        auto answer = normalize_nfc(record.answer);
        if (!context || !answer) {
            return std::nullopt;
        }

        const auto context_length = unicode_scalar_count(*context);
        const auto answer_length = unicode_scalar_count(*answer);
        if (!context_length || !answer_length || *context_length > 500 || *answer_length == 0 ||
            *answer_length > 32 || *answer_length != record.padding.size() ||
            record.difficulty < 1 || record.difficulty > 5) {
            return std::nullopt;
        }

        jsoncons::ojson padding = jsoncons::ojson::array();
        for (const auto& source : record.padding) {
            auto syllable = normalize_nfc(source.syllable);
            if (!syllable || !valid_bopomofo_syllable(*syllable) || source.tone < 1 ||
                source.tone > 5) {
                return std::nullopt;
            }

            jsoncons::ojson unit;
            unit["syllable"] = to_utf8(*syllable);
            unit["tone"] = source.tone;
            padding.push_back(std::move(unit));
        }

        jsoncons::ojson line;
        line["schemaVersion"] = 1;
        line["license"] = "CC-BY-4.0";
        line["context"] = to_utf8(*context);
        line["answer"] = to_utf8(*answer);
        line["padding"] = std::move(padding);
        line["difficulty"] = record.difficulty;

        std::string serialized = line.to_string();
        escape_json_line_separators(serialized);
        return serialized;
    } catch (...) {
        return std::nullopt;
    }
}

bool FeedbackLog::append(const FeedbackRecord& record) noexcept {
    try {
        const auto serialized = serialize(record);
        if (!serialized) {
            return false;
        }

        const std::filesystem::path log_path = path();
        if (log_path.empty()) {
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(log_path.parent_path(), error);
        if (error) {
            return false;
        }

        MutexLock lock;
        if (!lock.locked()) {
            return false;
        }

        std::ofstream output(log_path, std::ios::binary | std::ios::app);
        if (!output) {
            return false;
        }

        output << *serialized << '\n';
        output.flush();
        return output.good();
    } catch (...) {
        return false;
    }
}

}  // namespace tsf
