#pragma once

#include "ml/model_config.hpp"

#include <torch/torch.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace imesvc::ml {

struct SafetensorsLimits {
    std::size_t max_file_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t max_header_bytes = 16ULL * 1024ULL * 1024ULL;
};

class SafetensorsReader final {
public:
    explicit SafetensorsReader(SafetensorsLimits limits = {});

    static std::string sha256_file(const std::filesystem::path& path);
    void load_fixed_llama(const std::filesystem::path& path, const std::string& expected_sha256,
                          const std::unordered_map<std::string, torch::Tensor>& destinations) const;

private:
    SafetensorsLimits limits_;
};

}  // namespace imesvc::ml
