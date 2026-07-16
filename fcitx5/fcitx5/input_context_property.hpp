#pragma once

#include <fcitx/inputcontextproperty.h>

#include <memory>
#include <optional>
#include <functional>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "protocol/protocol.hpp"

namespace ime::fcitx5 {

class ImeInputContextProperty final : public fcitx::InputContextProperty {
public:
    ~ImeInputContextProperty() override {
        if (session_close_handle) session_close_handle();
    }

    CompositionBuffer buffer;
    std::vector<char32_t> displayed_candidates;
    int candidate_page = 0;
    int candidate_cursor = 0;
    bool candidate_expanded = false;
    bool candidate_ui_hidden = true;

    protocol::SessionId session_id{};
    std::uint64_t next_request_id = 1;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> inflight_request_id;
    std::uint64_t inflight_revision = 0;
    std::u16string prediction_key;
    std::u16string prediction_context;
    std::string prediction_base_model_hash;
    protocol::EventId prediction_feedback_token{};
    bool feedback_sensitive = false;
    std::vector<std::size_t> inflight_segment_indices;
    bool prediction_pending = false;
    bool prediction_dirty = false;

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::function<void()> session_close_handle;

    void invalidate_generation() {
        ++generation;
        inflight_request_id.reset();
        prediction_pending = false;
        prediction_dirty = false;
        inflight_segment_indices.clear();
        prediction_context.clear();
        prediction_base_model_hash.clear();
        prediction_feedback_token = {};
        feedback_sensitive = false;
    }

    void copyTo(fcitx::InputContextProperty*) override {}
    bool needCopy() const override { return false; }
};

using ImeInputContextPropertyFactory = fcitx::SimpleInputContextPropertyFactory<ImeInputContextProperty>;

}  // namespace ime::fcitx5
