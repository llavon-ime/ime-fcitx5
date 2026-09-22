#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("punctuation", punctuation) {
    {
        Harness harness;
        harness.type("[");
        harness.expect_commit("「");
        harness.type("]");
        harness.expect_commit("」");
        harness.key("?");
        harness.expect_commit("？");
        harness.key("{");
        harness.expect_commit("『");
        harness.key("}");
        harness.expect_commit("』");
        harness.key(":");
        harness.expect_commit("：");
        harness.key("\"");
        harness.expect_commit("；");
    }

    {
        // The comma key is a bopomofo letter in the standard layout.
        Harness harness;
        harness.type(",");
        RAWKEY_ASSERT(harness.preedit() == "ㄝ");
    }

    {
        Harness harness;
        harness.type("su3");
        harness.type("[");
        harness.expect_commit("你「");
    }

    {
        // Ctrl+punctuation uses the Microsoft key map.
        Harness harness;
        harness.key(Key("!").with(kCtrl));
        harness.expect_commit("！");
        harness.key(Key("'").with(kCtrl));
        harness.expect_commit("、");
        harness.key(Key(",").with(kCtrl));
        harness.expect_commit("，");
        harness.key(Key(".").with(kCtrl));
        harness.expect_commit("。");
        harness.key(Key("/").with(kCtrl));
        harness.expect_commit("？");
        harness.key(Key(";").with(kCtrl));
        harness.expect_commit("；");
    }

    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");

        harness.type(",");
        harness.expect_commit(",");
        harness.key("<");
        harness.expect_commit("，");
        harness.type("[");
        harness.expect_commit("[");
        harness.key("{");
        harness.expect_commit("『");
        harness.type(";");
        harness.expect_commit(";");
        harness.key(":");
        harness.expect_commit("：");
        harness.type("'");
        harness.expect_commit("'");
        harness.key("\"");
        harness.expect_commit("；");
        harness.type("b");
        RAWKEY_ASSERT(harness.preedit() == "ㄅ");
    }
}
