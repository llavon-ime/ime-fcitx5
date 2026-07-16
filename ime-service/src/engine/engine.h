#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tokenizer.hpp"

namespace imesvc {

struct PredictResult {
    std::vector<std::pair<char32_t, float>> candidates;
};

class IEngine {
public:
    virtual ~IEngine() = default;
    virtual void ready() {}
    virtual std::vector<PredictResult> predict(const std::u16string& context,
                                               const std::vector<PaddingEntry>& padding) = 0;
};

}  // namespace imesvc
