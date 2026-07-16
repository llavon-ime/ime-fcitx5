#include "ml/safetensors_reader.hpp"
#include "training/sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace imesvc::ml {
namespace {

std::uint64_t little_u64(const std::array<std::uint8_t, 8>& bytes) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(bytes[shift / 8U]) << shift;
    return value;
}

std::uint64_t checked_product(const nlohmann::json& shape, const std::string& name) {
    if (!shape.is_array() || shape.empty()) throw std::runtime_error("invalid safetensors shape: " + name);
    std::uint64_t result = 1;
    for (const auto& dimension : shape) {
        if (!dimension.is_number_unsigned()) throw std::runtime_error("invalid safetensors dimension: " + name);
        const auto value = dimension.get<std::uint64_t>();
        if (value == 0 || result > std::numeric_limits<std::uint64_t>::max() / value)
            throw std::runtime_error("overflowing safetensors shape: " + name);
        result *= value;
    }
    return result;
}

std::vector<std::int64_t> dimensions(const nlohmann::json& shape, const std::string& name) {
    std::vector<std::int64_t> result;
    result.reserve(shape.size());
    for (const auto& dimension : shape) {
        const auto value = dimension.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("safetensors dimension is too large: " + name);
        result.push_back(static_cast<std::int64_t>(value));
    }
    return result;
}

class FileView final {
public:
    FileView(const std::filesystem::path& path, std::uint64_t maximum_size) {
#ifndef _WIN32
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::open(path.c_str(), flags);
        if (descriptor_ < 0) throw std::runtime_error("open safetensors file failed: " + path.string());
        struct stat status {};
        if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 8 ||
            static_cast<std::uint64_t>(status.st_size) > maximum_size ||
            static_cast<std::uint64_t>(status.st_size) > std::numeric_limits<std::size_t>::max()) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("invalid safetensors file size");
        }
        size_ = static_cast<std::size_t>(status.st_size);
        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
        if (mapping_ == MAP_FAILED) {
            ::close(descriptor_);
            descriptor_ = -1;
            mapping_ = nullptr;
            throw std::runtime_error("map safetensors file failed");
        }
#else
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("open safetensors file failed: " + path.string());
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size < 8 || static_cast<std::uint64_t>(size) > maximum_size) throw std::runtime_error("invalid safetensors file size");
        bytes_.resize(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
        if (!input) throw std::runtime_error("read safetensors file failed");
        size_ = bytes_.size();
#endif
    }

    ~FileView() {
#ifndef _WIN32
        if (mapping_ != nullptr) ::munmap(mapping_, size_);
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }

    FileView(const FileView&) = delete;
    FileView& operator=(const FileView&) = delete;

    const std::uint8_t* data() const noexcept {
#ifndef _WIN32
        return static_cast<const std::uint8_t*>(mapping_);
#else
        return bytes_.data();
#endif
    }
    std::size_t size() const noexcept { return size_; }

private:
    std::size_t size_ = 0;
#ifndef _WIN32
    int descriptor_ = -1;
    void* mapping_ = nullptr;
#else
    std::vector<std::uint8_t> bytes_;
#endif
};

}  // namespace

SafetensorsReader::SafetensorsReader(SafetensorsLimits limits) : limits_(limits) {}

std::string SafetensorsReader::sha256_file(const std::filesystem::path& path) {
    return training::sha256_file(path);
}

