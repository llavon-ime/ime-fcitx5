#pragma once

#include <iostream>

#include "server_strategy.hpp"

namespace imesvc {

class UnsupportedServer final : public ServerStrategy {
public:
    const char* name() const override {
        return "unsupported";
    }

    int run() override {
        std::cerr << "[ERR] llavon-ime service is not supported on this platform yet." << std::endl;
        return 1;
    }
};

}  // namespace imesvc
