#include "feedback_log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>

#include "jsoncons/json.hpp"
#include "utf8cpp/utf8/cpp20.h"

namespace tsf {
namespace {

constexpr wchar_t feedback_mutex_name[] = L"Local\\LlavonImeFeedbackLog";

std::string utc_timestamp() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);
    char value[32] = {};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                  static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                  static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                  static_cast<unsigned>(time.wMilliseconds));
    return value;
}

std::string to_utf8(std::u16string_view value) {
    return utf8::utf16to8(value);
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

bool FeedbackLog::append(std::span<const FeedbackRecord> records) noexcept {
    if (records.empty()) {
        return true;
    }

    try {
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

        for (const auto& feedback : records) {
            jsoncons::json line;
            line["timestamp"] = utc_timestamp();
            line["bopomofo"] = to_utf8(feedback.bopomofo);
            line["original"] = to_utf8(feedback.original);
            line["selected"] = to_utf8(feedback.selected);
            line["sentence"] = to_utf8(feedback.sentence);
            output << line.to_string() << '\n';
        }
        output.flush();
        return output.good();
    } catch (...) {
        return false;
    }
}

}  // namespace tsf
