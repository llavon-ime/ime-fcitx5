#include "raw_key_harness.hpp"
#include <vector>

using namespace llavon::ime::rawkey;

namespace {

const std::vector<std::string> kLevel1Items = {"…", "※",     "常用符號", "左右括號", "上下括號", "希臘字母", "數學符號",
                                               "特殊圖形", "Unicode", "單線框",   "雙線框",   "填色方塊", "線段"};

void configure(Harness& harness) {
    harness.set_config("CandidatePageSize", "20");
    harness.set_config("SelectionKeysCount", "10");
    harness.set_config("ChooseCandidateUsingSpace", "True");
}

}  // namespace

RAWKEY_SUITE("symbol menu tests", engine_test_symbol_menu_tests) {
    {
        Harness harness;
        configure(harness);

        harness.key(Key("`"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit().empty());
        RAWKEY_ASSERT(harness.candidate_count() == kLevel1Items.size());
        for (size_t i = 0; i < kLevel1Items.size(); ++i) {
            RAWKEY_ASSERT(harness.candidate(i) == kLevel1Items[i]);
        }
        RAWKEY_ASSERT(harness.cursor_index() == 0);

        Harness h_direct;
        configure(h_direct);
        h_direct.key(Key("`"));
        h_direct.key(Key("1"));
        RAWKEY_ASSERT(!h_direct.has_candidates());
        RAWKEY_ASSERT(h_direct.preedit() == "…");
        h_direct.expect_commit("…");

        Harness h_space;
        configure(h_space);
        h_space.key(Key("`"));
        h_space.key(Key(" "));
        RAWKEY_ASSERT(!h_space.has_candidates());
        RAWKEY_ASSERT(h_space.preedit() == "…");
        h_space.expect_commit("…");

        Harness h_tenth;
        configure(h_tenth);
        h_tenth.key(Key("`"));
        h_tenth.key(Key("0"));
        RAWKEY_ASSERT(h_tenth.has_candidates());
        RAWKEY_ASSERT(h_tenth.candidate_count() == 20);
        RAWKEY_ASSERT(h_tenth.candidate(0) == "├");
        RAWKEY_ASSERT(h_tenth.candidate(19) == "╯");
        h_tenth.key(Key("Escape"));
        RAWKEY_ASSERT(!h_tenth.has_candidates());

        Harness h_category;
        configure(h_category);
        h_category.key(Key("`"));
        h_category.key(Key("3"));
        RAWKEY_ASSERT(h_category.has_candidates());
        RAWKEY_ASSERT(h_category.preedit().empty());
        RAWKEY_ASSERT(h_category.candidate(0) == "，");
        RAWKEY_ASSERT(h_category.candidate(1) == "、");
        RAWKEY_ASSERT(h_category.candidate_count() == 20);
        h_category.key(Key("Page_Down"));
        RAWKEY_ASSERT(h_category.candidate_count() == 10);
        RAWKEY_ASSERT(h_category.candidate(0) == "‵");
        RAWKEY_ASSERT(h_category.candidate(9) == "＊");
        h_category.key(Key("Page_Up"));
        RAWKEY_ASSERT(h_category.candidate_count() == 20);
        RAWKEY_ASSERT(h_category.candidate(0) == "，");
        h_category.key(Key("1"));
        RAWKEY_ASSERT(!h_category.has_candidates());
        RAWKEY_ASSERT(h_category.preedit() == "，");
        h_category.expect_commit("，");

        Harness h_back;
        configure(h_back);
        h_back.key(Key("`"));
        h_back.key(Key("3"));
        RAWKEY_ASSERT(h_back.candidate(0) == "，");
        h_back.key(Key("BackSpace"));
        RAWKEY_ASSERT(h_back.candidate_count() == kLevel1Items.size());
        RAWKEY_ASSERT(h_back.candidate(0) == "…");
        h_back.key(Key("BackSpace"));
        RAWKEY_ASSERT(!h_back.has_candidates());
        RAWKEY_ASSERT(h_back.preedit().empty());

        Harness h_esc;
        configure(h_esc);
        h_esc.key(Key("`"));
        RAWKEY_ASSERT(h_esc.has_candidates());
        h_esc.key(Key("Escape"));
        RAWKEY_ASSERT(!h_esc.has_candidates());
        RAWKEY_ASSERT(h_esc.preedit().empty());

        Harness h_buffer;
        configure(h_buffer);
        h_buffer.type("su3");
        RAWKEY_ASSERT(h_buffer.preedit() == "你");
        h_buffer.key(Key("`"));
        RAWKEY_ASSERT(h_buffer.has_candidates());
        RAWKEY_ASSERT(h_buffer.candidate(0) == "…");
        RAWKEY_ASSERT(h_buffer.preedit() == "你");
        h_buffer.key(Key("1"));
        RAWKEY_ASSERT(h_buffer.preedit() == "你…");
        h_buffer.expect_commit("你…");

        Harness h_reading;
        configure(h_reading);
        h_reading.type("su");
        RAWKEY_ASSERT(h_reading.preedit() == "ㄋㄧ");
        h_reading.key(Key("`"));
        RAWKEY_ASSERT(!h_reading.has_candidates());
        RAWKEY_ASSERT(h_reading.preedit() == "ㄋㄧ");

        Harness h_cursor;
        configure(h_cursor);
        h_cursor.key(Key("`"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 0);
        h_cursor.key(Key("Down"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 1);
        h_cursor.key(Key("Down"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 2);
        h_cursor.key(Key("Up"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 1);
        RAWKEY_ASSERT(h_cursor.candidate(static_cast<size_t>(h_cursor.cursor_index())) == "※");
        h_cursor.key(Key("End"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 12);
        h_cursor.key(Key("Home"));
        RAWKEY_ASSERT(h_cursor.cursor_index() == 0);

        Harness h_return;
        configure(h_return);
        h_return.key(Key("`"));
        h_return.key(Key("Down"));
        h_return.key(Key("Return"));
        RAWKEY_ASSERT(!h_return.has_candidates());
        RAWKEY_ASSERT(h_return.preedit() == "※");
        h_return.expect_commit("※");
}
}
