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

    bool has_effective_feedback() const {
        for (const auto& item : buffer) {
            if (item.feedback_original && *item.feedback_original != item.current32()) {
                return true;
            }
        }
        return false;
    }

    static std::optional<int> validation_tone(const BopomofoPos& item) {
        switch (item.tone) {
            case u' ':
                return 1;
            case u'ˊ':
                return 2;
            case u'ˇ':
                return 3;
            case u'ˋ':
                return 4;
            case u'˙':
                return 5;
            default:
                return std::nullopt;
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

    bool has_recordable_feedback() const {
        return !feedback_invalid_ && has_effective_feedback();
    }

    std::optional<FeedbackRecord> feedback_record(std::u16string context) const {
        if (!has_recordable_feedback() || buffer.empty() || buffer.size() > 32) {
            return std::nullopt;
        }

        FeedbackRecord record;
        record.context = std::move(context);
        record.answer = to_string();
        record.padding.reserve(buffer.size());

        for (const auto& item : buffer) {
            const auto tone_number = validation_tone(item);
            if (!item.is_compositable() || !tone_number) {
                return std::nullopt;
            }

            FeedbackPadding padding;
            if (item.initial) padding.syllable.push_back(item.initial);
            if (item.medial) padding.syllable.push_back(item.medial);
            if (item.final) padding.syllable.push_back(item.final);
            if (padding.syllable.empty()) {
                return std::nullopt;
            }
            padding.tone = *tone_number;
            record.padding.push_back(std::move(padding));
        }

        return record;
    }
};

}  // namespace tsf
