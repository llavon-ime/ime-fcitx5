#pragma once

#include "fcitx5/input_context_property.hpp"
#include "testdir.h"

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <testfrontend_public.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace ime::fcitx5::test {

// Test harness driving the llavon-ime addon through fcitx5's TestFrontend.
// Construct inside the event loop (i.e. from a scheduled lambda).
class EngineHarness {
public:
    explicit EngineHarness(fcitx::Instance* instance) : instance_(instance) { setup(); }

    // Destroys the input context shortly after; resetting first clears any
    // leftover composition so the engine's focus-out switch does not commit it
    // unexpectedly (each scenario must still commit or clear its own text).
    ~EngineHarness() {
        if (auto* context = input_context()) context->reset();
    }

    // Switches the input context to llavon-ime (trigger key cycles the group).
    void activate() {
        if (instance_->inputMethod(input_context()) == "llavon-ime") return;
        key(fcitx::Key("Control+space"));
        if (instance_->inputMethod(input_context()) != "llavon-ime") {
            key(fcitx::Key("Control+space"));
        }
    }

    // Applies addon config values together. Addon setConfig() treats a partial
    // RawConfig as a replacement, so start from the complete current config.
    void set_configs(std::initializer_list<std::pair<std::string, std::string>> values) {
        auto* addon = instance_->addonManager().addon("llavon-ime");
        fcitx::RawConfig config;
        if (const auto* current = addon->getConfig()) current->save(config);
        for (const auto& [path, value] : values) config.setValueByPath(path, value);
        addon->setConfig(config);
    }

    // Applies an addon config value by path, e.g. set_config("BopomofoKeyboardLayout", "許氏").
    void set_config(const std::string& path, const std::string& value) {
        set_configs({{path, value}});
    }

    // Sends a single key event (press).
    void key(const fcitx::Key& key) {
        testfrontend_->call<fcitx::ITestFrontend::sendKeyEvent>(uuid_, key, false);
    }

    // Sends a key event and reports whether the engine accepted it. Boundary
    // navigation keys are deliberately passed through (not accepted) so the
    // macOS frontend keeps the panel/preedit untouched.
    bool key_accepted(const fcitx::Key& key) {
        return testfrontend_->call<fcitx::ITestFrontend::sendKeyEvent>(uuid_, key, false);
    }

    // Types a sequence of printable characters.
    void type(std::string_view text) {
        for (const char ch : text) {
            key(fcitx::Key(static_cast<fcitx::KeySym>(ch)));
        }
    }

    // Commits the preedit with Return and asserts the committed text.
    void expect_commit(std::string_view text) {
        testfrontend_->call<fcitx::ITestFrontend::pushCommitExpectation>(std::string(text));
        key(fcitx::Key(FcitxKey_Return));
    }

    // Pushes a commit expectation, then presses a key that is expected to
    // commit directly (e.g. Shift+letter English output), without a trailing
    // Return.
    void expect_direct_commit(std::string_view text, const fcitx::Key& key) {
        testfrontend_->call<fcitx::ITestFrontend::pushCommitExpectation>(std::string(text));
        this->key(key);
    }

    void expect_focus_out_commit(std::string_view text) {
        testfrontend_->call<fcitx::ITestFrontend::pushCommitExpectation>(std::string(text));
        input_context()->focusOut();
    }

    std::string preedit() const {
        return input_context()->inputPanel().preedit().toString();
    }

    bool has_candidates() const {
        return input_context()->inputPanel().candidateList() != nullptr &&
               !input_context()->inputPanel().candidateList()->empty();
    }

    size_t candidate_count() const {
        if (!has_candidates()) return 0;
        return input_context()->inputPanel().candidateList()->size();
    }

    std::string candidate(size_t index) const {
        if (!has_candidates()) return {};
        const auto& list = input_context()->inputPanel().candidateList();
        if (index >= list->size()) return {};
        return list->candidate(index).text().toString();
    }

    fcitx::InputContext* input_context() const {
        return instance_->inputContextManager().findByUUID(uuid_);
    }

    // Simulates the client pushing surrounding text to fcitx5. `cursor` and
    // `anchor` are character offsets into the text (fcitx5 counts in
    // characters, so ASCII text is 1:1).
    void set_surrounding(std::string_view text, size_t cursor, size_t anchor) {
        auto* context = input_context();
        context->surroundingText().setText(std::string(text), static_cast<unsigned int>(cursor),
                                           static_cast<unsigned int>(anchor));
        context->updateSurroundingText();
    }

    // The per-input-context engine state registered by the addon.
    ime::fcitx5::ImeInputContextProperty* engine_state() const {
        return static_cast<ime::fcitx5::ImeInputContextProperty*>(
            input_context()->property("llavon-ime-input-state"));
    }

    fcitx::Instance* instance() const { return instance_; }

private:
    void setup() {
        (void)instance_->addonManager().addon("llavon-ime", true);

        auto default_group = instance_->inputMethodManager().currentGroup();
        default_group.inputMethodList().clear();
        default_group.inputMethodList().push_back(fcitx::InputMethodGroupItem("keyboard-us"));
        default_group.inputMethodList().push_back(fcitx::InputMethodGroupItem("llavon-ime"));
        default_group.setDefaultInputMethod("");
        instance_->inputMethodManager().setGroup(default_group);

        testfrontend_ = instance_->addonManager().addon("testfrontend");
        uuid_ = testfrontend_->call<fcitx::ITestFrontend::createInputContext>("testapp");
        activate();
    }

    fcitx::Instance* instance_;
    fcitx::AddonInstance* testfrontend_ = nullptr;
    fcitx::ICUUID uuid_{};
};

}  // namespace ime::fcitx5::test
