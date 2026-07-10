#pragma once

#include "config/config.hpp"

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>

namespace ime::fcitx5 {

inline constexpr const char* kFcitxConfigFile = "conf/llavon-ime.conf";

enum class BopomofoKeyboardLayout { Standard };
FCITX_CONFIG_ENUM_NAME(BopomofoKeyboardLayout, "標準");

enum class SelectionKeys { Digits, HomeRow, LeftHand };
FCITX_CONFIG_ENUM_NAME(SelectionKeys, "數字鍵", "本位列", "左手鍵");

enum class CandidateLayout { NotSet, Vertical, Horizontal };
FCITX_CONFIG_ENUM_NAME(CandidateLayout, "系統預設", "垂直", "水平");

enum class SelectPhrase { BeforeCursor, AfterCursor };
FCITX_CONFIG_ENUM_NAME(SelectPhrase, "游標前", "游標後");

FCITX_CONFIGURATION(ImeFcitxConfig,
    fcitx::Option<std::string> modelPath{this, "ModelPath", "模型路徑", default_config().model_path};
    fcitx::Option<int, fcitx::IntConstrain> contextLength{
        this, "ContextLength", "上下文長度", default_config().context_length, fcitx::IntConstrain(1, 1048576)};
    fcitx::Option<int, fcitx::IntConstrain> threadCount{
        this, "ThreadCount", "執行緒數", default_config().thread_count, fcitx::IntConstrain(1, 1024)};
    fcitx::Option<int, fcitx::IntConstrain> gpuLayers{
        this, "GpuLayers", "顯示卡分層數", default_config().gpu_layers, fcitx::IntConstrain(0, 1024)};
    fcitx::Option<int, fcitx::IntConstrain> idleTimeoutSeconds{this, "IdleTimeoutSeconds", "閒置逾時秒數",
                                                                default_config().idle_timeout_seconds,
                                                                fcitx::IntConstrain(0, 86400)};
    fcitx::Option<BopomofoKeyboardLayout> keyboardLayout{this, "BopomofoKeyboardLayout",
                                                          "注音鍵盤配置",
                                                          BopomofoKeyboardLayout::Standard};
    fcitx::Option<SelectionKeys> selectionKeys{this, "SelectionKeys", "候選選字鍵", SelectionKeys::Digits};
    fcitx::Option<int, fcitx::IntConstrain> selectionKeyCount{this, "SelectionKeysCount", "候選選字鍵數量",
                                                              default_config().selection_key_count,
                                                              fcitx::IntConstrain(4, 9)};
    fcitx::Option<int, fcitx::IntConstrain> candidatePageSize{this, "CandidatePageSize", "候選頁大小",
                                                              default_config().candidate_page_size,
                                                              fcitx::IntConstrain(1, 50)};
    fcitx::Option<CandidateLayout> candidateLayout{this, "CandidateLayout", "候選窗排列",
                                                    CandidateLayout::NotSet};
    fcitx::Option<bool> chooseCandidateUsingSpace{this, "ChooseCandidateUsingSpace",
                                                   "空白鍵選取候選字",
                                                   default_config().space_selects_candidate};
    fcitx::Option<SelectPhrase> selectPhrase{this, "SelectPhrase", "候選字查詢位置", SelectPhrase::BeforeCursor};
    fcitx::Option<bool> moveCursorAfterSelection{this, "MoveCursorAfterSelection", "選字後移動游標",
                                                  default_config().move_cursor_after_selection};
    fcitx::Option<bool> escKeyClearsEntireComposingBuffer{this, "EscKeyClearsEntireComposingBuffer",
                                                           "逸出鍵清除整個組字區",
                                                           default_config().esc_clears_entire_buffer};
    fcitx::Option<bool> capsLockInputsBopomofo{this, "CapsLockInputsBopomofo",
                                               "大寫鎖定時仍輸入注音",
                                               default_config().caps_lock_inputs_bopomofo};);

Config to_shared_config(const ImeFcitxConfig& config);
void apply_shared_config(ImeFcitxConfig& target, const Config& source);

}  // namespace ime::fcitx5
