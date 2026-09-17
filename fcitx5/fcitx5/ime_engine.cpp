#include "fcitx5/ime_engine.hpp"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "context/accessibility_context.hpp"
#include "context/sample_adoption.hpp"
#include "debug/context_log.hpp"
#include "input/input_processor.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

const char* non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

std::string to_utf8(const std::u16string& value) {
    return u16_to_utf8(value);
}

std::string to_utf8(const std::u32string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char32_t codepoint : value) result += char32_to_utf8(codepoint);
    return result;
}

std::string accessibility_status_text(const AccessibilityContextState& state) {
    switch (state.availability) {
        case AccessibilityAvailability::Disabled:
            return "已停用";
        case AccessibilityAvailability::Unsupported:
            return "此平台不支援";
        case AccessibilityAvailability::Available:
            if (state.detail == "sample-file") return "樣本檔案: 可取得";
            if (state.detail == "atspi") return "AT-SPI: 可取得";
            return "無障礙: 可取得";
        case AccessibilityAvailability::Unavailable:
            if (state.detail == "libatspi-missing") return "AT-SPI: 不可用(未安裝 at-spi2-core)";
            if (state.detail == "a11y-bus-unavailable") {
                return "AT-SPI: 不可用(無法連線 a11y bus;請安裝或啟動 at-spi2-core)";
            }
            if (state.detail == "atspi-init-failed") return "AT-SPI: 不可用(初始化失敗)";
            if (state.detail == "atspi-listener-failed") return "AT-SPI: 不可用(無法註冊事件監聽)";
            if (state.detail == "atspi-loop-failed") return "AT-SPI: 不可用(事件迴圈建立失敗)";
            return state.detail.empty() ? "無障礙: 不可用" : "無障礙: 不可用(" + state.detail + ")";
    }
    return "無障礙: 未知";
}

std::u16string to_utf16(char32_t value) {
    return utf8_to_u16(char32_to_utf8(value));
}

std::filesystem::path default_table_path() {
    if (const char* override = non_empty_env("IME_FCITX5_TABLE_PATH")) return override;
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_path =
            std::filesystem::path(home) / "Library" / "fcitx5" / "share" / "llavon-ime" / "tables" /
            "bopomofo_char.json";
        if (std::filesystem::exists(user_path)) return user_path;
    }
#endif
#ifdef IME_FCITX5_SOURCE_TABLE_PATH
    const auto source_path = std::filesystem::path(IME_FCITX5_SOURCE_TABLE_PATH);
    if (std::filesystem::exists(source_path)) return source_path;
#endif
#ifdef IME_FCITX5_INSTALLED_TABLE_PATH
    const auto installed_path = std::filesystem::path(IME_FCITX5_INSTALLED_TABLE_PATH);
    if (std::filesystem::exists(installed_path)) return installed_path;
    return installed_path;
#endif
    return "/usr/share/llavon-ime/tables/bopomofo_char.json";
}

ServiceTransportOptions default_transport_options() {
    ServiceTransportOptions options;
    const auto config = load_config();
    options.tables_dir = default_table_path().parent_path();
    options.model_path = config.model_path;
    options.context_length = static_cast<std::uint32_t>(config.context_length);
    options.threads = static_cast<std::uint32_t>(config.thread_count);
    options.gpu_layers = config.gpu_layers;
    options.idle_timeout_seconds = static_cast<std::uint32_t>(config.idle_timeout_seconds);
    if (const char* model = non_empty_env("IME_FCITX5_MODEL_PATH")) options.model_path = model;
    if (non_empty_env("IME_FCITX5_DISABLE_SERVICE") != nullptr) options.auto_start = false;
    return options;
}

