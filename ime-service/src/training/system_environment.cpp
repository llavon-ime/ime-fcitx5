#include "training/system_environment.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace imesvc::training {
namespace {

#ifndef _WIN32
std::optional<double> normalized_cpu_load() {
    double load = 0.0;
    const auto processors = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (processors <= 0 || ::getloadavg(&load, 1) != 1 || load < 0.0) return std::nullopt;
    return load / static_cast<double>(processors);
}
#endif

#ifdef __linux__
std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!input || !std::getline(input, value)) return std::nullopt;
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::optional<std::uint64_t> parse_u64(const std::string& value) {
    try {
        std::size_t parsed = 0;
        const auto result = std::stoull(value, &parsed, 10);
        return parsed == value.size() ? std::optional<std::uint64_t>(result) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}
#endif

#ifdef __linux__
std::optional<TrainingEnvironment> linux_environment(const std::filesystem::path& disk_path) {
    TrainingEnvironment result;
    bool saw_battery = false;
    bool saw_ac = false;
    int minimum_battery = 100;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply")) {
        const auto type = read_text_file(entry.path() / "type");
        if (!type) continue;
        if (*type == "Mains" || *type == "USB") {
            saw_ac = true;
            result.on_ac_power = result.on_ac_power || read_text_file(entry.path() / "online").value_or("0") == "1";
        } else if (*type == "Battery") {
            saw_battery = true;
            if (const auto capacity = read_text_file(entry.path() / "capacity"); capacity) {
                const auto parsed = parse_u64(*capacity);
                if (!parsed || *parsed > 100) return std::nullopt;
                minimum_battery = std::min(minimum_battery, static_cast<int>(*parsed));
            }
        }
    }
    if (!saw_ac || !saw_battery || minimum_battery < 0) return std::nullopt;
    result.battery_percentage = minimum_battery;

    std::ifstream memory("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (memory >> key >> value >> unit) {
        if (key == "MemAvailable:") {
            result.free_memory_bytes = value * 1024U;
            break;
        }
    }
    if (result.free_memory_bytes == 0) return std::nullopt;

    result.thermal_acceptable = true;
    bool saw_thermal_zone = false;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/thermal")) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
        saw_thermal_zone = true;
        const auto temperature = read_text_file(entry.path() / "temp");
        if (!temperature) continue;
        const auto parsed = parse_u64(*temperature);
        if (!parsed) return std::nullopt;
        result.thermal_acceptable = result.thermal_acceptable && *parsed < 80000U;
    }
    if (!saw_thermal_zone) return std::nullopt;

    struct statvfs disk {};
    if (::statvfs(disk_path.c_str(), &disk) != 0) return std::nullopt;
    result.free_disk_bytes = static_cast<std::uint64_t>(disk.f_bavail) * static_cast<std::uint64_t>(disk.f_frsize);
    const auto cpu_load = normalized_cpu_load();
    if (!cpu_load) return std::nullopt;
    result.cpu_load_fraction = *cpu_load;
    return result;
}
#endif

#ifdef __APPLE__
std::optional<TrainingEnvironment> macos_environment(const std::filesystem::path& disk_path) {
    TrainingEnvironment result;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr) return std::nullopt;
    const auto providing = IOPSGetProvidingPowerSourceType(info);
    result.on_ac_power = providing != nullptr && CFEqual(providing, kIOPSACPowerValue);
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    bool saw_battery = false;
    int minimum_battery = 100;
    if (sources != nullptr) {
        for (CFIndex index = 0; index < CFArrayGetCount(sources); ++index) {
            const auto source = CFArrayGetValueAtIndex(sources, index);
            const auto description = IOPSGetPowerSourceDescription(info, source);
            if (description == nullptr) continue;
            const auto state = static_cast<CFStringRef>(CFDictionaryGetValue(description, kIOPSPowerSourceStateKey));
            result.on_ac_power = result.on_ac_power || (state != nullptr && CFEqual(state, kIOPSACPowerValue));
            const auto capacity = static_cast<CFNumberRef>(CFDictionaryGetValue(description, kIOPSCurrentCapacityKey));
            int percentage = -1;
            if (capacity == nullptr || !CFNumberGetValue(capacity, kCFNumberIntType, &percentage) || percentage < 0 || percentage > 100) {
                CFRelease(sources);
                CFRelease(info);
                return std::nullopt;
            }
            saw_battery = true;
            minimum_battery = std::min(minimum_battery, percentage);
        }
        CFRelease(sources);
    }
    CFRelease(info);
    if (!saw_battery && !result.on_ac_power) return std::nullopt;
    result.battery_percentage = saw_battery ? minimum_battery : 100;

    vm_size_t page_size = 0;
    const auto host = mach_host_self();
    if (host_page_size(host, &page_size) != KERN_SUCCESS) return std::nullopt;
    vm_statistics64_data_t memory{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    result.free_memory_bytes = static_cast<std::uint64_t>(memory.free_count + memory.inactive_count + memory.speculative_count) * page_size;

    const auto process_info_class = reinterpret_cast<void*>(objc_getClass("NSProcessInfo"));
    if (process_info_class == nullptr) return std::nullopt;
    const auto send_object = reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend);
    const auto process_info = send_object(process_info_class, sel_registerName("processInfo"));
    if (process_info == nullptr) return std::nullopt;
    const auto send_integer = reinterpret_cast<long (*)(void*, SEL)>(objc_msgSend);
    const auto thermal_state = send_integer(process_info, sel_registerName("thermalState"));
    // NSProcessInfoThermalStateNominal/Fair are suitable; Serious/Critical block training.
    result.thermal_acceptable = thermal_state == 0 || thermal_state == 1;

    struct statvfs disk {};
    if (::statvfs(disk_path.c_str(), &disk) != 0) return std::nullopt;
    result.free_disk_bytes = static_cast<std::uint64_t>(disk.f_bavail) * static_cast<std::uint64_t>(disk.f_frsize);
    const auto cpu_load = normalized_cpu_load();
    if (!cpu_load) return std::nullopt;
    result.cpu_load_fraction = *cpu_load;
    return result;
}
#endif

class SystemTrainingEnvironmentProvider final : public TrainingEnvironmentProvider {
public:
    explicit SystemTrainingEnvironmentProvider(std::filesystem::path disk_path) : disk_path_(std::move(disk_path)) {}

    std::optional<TrainingEnvironment> current_environment() override {
        try {
#ifdef __linux__
            return linux_environment(disk_path_);
#elif defined(__APPLE__)
            return macos_environment(disk_path_);
#else
            return std::nullopt;
#endif
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    std::filesystem::path disk_path_;
};

}  // namespace

std::shared_ptr<TrainingEnvironmentProvider> make_system_training_environment_provider(std::filesystem::path disk_path) {
    if (disk_path.empty()) disk_path = ".";
    return std::make_shared<SystemTrainingEnvironmentProvider>(std::move(disk_path));
}

}  // namespace imesvc::training
