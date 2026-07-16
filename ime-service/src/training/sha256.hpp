#pragma once

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace imesvc::training {

// Streams a regular file so large base checkpoints do not need to fit in RAM.
std::string sha256_file(const std::filesystem::path& path);
std::string sha256_bytes(const void* data, std::size_t size);

// Hashes a labeled set of files relative to root with deterministic framing.
std::string sha256_file_set(const std::filesystem::path& root,
                            const std::vector<std::filesystem::path>& relative_paths);

}  // namespace imesvc::training