// Builds the current composition as a sequence of per-segment states and
// removes it from the tail of an accessibility sample.
std::optional<std::u16string> strip_accessibility_preedit(const InputSession& session,
                                                          const std::u16string& sample) {
    std::vector<std::pair<std::u16string, std::u16string>> storage;
    for (const auto& segment : session.buffer.segments()) {
        if (segment.empty()) continue;
        storage.emplace_back(segment.rendered_text(), segment.reading());
    }
    if (!session.pending_token.empty()) {
        storage.emplace_back(InputProcessor::pending_rendered_text(session), session.pending_token.raw);
    }

    std::vector<PreeditSegmentState> states;
    states.reserve(storage.size());
    for (const auto& [rendered, reading] : storage) states.push_back({rendered, reading});
    return strip_preedit_suffix(sample, states);
}

class SelectableCandidateWord final : public fcitx::CandidateWord {
public:
    SelectableCandidateWord(fcitx::Text text, std::function<void(fcitx::InputContext*)> callback)
        : CandidateWord(std::move(text)), callback_(std::move(callback)) {}

    void select(fcitx::InputContext* input_context) const override {
        callback_(input_context);
    }

private:
    std::function<void(fcitx::InputContext*)> callback_;
};

}  // namespace

ImeEngine::StateScope::StateScope(ImeEngine& engine, fcitx::InputContext* input_context) : engine_(engine) {
    if (input_context != nullptr) {
        engine_.enter_context(input_context);
        entered_ = true;
    }
}

ImeEngine::StateScope::~StateScope() {
    if (entered_) engine_.leave_context();
}

ImeInputContextProperty* ImeEngine::property(fcitx::InputContext* input_context) const {
    if (input_context == nullptr) return nullptr;
    return static_cast<ImeInputContextProperty*>(input_context->property(&property_factory_));
}

void ImeEngine::enter_context(fcitx::InputContext* input_context) {
    if (state_scope_depth_++ != 0) return;
    active_input_context_ = input_context;
    auto* state = property(input_context);
    if (state == nullptr) return;

    session_ = state->session;
    session_.context_cache.set_limit(static_cast<size_t>(config_.context_history_limit));
    session_.context_cache.set_surrounding_limit(static_cast<size_t>(config_.context_length));
}

void ImeEngine::leave_context() {
    if (state_scope_depth_ == 0) return;
    if (--state_scope_depth_ != 0) return;
    auto* state = property(active_input_context_);
    if (state != nullptr) {
        state->session = session_;
    }
    active_input_context_ = nullptr;
}

ImeEngine::ImeEngine(fcitx::Instance* instance)
    : fallback_(default_table_path()),
      decoder_([this](std::u16string_view reading) { return fallback_.lookup(reading); },
               [this](std::u16string_view word) { return fallback_.latin_frequency(word); }),
      phrase_overrides_(phrase_overrides_path()),
      processor_(fallback_, decoder_, phrase_overrides_),
      service_transport_(default_transport_options()),
      coordinator_(service_transport_, processor_,
                   [dispatcher = instance ? &instance->eventDispatcher() : nullptr](
                       PredictionCoordinator::ContextReference context,
                       std::function<void(fcitx::InputContext*)> body) {
                       if (dispatcher == nullptr) return;
                       dispatcher->scheduleWithContext(context, [context, body = std::move(body)]() mutable {
                           if (auto* input_context = context.get()) body(input_context);
                       });
                   },
                   alive_,
                   PredictionCoordinator::Callbacks{
                       [this]() -> const Config& { return config_; },
                       [this](fcitx::InputContext* input_context, InputSession& session) {
                           resync_context_cache(input_context, session);
                       },
                       [this](fcitx::InputContext* input_context, const std::function<void(InputSession&)>& body) {
                           StateScope state_scope(*this, input_context);
                           body(session_);
                       },
                       [this](fcitx::InputContext* input_context) { update_ui(input_context); },
                       [this](fcitx::InputContext* input_context) { return property(input_context); },
                   }),
      config_(default_config()),
      instance_(instance),
      event_dispatcher_(instance ? &instance->eventDispatcher() : nullptr) {
    processor_.set_state_observer([this](InputStateKind previous, InputStateKind next) {
        if (accessibility_context_ == nullptr) return;
        if (previous == InputStateKind::Empty && next == InputStateKind::Inputting) {
            // Samples published from here on may contain this composition's
            // preedit; earlier ones cannot.
            accessibility_composition_base_ = accessibility_context_->sequence();
        } else if (next == InputStateKind::Empty) {
            accessibility_composition_base_ = 0;
        }
    });

    if (instance_ != nullptr) {
        (void)instance_->inputContextManager().registerProperty("llavon-ime-input-state", &property_factory_);
        capability_changed_handler_ = instance_->watchEvent(
            fcitx::EventType::InputContextCapabilityAboutToChange, fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event& event) {
                const auto& capability_event = static_cast<const fcitx::CapabilityEvent&>(event);
                if (capability_event.newFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
                    if (auto* state = property(capability_event.inputContext())) state->session.context_cache.clear();
                }
            });
    }
    reload_config();
}