void SafetensorsReader::load_fixed_llama(const std::filesystem::path& path, const std::string& expected_sha256,
                                          const std::unordered_map<std::string, torch::Tensor>& destinations) const {
    const FileView file(path, limits_.max_file_bytes);
    const auto file_size = file.size();
    const auto actual_sha256 = training::sha256_bytes(file.data(), file.size());
    if (expected_sha256.empty() || actual_sha256 != expected_sha256) {
        throw std::runtime_error("base safetensors SHA256 mismatch: " + actual_sha256);
    }
    std::array<std::uint8_t, 8> length_bytes{};
    std::copy_n(file.data(), length_bytes.size(), length_bytes.data());
    const auto header_size = little_u64(length_bytes);
    if (header_size == 0 || header_size > limits_.max_header_bytes || header_size > file_size - 8U)
        throw std::runtime_error("invalid safetensors header length");
    std::string header(reinterpret_cast<const char*>(file.data() + 8U), static_cast<std::size_t>(header_size));
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto reject_duplicate_keys = [&object_keys](int depth, nlohmann::json::parse_event_t event,
                                                       nlohmann::json& parsed) {
        auto index = static_cast<std::size_t>(std::max(depth, 0));
        if (event == nlohmann::json::parse_event_t::object_start) ++index;
        if (object_keys.size() <= index) object_keys.resize(index + 1U);
        if (event == nlohmann::json::parse_event_t::object_start) object_keys[index].clear();
        if (event == nlohmann::json::parse_event_t::key && !object_keys[index].insert(parsed.get<std::string>()).second) {
            throw std::runtime_error("duplicate safetensors JSON key");
        }
        return true;
    };
    const auto metadata = nlohmann::json::parse(header, reject_duplicate_keys);
    if (!metadata.is_object()) throw std::runtime_error("safetensors header is not an object");
    for (const auto& [name, value] : metadata.items()) {
        (void)value;
        if (name != "__metadata__" && !destinations.contains(name)) throw std::runtime_error("unknown safetensors tensor: " + name);
    }
    const auto data_size = file_size - 8U - header_size;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t parameter_count = 0;
    for (const auto& [name, destination] : destinations) {
        if (!metadata.contains(name)) throw std::runtime_error("missing safetensors tensor: " + name);
        const auto& tensor = metadata.at(name);
        if (!tensor.is_object() || tensor.value("dtype", "") != "F32" || !tensor.contains("shape") || !tensor.contains("data_offsets"))
            throw std::runtime_error("unsupported safetensors tensor: " + name);
        const auto elements = checked_product(tensor.at("shape"), name);
        if (elements > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) throw std::runtime_error("tensor byte overflow: " + name);
        const auto& offsets = tensor.at("data_offsets");
        if (!offsets.is_array() || offsets.size() != 2 || !offsets[0].is_number_unsigned() || !offsets[1].is_number_unsigned())
            throw std::runtime_error("invalid safetensors offsets: " + name);
        const auto begin = offsets[0].get<std::uint64_t>();
        const auto end = offsets[1].get<std::uint64_t>();
        if (begin > end || end > data_size || end - begin != elements * sizeof(float))
            throw std::runtime_error("invalid safetensors byte range: " + name);
        if (destination.scalar_type() != torch::kFloat32 || destination.sizes().vec() != dimensions(tensor.at("shape"), name))
            throw std::runtime_error("safetensors model shape mismatch: " + name);
        ranges.emplace_back(begin, end);
        if (parameter_count > std::numeric_limits<std::uint64_t>::max() - elements) throw std::runtime_error("parameter count overflow");
        parameter_count += elements;
    }
    if (metadata.size() != destinations.size() && metadata.size() != destinations.size() + 1U) throw std::runtime_error("invalid safetensors tensor set");
    if (parameter_count != LlamaModelConfig::kParameterCount) throw std::runtime_error("unexpected base parameter count");
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index - 1].second > ranges[index].first) throw std::runtime_error("overlapping safetensors ranges");
    }
    const auto* data = file.data() + 8U + static_cast<std::size_t>(header_size);
    torch::NoGradGuard guard;
    for (const auto& [name, destination] : destinations) {
        const auto& offsets = metadata.at(name).at("data_offsets");
        const auto begin = offsets[0].get<std::uint64_t>();
        const auto shape = dimensions(metadata.at(name).at("shape"), name);
        auto view = torch::from_blob(const_cast<std::uint8_t*>(data + begin), shape,
                                     torch::TensorOptions().dtype(torch::kFloat32));
        destination.copy_(view);
    }
}

}  // namespace imesvc::ml
