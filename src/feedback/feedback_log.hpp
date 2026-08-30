#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tsf {

struct FeedbackPadding {
    std::u16string syllable;
    int tone = 1;
};

struct FeedbackRecord {
    std::u16string context;
    std::u16string answer;
    std::vector<FeedbackPadding> padding;
    int difficulty = 1;
};

class FeedbackLog final {
public:
    static std::optional<std::string> serialize(const FeedbackRecord& record) noexcept;
    static bool append(const FeedbackRecord& record) noexcept;
    static std::filesystem::path path() noexcept;
};

}  // namespace tsf