ImeEngine::~ImeEngine() {
    alive_.reset();
    // Transport callbacks hold a raw coordinator pointer after checking the
    // lifetime token. Drain them while the coordinator is still alive.
    service_transport_.stop();
}

void ImeEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) {
    if (event.isRelease()) return;

    auto* input_context = event.inputContext();
    StateScope state_scope(*this, input_context);
    const auto raw_key = event.rawKey();
    const fcitx::Key effective_key(event.key().sym(),
                                   event.key().states() | (raw_key.states() & fcitx::KeyState::Meta),
                                   event.key().code());
    InputKey input_key;
    input_key.sym = static_cast<char32_t>(effective_key.sym());
    input_key.states = static_cast<std::uint32_t>(effective_key.states());
    input_key.frontend_states = static_cast<std::uint32_t>(event.key().states());
    input_key.raw_states = static_cast<std::uint32_t>(raw_key.states());
    input_key.caps_lock = static_cast<bool>(raw_key.states() & fcitx::KeyState::CapsLock);
    input_key.release = event.isRelease();

    const auto effect = processor_.process(input_key, session_, config_);
    apply_effect(input_context, effect);
    if (effect.handled) event.filterAndAccept();
}

void ImeEngine::activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    StateScope state_scope(*this, event.inputContext());
    reload_config();
    if (accessibility_context_) {
        accessibility_context_->set_active(true);
        accessibility_base_sequence_ = accessibility_context_->sequence();
        accessibility_context_->refresh();
    }
    update_ui(event.inputContext());
}

void ImeEngine::deactivate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) {
    if (accessibility_context_) {
        accessibility_context_->set_active(false);
        accessibility_base_sequence_ = accessibility_context_->sequence();
    }
    reset(entry, event);
}

void ImeEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    StateScope state_scope(*this, event.inputContext());
    const bool focus_out = event.type() == fcitx::EventType::InputContextFocusOut;
    const auto reason = focus_out ? InputResetReason::FocusOut
                                 : event.type() == fcitx::EventType::InputContextReset
                                       ? InputResetReason::Explicit
                                       : InputResetReason::Deactivate;
    const bool sensitive =
        event.inputContext()->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive);
    const auto effect = processor_.reset(session_, config_, reason,
                                         sensitive || (focus_out && config_.reset_context_on_focus_out));
    if (focus_out && accessibility_context_) {
        accessibility_context_->set_active(false);
        accessibility_base_sequence_ = accessibility_context_->sequence();
    }
    apply_effect(event.inputContext(), effect);
}

void ImeEngine::reloadConfig() {
    reload_config();
}

void ImeEngine::save() {
    // The default INI location is PkgConfig, matching shared config_path().
    // The accessibility status is informational and must not be persisted.
    const std::string status = *fcitx_config_.accessibilityStatus;
    (void)fcitx_config_.accessibilityStatus.setValue(std::string());
    fcitx::safeSaveAsIni(fcitx_config_, kFcitxConfigFile);
    (void)fcitx_config_.accessibilityStatus.setValue(status);
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &fcitx_config_;
}

