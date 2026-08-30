#include "feedback/feedback_log.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    tsf::FeedbackRecord record;
    record.context = u"我今天想吃";
    record.answer = u"早餐";
    record.padding = {{u"ㄗㄠ", 3}, {u"ㄘㄢ", 1}};

    const auto serialized = tsf::FeedbackLog::serialize(record);
    const std::string expected =
        R"({"schemaVersion":1,"license":"CC-BY-4.0","context":"我今天想吃","answer":"早餐","padding":[{"syllable":"ㄗㄠ","tone":3},{"syllable":"ㄘㄢ","tone":1}],"difficulty":1})";
    if (!expect(serialized.has_value(), "valid feedback record was rejected") ||
        !expect(*serialized == expected, "feedback JSON is not canonical validation-set format")) {
        return 1;
    }

    record.context = u"line\u2028paragraph\u2029";
    const auto escaped = tsf::FeedbackLog::serialize(record);
    if (!expect(escaped.has_value(), "line-separator feedback record was rejected") ||
        !expect(escaped->find("\\u2028") != std::string::npos &&
                    escaped->find("\\u2029") != std::string::npos,
                "JSON line separators were not escaped")) {
        return 1;
    }

    record.padding.pop_back();
    if (!expect(!tsf::FeedbackLog::serialize(record),
                "misaligned answer and padding should be rejected")) {
        return 1;
    }

    return 0;
}
