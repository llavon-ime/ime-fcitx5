#include <cstdlib>
#include <string>

#include "bopomofo/keymap.hpp"
#include "buffer/composition_buffer.hpp"
#include "engine/fallback_engine.hpp"

namespace {

void apply_fallback_to_last_segment(ime::fcitx5::CompositionBuffer& buffer, const ime::fcitx5::FallbackEngine& fallback) {
    const auto segment = buffer.last_edited_segment();
    if (!segment || !buffer.segment_complete(*segment)) return;

    const auto predictions = fallback.predict(buffer);
    if (*segment < predictions.size()) {
        (void)buffer.set_segment_candidates(*segment, predictions[*segment].candidates, false);
        const auto* candidates = buffer.segment_candidates(*segment);
        if (candidates == nullptr || candidates->empty()) (void)buffer.remove_segment(*segment);
    }
}

bool type_keys(ime::fcitx5::CompositionBuffer& buffer, const ime::fcitx5::FallbackEngine& fallback,
               const std::u32string& keys) {
    for (const char32_t key : keys) {
        const auto mapped = ime::fcitx5::lookup_bopomofo_key(key);
        if (!mapped || !buffer.add_bopomofo(*mapped)) return false;
        if (ime::fcitx5::is_bopomofo_tone(*mapped)) apply_fallback_to_last_segment(buffer, fallback);
    }
    return true;
}

}  // namespace

int run_buffer_tests() {
    bool ok = true;

    ime::fcitx5::CompositionBuffer buffer;
    ok = ok && buffer.empty();
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U'ˇ');
    ok = ok && !buffer.empty();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧ");
    buffer.clear();
    ok = ok && buffer.empty();

    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U'ˇ');
    ime::fcitx5::FallbackEngine fallback("tables/bopomofo_char.json");
    const auto predictions = fallback.predict(buffer);
    ok = ok && predictions.size() == 1;
    ok = ok && !predictions.front().candidates.empty();

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇㄏㄠˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好");
    ok = ok && buffer.commit_text() == std::u16string(u"你好");
    ok = ok && buffer.completed_segment_indices().size() == 2;

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"SU3CL3");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好");

    const auto target = buffer.candidate_target(ime::fcitx5::CandidateTarget::BeforeCursor);
    ok = ok && target && *target == 1;
    const auto* target_candidates = target ? buffer.segment_candidates(*target) : nullptr;
    if (target_candidates != nullptr && target_candidates->size() > 1) {
        ok = ok && buffer.select_candidate(*target, 1, false);
        ok = ok && !buffer.empty();
        ok = ok && buffer.rendered_composition().size() == 2;
    }

    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你");
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U' ');
    ok = ok && buffer.segment_complete(0);

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ˋ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ˋ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"ˋ");
    ok = ok && !buffer.segment_complete(0);
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄧˋ");

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"4u4");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄧˋ");
    ok = ok && buffer.rendered_composition().size() == 1;

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && buffer.move_cursor_left();
    ok = ok && buffer.candidate_target(ime::fcitx5::CandidateTarget::BeforeCursor) &&
         *buffer.candidate_target(ime::fcitx5::CandidateTarget::BeforeCursor) == 0;
    ok = ok && buffer.candidate_target(ime::fcitx5::CandidateTarget::AfterCursor) &&
         *buffer.candidate_target(ime::fcitx5::CandidateTarget::AfterCursor) == 1;

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"1m3");
    ok = ok && buffer.empty();
    ok = ok && type_keys(buffer, fallback, U"su3");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你");

    ok = ok && buffer.add_bopomofo(U'ˋ');
    ok = ok && buffer.commit_text() == std::u16string(u"你ˋ");
    ok = ok && buffer.candidate_commit_text() == std::u16string(u"你");

    buffer.clear();
    ok = ok && buffer.add_literal(U'。');
    ok = ok && buffer.rendered_composition() == std::u16string(u"。");
    ok = ok && buffer.commit_text() == std::u16string(u"。");
    ok = ok && buffer.candidate_commit_text() == std::u16string(u"。");
    ok = ok && !buffer.has_unfinished_reading();
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ㄑ');
    ok = ok && buffer.add_bopomofo(U'ㄎ');
    ok = ok && buffer.add_bopomofo(U'ㄇ');
    ok = ok && buffer.add_bopomofo(U'ㄨ');
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄑ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄑㄨ");
    ok = ok && buffer.segments().size() == 1;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