void ImeEngine::refresh_phrase_override_editor() const {
    auto* entries = phrase_override_editor_.entries.mutableValue();
    entries->clear();
    for (const auto& record : phrase_overrides_.entries()) {
        PunctuationMapEntryConfig entry;
        (void)entry.phrase.setValue(u16_to_utf8(record.phrase));
        (void)entry.readings.setValue(PhraseOverrideStore::format_readings(record.readings));
        entries->emplace_back(std::move(entry));
    }
}

const fcitx::Configuration* ImeEngine::getSubConfig(const std::string& path) const {
    if (path != "phraseoverrides") return nullptr;

    refresh_phrase_override_editor();
    return &phrase_override_editor_;
}

void ImeEngine::setSubConfig(const std::string& path, const fcitx::RawConfig& config) {
    if (path != "phraseoverrides") return;
    // An absent Entries list is not authoritative: frontends also send empty
    // configs as action triggers, and silently wiping every saved phrase would
    // be unrecoverable. Removing all entries is done by editing the file.
    if (!config.get("Entries")) return;

    PhraseOverrideEditorConfig editor;
    editor.load(config, true);
    std::vector<PhraseOverrideRecord> records;
    records.reserve(editor.entries->size());
    for (const auto& entry : *editor.entries) {
        // The dialog has no error channel, so one malformed row must not
        // discard the edits the user made to every other row. Reusing the file
        // parser keeps the accepted readings identical to the on-disk format.
        const auto record = PhraseOverrideStore::parse_line(*entry.phrase + " " + *entry.readings);
        if (!record || !PhraseOverrideStore::valid_entry(record->phrase, record->readings.size())) continue;
        records.push_back(*record);
    }
    (void)phrase_overrides_.replace(records);
}

void ImeEngine::setConfig(const fcitx::RawConfig& config) {
    fcitx_config_.load(config, true);
    (void)fcitx_config_.version.setValue(DisplayVersion::Current);
    config_ = to_shared_config(fcitx_config_);
    apply_context_cache_limits();
    apply_context_sources();
    save();
    processor_.prepare_for_config_change(session_);
    if (instance_ != nullptr) {
        instance_->inputContextManager().foreach([this](fcitx::InputContext* input_context) {
            auto* state = property(input_context);
            if (state == nullptr) return true;
            processor_.prepare_for_config_change(state->session);
            coordinator_.close_session(state->session);
            state->session_close_handle = {};
            return true;
        });
    }
}

void ImeEngine::reload_config() {
    fcitx_config_ = ImeFcitxConfig();
    try {
        fcitx::readAsIni(fcitx_config_, kFcitxConfigFile);
    } catch (...) {
        fcitx_config_ = ImeFcitxConfig();
    }

    std::error_code ec;
    const bool has_fcitx_config = std::filesystem::exists(config_path(), ec) && !ec;
    apply_shared_config(fcitx_config_, load_config());
    if (!has_fcitx_config) save();
    config_ = to_shared_config(fcitx_config_);
    (void)phrase_overrides_.load();
    apply_context_cache_limits();
    apply_context_sources();
}

void ImeEngine::apply_context_cache_limits() {
    const auto history_limit = static_cast<size_t>(config_.context_history_limit);
    const auto surrounding_limit = static_cast<size_t>(config_.context_length);
    session_.context_cache.set_limit(history_limit);
    session_.context_cache.set_surrounding_limit(surrounding_limit);

    if (instance_ == nullptr) return;
    instance_->inputContextManager().foreach([this, history_limit, surrounding_limit](fcitx::InputContext* input_context) {
        auto* state = property(input_context);
        if (state == nullptr) return true;
        state->session.context_cache.set_limit(history_limit);
        state->session.context_cache.set_surrounding_limit(surrounding_limit);
        return true;
    });
}

