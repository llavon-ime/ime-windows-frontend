#pragma once
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "bopomofo.hpp"
#include "engine/engine.hpp"
#include "feedback/feedback_log.hpp"
using namespace std::literals;

namespace tsf {

class Buffer {
protected:
    std::vector<BopomofoPos> buffer;

public:
    std::u16string to_string() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += item.current();
        }
        return res;
    }
    void clear() {
        buffer.clear();
    }
    bool empty() const {
        return buffer.empty();
    }
};

class CompositionBuffer {
    std::vector<BopomofoPos> buffer;
    std::mutex mutex;
    int idx = -1;
    bool feedback_invalid_ = false;

    bool has_feedback() const {
        for (const auto& item : buffer) {
            if (item.feedback_original) {
                return true;
            }
        }
        return false;
    }

    static bool sentence_terminal(char32_t ch) {
        switch (ch) {
            case U'.':
            case U'!':
            case U'?':
            case U';':
            case U'\n':
            case U'\r':
            case U'。':
            case U'！':
            case U'？':
            case U'；':
            case U'…':
                return true;
            default:
                return false;
        }
    }

    int candidate_target_index() const {
        if (buffer.empty()) return -1;

        const int size = static_cast<int>(buffer.size());
        const int right_of_caret = idx + 1;
        if (right_of_caret >= 0 && right_of_caret < size) {
            return right_of_caret;
        }

        if (idx >= 0 && idx < size) {
            return idx;
        }

        return -1;
    }

public:
    bool pre() {
        if (idx < 0) return false;
        idx--;
        return true;
    }
    bool next() {
        if (idx + 1 >= static_cast<int>(buffer.size())) return false;
        idx++;
        return true;
    }
    bool remove_last() {
        std::lock_guard lock(mutex);
        if (idx < 0 || idx >= static_cast<int>(buffer.size())) return false;

        if (has_feedback()) {
            feedback_invalid_ = true;
        }

        bool removed = false;
        if (buffer[idx].is_compositable()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
            removed = true;
        } else {
            removed = buffer[idx].pop_last_bopomofo();
            if (!removed || buffer[idx].is_null()) {
                buffer.erase(buffer.begin() + idx);
                idx--;
                removed = true;
            }
        }

        if (buffer.empty()) {
            idx = -1;
            return removed;
        }

        if (idx >= static_cast<int>(buffer.size())) {
            idx = static_cast<int>(buffer.size()) - 1;
        }

        for (int i = 0; i <= idx; i++) {
            buffer[i].predicted = false;
        }

        return removed;
    }
    void clear() {
        buffer.clear();
        idx = -1;
        feedback_invalid_ = false;
    }
    bool empty() const {
        return buffer.empty();
    }
    bool current_invalid() const {
        return idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid();
    }
    bool current_compositable() const {
        return idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_compositable();
    }
    bool current_has_candidate_list() const {
        const int target = candidate_target_index();
        return target >= 0 && buffer[target].is_compositable() && buffer[target].get_candidates().size() > 1;
    }
    void invalidate_all_predictions() {
        for (int i = 0; i < static_cast<int>(buffer.size()); i++) {
            buffer[i].predicted = false;
        }
    }
    size_t caret_offset() const {
        size_t offset = 0;
        for (int i = 0; i <= idx && i < static_cast<int>(buffer.size()); i++) {
            offset += buffer[i].current().size();
        }
        return offset;
    }
    std::optional<std::pair<size_t, size_t>> current_invalid_span() const {
        if (!current_invalid()) {
            return std::nullopt;
        }

        size_t start = 0;
        for (int i = 0; i < idx; i++) {
            start += buffer[i].current().size();
        }
        return std::pair{start, buffer[idx].current().size()};
    }
    void add(char16_t ch) {
        std::lock_guard lock(mutex);
        if (has_feedback() && idx + 1 < static_cast<int>(buffer.size())) {
            feedback_invalid_ = true;
        }
        DebugSink::instance().send(L"INFO", ch);
        if (idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
        }
        if (idx == -1 || !buffer[idx].accept(ch)) {
            idx++;
            // buffer.insert(buffer.begin() + idx, {}); 會炸 我操你媽的標準委員會
            buffer.insert(buffer.begin() + idx, BopomofoPos{});
            DebugSink::instance().send(L"INFO", std::to_string(idx) + " wtf " + std::to_string(buffer.size()));
            buffer[idx].accept(ch);
        } else {
            // current accept this ch
            // do nothing
        }
    }
    void add_chosen_candidate(char32_t ch) {
        std::lock_guard lock(mutex);
        if (has_feedback() && idx + 1 < static_cast<int>(buffer.size())) {
            feedback_invalid_ = true;
        }
        if (idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
        }

        idx++;
        buffer.insert(buffer.begin() + idx, BopomofoPos{});
        buffer[idx].set_chosen_candidate(ch);

        for (int i = 0; i <= idx; i++) {
            buffer[i].predicted = false;
        }
        buffer[idx].predicted = true;
    }
    void predict_paddings(std::u16string context) {
        DebugSink::instance().send(L"INFO", u"context : " + context);
        if (buffer.empty()) {
            return;
        }

        bool need_predict = false;
        for (auto& item : buffer) {
            if (!item.is_predictable_by_engine()) {
                return;
            }
            if (!item.predicted) {
                need_predict = true;
                break;
            }
        }
        if (!need_predict) {
            return;
        }

        auto engine = get_engine();
        engine->predict(context, std::span<BopomofoPos>(buffer.begin(), buffer.end()));
    }
    BopomofoPos& cur() {
        if (idx < 0 || idx >= buffer.size()) {
            DebugSink::instance().send(L"ERROR", "out of range");
            throw std::runtime_error("out of range");
        }
        return buffer[idx];
    }
    BopomofoPos& candidate_target() {
        const int target = candidate_target_index();
        if (target < 0 || target >= static_cast<int>(buffer.size())) {
            DebugSink::instance().send(L"ERROR", "candidate target out of range");
            throw std::runtime_error("candidate target out of range");
        }
        return buffer[target];
    }
    std::u16string to_string() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += item.current();
        }
        return res;
    }

    std::vector<FeedbackRecord> feedback_records() const {
        std::vector<FeedbackRecord> records;
        if (feedback_invalid_ || !has_feedback()) {
            return records;
        }

        for (size_t correction = 0; correction < buffer.size(); ++correction) {
            const auto& corrected_item = buffer[correction];
            if (!corrected_item.feedback_original) {
                continue;
            }
            if (*corrected_item.feedback_original == corrected_item.current32()) {
                continue;
            }

            size_t sentence_start = 0;
            for (size_t i = correction; i > 0; --i) {
                const auto& previous = buffer[i - 1];
                if (previous.is_compositable() && sentence_terminal(previous.current32())) {
                    sentence_start = i;
                    break;
                }
            }

            size_t sentence_end = buffer.size();
            for (size_t i = correction; i < buffer.size(); ++i) {
                const auto& item = buffer[i];
                if (item.is_compositable() && sentence_terminal(item.current32())) {
                    sentence_end = i + 1;
                    break;
                }
            }

            FeedbackRecord record;
            record.bopomofo = corrected_item.feedback_bopomofo;
            utf8::append16(*corrected_item.feedback_original, record.original);
            utf8::append16(corrected_item.current32(), record.selected);
            for (size_t i = sentence_start; i < sentence_end; ++i) {
                record.sentence += buffer[i].current();
            }
            records.push_back(std::move(record));
        }
        return records;
    }
};

}  // namespace tsf
