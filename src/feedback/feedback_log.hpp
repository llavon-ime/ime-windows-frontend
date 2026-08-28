#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace tsf {

struct FeedbackRecord {
    std::u16string bopomofo;
    std::u16string original;
    std::u16string selected;
    std::u16string sentence;
};

class FeedbackLog final {
public:
    static bool append(std::span<const FeedbackRecord> records) noexcept;
    static std::filesystem::path path() noexcept;
};

}  // namespace tsf