void ImeEngine::apply_context_sources() {
#ifdef IME_FCITX5_NATIVE_SURROUNDING
    if (accessibility_context_) {
        accessibility_context_->stop();
        accessibility_context_.reset();
    }
    accessibility_max_code_units_ = 0;
    accessibility_base_sequence_ = 0;
    (void)fcitx_config_.accessibilityStatus.setValue("InputMethodKit: 可取得（不需輔助使用權限）");
    return;
#endif

    const size_t limit = static_cast<size_t>(std::max(1, config_.context_length));
    if (accessibility_context_ && accessibility_max_code_units_ != limit) {
        accessibility_context_->stop();
        accessibility_context_.reset();
        accessibility_max_code_units_ = 0;
        accessibility_base_sequence_ = 0;
    }
    if (!accessibility_context_) {
        accessibility_context_ = create_accessibility_context_provider(limit);
        accessibility_max_code_units_ = limit;
        accessibility_base_sequence_ = 0;
    }
    (void)accessibility_context_->start();
    update_accessibility_status();
}

void ImeEngine::update_accessibility_status() {
    const AccessibilityContextState state = accessibility_context_ ? accessibility_context_->availability()
                                                                   : AccessibilityContextState{};
    const std::string status = accessibility_status_text(state);
    if (*fcitx_config_.accessibilityStatus == status) return;
    (void)fcitx_config_.accessibilityStatus.setValue(status);
}

void ImeEngine::apply_effect(fcitx::InputContext* input_context, const InputEffect& effect) {
    if (!effect.commit.empty()) {
        input_context->commitString(to_utf8(effect.commit));
        record_context_commit(input_context, effect.commit);
    }
    if (effect.request_prediction) request_prediction_if_ready(input_context);
    if (effect.redraw) update_ui(input_context);
}

void ImeEngine::run_effect(fcitx::InputContext* input_context, const std::function<InputEffect()>& operation) {
    StateScope state_scope(*this, input_context);
    apply_effect(input_context, operation());
}

