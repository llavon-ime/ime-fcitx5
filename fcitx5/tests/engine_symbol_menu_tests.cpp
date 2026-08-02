#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>
#include <vector>

using namespace ime::fcitx5::test;

namespace {

const std::vector<std::string> kLevel1Items = {"…", "※",     "常用符號", "左右括號", "上下括號", "希臘字母", "數學符號",
                                               "特殊圖形", "Unicode", "單線框",   "雙線框",   "填色方塊", "線段"};

void configure(EngineHarness& harness) {
    harness.set_config("CandidatePageSize", "20");
    harness.set_config("SelectionKeysCount", "10");
    harness.set_config("ChooseCandidateUsingSpace", "True");
}

int cursor_index(const EngineHarness& harness) {
    const auto list = harness.input_context()->inputPanel().candidateList();
    return list ? list->cursorIndex() : -1;
}

}  // namespace

void engine_test_symbol_menu_tests(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        configure(harness);

        harness.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.preedit().empty());
        FCITX_ASSERT(harness.candidate_count() == kLevel1Items.size());
        for (size_t i = 0; i < kLevel1Items.size(); ++i) {
            FCITX_ASSERT(harness.candidate(i) == kLevel1Items[i]);
        }
        FCITX_ASSERT(cursor_index(harness) == 0);

        EngineHarness h_direct(instance);
        configure(h_direct);
        h_direct.key(fcitx::Key(FcitxKey_grave));
        h_direct.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(!h_direct.has_candidates());
        FCITX_ASSERT(h_direct.preedit() == "…");
        h_direct.expect_commit("…");

        EngineHarness h_space(instance);
        configure(h_space);
        h_space.key(fcitx::Key(FcitxKey_grave));
        h_space.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!h_space.has_candidates());
        FCITX_ASSERT(h_space.preedit() == "…");
        h_space.expect_commit("…");

        EngineHarness h_tenth(instance);
        configure(h_tenth);
        h_tenth.key(fcitx::Key(FcitxKey_grave));
        h_tenth.key(fcitx::Key(FcitxKey_0));
        FCITX_ASSERT(h_tenth.has_candidates());
        FCITX_ASSERT(h_tenth.candidate_count() == 20);
        FCITX_ASSERT(h_tenth.candidate(0) == "├");
        FCITX_ASSERT(h_tenth.candidate(19) == "╯");
        h_tenth.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(!h_tenth.has_candidates());

        EngineHarness h_category(instance);
        configure(h_category);
        h_category.key(fcitx::Key(FcitxKey_grave));
        h_category.key(fcitx::Key(FcitxKey_3));
        FCITX_ASSERT(h_category.has_candidates());
        FCITX_ASSERT(h_category.preedit().empty());
        FCITX_ASSERT(h_category.candidate(0) == "，");
        FCITX_ASSERT(h_category.candidate(1) == "、");
        FCITX_ASSERT(h_category.candidate_count() == 20);
        h_category.key(fcitx::Key(FcitxKey_Page_Down));
        FCITX_ASSERT(h_category.candidate_count() == 10);
        FCITX_ASSERT(h_category.candidate(0) == "‵");
        FCITX_ASSERT(h_category.candidate(9) == "＊");
        h_category.key(fcitx::Key(FcitxKey_Page_Up));
        FCITX_ASSERT(h_category.candidate_count() == 20);
        FCITX_ASSERT(h_category.candidate(0) == "，");
        h_category.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(!h_category.has_candidates());
        FCITX_ASSERT(h_category.preedit() == "，");
        h_category.expect_commit("，");

        EngineHarness h_back(instance);
        configure(h_back);
        h_back.key(fcitx::Key(FcitxKey_grave));
        h_back.key(fcitx::Key(FcitxKey_3));
        FCITX_ASSERT(h_back.candidate(0) == "，");
        h_back.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(h_back.candidate_count() == kLevel1Items.size());
        FCITX_ASSERT(h_back.candidate(0) == "…");
        h_back.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(!h_back.has_candidates());
        FCITX_ASSERT(h_back.preedit().empty());

        EngineHarness h_esc(instance);
        configure(h_esc);
        h_esc.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(h_esc.has_candidates());
        h_esc.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(!h_esc.has_candidates());
        FCITX_ASSERT(h_esc.preedit().empty());

        EngineHarness h_buffer(instance);
        configure(h_buffer);
        h_buffer.type("su3");
        FCITX_ASSERT(h_buffer.preedit() == "你");
        h_buffer.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(h_buffer.has_candidates());
        FCITX_ASSERT(h_buffer.candidate(0) == "…");
        FCITX_ASSERT(h_buffer.preedit() == "你");
        h_buffer.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(h_buffer.preedit() == "你…");
        h_buffer.expect_commit("你…");

        EngineHarness h_reading(instance);
        configure(h_reading);
        h_reading.type("su");
        FCITX_ASSERT(h_reading.preedit() == "ㄋㄧ");
        h_reading.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(!h_reading.has_candidates());
        FCITX_ASSERT(h_reading.preedit() == "ㄋㄧ");

        EngineHarness h_cursor(instance);
        configure(h_cursor);
        h_cursor.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(cursor_index(h_cursor) == 0);
        h_cursor.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(cursor_index(h_cursor) == 1);
        h_cursor.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(cursor_index(h_cursor) == 2);
        h_cursor.key(fcitx::Key(FcitxKey_Up));
        FCITX_ASSERT(cursor_index(h_cursor) == 1);
        FCITX_ASSERT(h_cursor.candidate(static_cast<size_t>(cursor_index(h_cursor))) == "※");
        h_cursor.key(fcitx::Key(FcitxKey_End));
        FCITX_ASSERT(cursor_index(h_cursor) == 12);
        h_cursor.key(fcitx::Key(FcitxKey_Home));
        FCITX_ASSERT(cursor_index(h_cursor) == 0);

        EngineHarness h_return(instance);
        configure(h_return);
        h_return.key(fcitx::Key(FcitxKey_grave));
        h_return.key(fcitx::Key(FcitxKey_Down));
        h_return.key(fcitx::Key(FcitxKey_Return));
        FCITX_ASSERT(!h_return.has_candidates());
        FCITX_ASSERT(h_return.preedit() == "※");
        h_return.expect_commit("※");
    });
}
