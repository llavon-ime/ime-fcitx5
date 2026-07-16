#pragma once

#include "training/training_orchestrator.hpp"

#include <filesystem>
#include <memory>

namespace imesvc::training {

// Collects only local, non-content system state needed for fail-closed training admission.
std::shared_ptr<TrainingEnvironmentProvider> make_system_training_environment_provider(
    std::filesystem::path disk_path);

}  // namespace imesvc::training