void ImeEngine::update_ui(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    processor_.sync_state(session_, config_);

    if (InputProcessor::composition_empty(session_)) {
        input_context->inputPanel().reset();
        input_context->updatePreedit();
        if (!session_.symbol_menu.active()) {
            input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            return;
        }
    } else {
        auto rendered = InputProcessor::current_preedit(session_);
        auto prefix = session_.buffer.rendered_prefix_before_caret();
        if (!session_.pending_token.empty()) prefix += InputProcessor::pending_rendered_text(session_);
        // Frontends that render preedit formatting underline the marked range;
        // the rest still show the caret at the marking edge.
        fcitx::Text preedit;
        if (const auto marked = session_.buffer.marked_range()) {
            for (size_t i = 0; i < session_.buffer.segments().size(); ++i) {
                preedit.append(to_utf8(session_.buffer.segments()[i].rendered_text()),
                               i >= marked->first && i < marked->second ? fcitx::TextFormatFlag::Underline
                                                                        : fcitx::TextFormatFlag::NoFlag);
            }
            if (!session_.pending_token.empty()) {
                preedit.append(to_utf8(InputProcessor::pending_rendered_text(session_)));
            }
        } else {
            preedit = fcitx::Text(to_utf8(rendered));
        }
        preedit.setCursor(static_cast<int>(to_utf8(prefix).size()));
        const bool use_client_preedit = input_context->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
        input_context->inputPanel().setClientPreedit(use_client_preedit ? preedit : fcitx::Text());
        input_context->inputPanel().setPreedit(use_client_preedit ? fcitx::Text() : preedit);
        if (session_.buffer.marked_range()) {
            input_context->inputPanel().setAuxUp(fcitx::Text(to_utf8(InputProcessor::marking_hint_text(session_))));
        } else {
            input_context->inputPanel().setAuxUp(fcitx::Text());
        }
        input_context->inputPanel().setAuxDown(fcitx::Text());
        input_context->updatePreedit();
    }

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    if (session_.mixed_decision.active() && session_.choosing_candidate()) {
        session_.displayed_candidates.clear();
        const auto entries = decoder_.expand_candidates(
            session_.mixed_decision.result, InputProcessor::candidate_page_size(session_, config_),
            session_.mixed_decision.preview_path);
        for (const auto& entry : entries) session_.displayed_candidates.push_back(entry.text);
    } else if (session_.symbol_menu.active()) {
        // Candidates are rendered from symbol menu items below; the placeholder
        // entries keep page and cursor bookkeeping sized identically.
        session_.displayed_candidates.assign(session_.symbol_menu.menu().size(), u"?");
    } else if (session_.buffer.marked_range() && !session_.choosing_candidate()) {
        // The marking hint doubles as the tooltip McBopomofo shows next to the
        // composing buffer, because the macOS frontend can only render it as a
        // candidate list.
        session_.displayed_candidates.assign(1, InputProcessor::marking_hint_text(session_));
    } else {
        session_.displayed_candidates.clear();
        if (session_.choosing_candidate()) {
            for (const char32_t candidate : InputProcessor::available_candidates(session_, config_)) {
                session_.displayed_candidates.push_back(to_utf16(candidate));
            }
        }
    }
    if (session_.displayed_candidates.empty()) {
        session_.candidate_view.reset();
        input_context->inputPanel().setCandidateList(nullptr);
        input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    InputProcessor::clamp_candidate_cursor(session_, config_);
    const int page_size = InputProcessor::candidate_page_size(session_, config_);
    const int page_count = static_cast<int>(
        (session_.displayed_candidates.size() + static_cast<size_t>(page_size) - 1) / static_cast<size_t>(page_size));
    if (session_.candidate_view.page >= page_count) session_.candidate_view.page = page_count - 1;
    if (session_.candidate_view.page < 0) session_.candidate_view.page = 0;
    candidates->setPageSize(page_size);
    candidates->setSelectionKey(selection_key_list());
    candidates->setLayoutHint(candidate_layout_hint());
    const auto target =
        session_.symbol_menu.active() ? std::optional<size_t>() : InputProcessor::current_candidate_target(session_, config_);
    const auto symbol_epoch = session_.symbol_menu.epoch();
    int index = 0;
    if (session_.symbol_menu.active()) {
        for (const auto& item : session_.symbol_menu.menu()) {
            candidates->append<SelectableCandidateWord>(
                fcitx::Text(to_utf8(item)), [this, index, symbol_epoch](fcitx::InputContext* context) {
                    run_effect(context, [this, index, symbol_epoch]() {
                        return processor_.select_symbol(session_, config_, index, symbol_epoch);
                    });
                });
            ++index;
        }
    } else if (session_.buffer.marked_range() && !session_.choosing_candidate()) {
        // The marking hint is informational: clicking it must not pick a
        // candidate behind the user's back.
        candidates->append<SelectableCandidateWord>(fcitx::Text(to_utf8(session_.displayed_candidates.front())),
                                                    [](fcitx::InputContext*) {});
    } else {
        for (const auto& candidate : session_.displayed_candidates) {
            candidates->append<SelectableCandidateWord>(
                fcitx::Text(to_utf8(candidate)), [this, index](fcitx::InputContext* context) {
                    run_effect(context, [this, index]() {
                        return processor_.select_candidate(session_, config_, index);
                    });
                });
            ++index;
        }
    }
    candidates->setPage(session_.candidate_view.page);
    if (session_.symbol_menu.active() || session_.mixed_decision.active() || target) {
        candidates->setCursorIndex(session_.candidate_view.cursor -
                                   InputProcessor::candidate_page_offset(session_, config_));
    }
    input_context->inputPanel().setCandidateList(std::move(candidates));
    input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ImeEngine::record_context_commit(const fcitx::InputContext* input_context, const std::u16string& text) {
    if (input_context == nullptr ||
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        session_.context_cache.clear();
        return;
    }
    if (config_.context_history_limit > 0 && !text.empty()) {
        session_.context_cache.on_commit(text);
    }
}

void ImeEngine::request_prediction_if_ready(fcitx::InputContext* input_context) {
    processor_.apply_phrase_override(session_);
    coordinator_.request(input_context, session_);
}

void ImeEngine::resync_context_cache(fcitx::InputContext* input_context, InputSession& session) {
    if (input_context == nullptr ||
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        session.context_cache.clear();
        return;
    }

    // Some clients report an empty (but valid) document, notably Electron,
    // Chromium and terminals. An empty client prefix must not shadow the
    // accessibility sample, which may still hold the focused widget's text.
    bool client_empty = false;
    const auto& surrounding = input_context->surroundingText();
    if (surrounding.isValid()) {
        try {
            const size_t cursor = std::min(surrounding.cursor(), surrounding.anchor());
            const size_t limit = session.context_cache.limit() > 0 ? session.context_cache.limit()
                                                                   : session.context_cache.surrounding_limit();
            const auto text = utf8_prefix_tail(surrounding.text(), cursor, limit);
            if (!text.empty()) {
                session.client_surrounding_authoritative = true;
                session.context_cache.on_surrounding(text, text.size());
                log_context("client-surrounding", text);
                return;
            }
            client_empty = true;
        } catch (const std::runtime_error&) {
            // Ignore malformed surrounding text supplied by a client.
        }
    }

    if (accessibility_context_) {
        const auto sample = accessibility_context_->latest();
        if (sample && sample->usable && sample->sequence > accessibility_base_sequence_) {
            // A sample published before this composition started cannot
            // contain its preedit; anything newer may, so it is only adopted
            // after the composing text is stripped from its tail.
            const bool predates_composition =
                accessibility_composition_base_ != 0 && sample->sequence <= accessibility_composition_base_;
            const bool may_contain_preedit = !InputProcessor::composition_empty(session) && !predates_composition;
            std::optional<std::u16string> text;
            if (!may_contain_preedit) {
                text = sample->text;
            } else {
                text = strip_accessibility_preedit(session, sample->text);
            }
            if (text && (!text->empty() || !may_contain_preedit || !session.context_cache.valid())) {
                session.context_cache.on_surrounding(*text, text->size());
                log_context("accessibility", *text);
                return;
            }
            log_context(text ? "accessibility-empty-cache-fallback" : "accessibility-preedit-mismatch",
                        sample->text);
        } else {
            log_context("accessibility-unusable", {});
        }
    }

    if (client_empty) {
        if (session.client_surrounding_authoritative) {
            session.context_cache.on_surrounding(std::u16string_view(), 0);
            log_context("client-surrounding-empty", {});
        } else {
            // Some clients always expose a valid but empty document. Preserve
            // commits until that client demonstrates usable surrounding text.
            log_context(session.context_cache.valid() ? "client-empty-cache-fallback" : "client-surrounding-empty",
                        {});
        }
        return;
    }
    log_context("cache-fallback", {});
}

fcitx::KeyList ImeEngine::selection_key_list() const {
    fcitx::KeyList keys;
    const int count = std::min(config_.selection_key_count, static_cast<int>(config_.selection_keys.size()));
    keys.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        keys.emplace_back(
            static_cast<fcitx::KeySym>(static_cast<unsigned char>(config_.selection_keys[static_cast<size_t>(i)])));
    }
    return keys;
}

fcitx::CandidateLayoutHint ImeEngine::candidate_layout_hint() const {
    if (config_.candidate_layout == "vertical") return fcitx::CandidateLayoutHint::Vertical;
    if (config_.candidate_layout == "horizontal") return fcitx::CandidateLayoutHint::Horizontal;
    return fcitx::CandidateLayoutHint::NotSet;
}

fcitx::AddonInstance* ImeEngineFactory::create(fcitx::AddonManager* manager) {
    return new ImeEngine(manager ? manager->instance() : nullptr);
}

}  // namespace ime::fcitx5

FCITX_ADDON_FACTORY(ime::fcitx5::ImeEngineFactory)
