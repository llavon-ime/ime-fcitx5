#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace ime::unix_service {

// Reports the accelerator the pinned trainer release can target. AMD GPUs train
// through ROCm, whose release carries the HIP libraries; everything else uses
// the CPU build, which already contains the Metal backend on Apple Silicon.
// NVIDIA needs a CUDA build that the automatic release does not carry.
inline std::string gpu_vendor() {
#if defined(__APPLE__)
    // The macOS release targets Apple Silicon, whose GPU is driven by Metal.
    return "apple";
#else
    std::error_code error;
    bool amd = false;
    bool nvidia = false;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("card") || name.find('-') != std::string::npos) continue;
        std::ifstream vendor(entry.path() / "device" / "vendor");
        std::string value;
        if (!(vendor >> value)) continue;
        if (value == "0x1002") amd = true;
        else if (value == "0x10de") nvidia = true;
    }
    if (amd) return "amd";
    if (nvidia) return "nvidia";
    return "none";
#endif
}

// The backend `install-trainer --backend auto` selects.
inline std::string recommended_backend() {
    const auto vendor = gpu_vendor();
    if (vendor == "amd") return "rocm";
    if (vendor == "nvidia") return "cuda";
    return "cpu";
}

}  // namespace ime::unix_service
