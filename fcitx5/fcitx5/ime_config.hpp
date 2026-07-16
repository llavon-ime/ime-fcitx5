#pragma once

#include "config/config.hpp"

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>

#ifndef IME_FCITX5_DISPLAY_VERSION
#define IME_FCITX5_DISPLAY_VERSION "unknown"
#endif

namespace ime::fcitx5 {

inline constexpr const char* kFcitxConfigFile = "conf/llavon-ime.conf";

enum class DisplayVersion { Current };
FCITX_CONFIG_ENUM_NAME(DisplayVersion, IME_FCITX5_DISPLAY_VERSION);

enum class BopomofoKeyboardLayout { Standard };
FCITX_CONFIG_ENUM_NAME(BopomofoKeyboardLayout, "標準");

enum class SelectionKeys { Digits, HomeRow, LeftHand };
FCITX_CONFIG_ENUM_NAME(SelectionKeys, "數字鍵", "本位列", "左手鍵");

enum class CandidateLayout { NotSet, Vertical, Horizontal };
FCITX_CONFIG_ENUM_NAME(CandidateLayout, "系統預設", "垂直", "水平");

enum class SelectPhrase { BeforeCursor, AfterCursor };
FCITX_CONFIG_ENUM_NAME(SelectPhrase, "游標前", "游標後");

FCITX_CONFIGURATION(ImeFcitxConfig,
    fcitx::Option<DisplayVersion> version{this, "Version", "版本", DisplayVersion::Current};
    fcitx::Option<std::string> modelPath{this, "ModelPath", "模型路徑", default_config().model_path};
    fcitx::Option<std::string> baseModelSha256{this, "BaseModelSha256", "基礎模型 SHA256",
                                               default_config().base_model_sha256};
    fcitx::Option<int, fcitx::IntConstrain> contextLength{
        this, "ContextLength", "上下文長度", default_config().context_length,
        fcitx::IntConstrain(1, kNativeContextLength)};
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
                                                default_config().caps_lock_inputs_bopomofo};
    fcitx::Option<bool> personalLearningEnabled{this, "PersonalLearningEnabled", "收集本機個人化回饋",
                                                   default_config().personal_learning_enabled};
    fcitx::Option<bool> loraTrainingEnabled{this, "LoraTrainingEnabled", "啟用背景 LoRA 個人化微調",
                                             default_config().lora_training_enabled};
    fcitx::Option<std::string> trainingBaseSafetensorsPath{this, "TrainingBaseSafetensorsPath",
                                                            "LoRA 訓練基礎權重路徑（F32 Safetensors）",
                                                            default_config().training_base_safetensors_path};
    fcitx::Option<bool> deletePersonalData{this, "DeletePersonalData", "刪除所有本機個人化資料（套用後執行）", false};);

Config to_shared_config(const ImeFcitxConfig& config);
void apply_shared_config(ImeFcitxConfig& target, const Config& source);

}  // namespace ime::fcitx5
