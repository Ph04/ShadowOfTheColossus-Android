#pragma once

#include "iso_source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct SotcPipelineProgress {
    int percent = 0;
    int step = 0;
    int totalSteps = 7;
    int64_t etaSeconds = -1;
    bool running = false;
    bool completed = false;
    bool failed = false;
    std::string stage;
    std::string detail;
    std::string error;
    std::string outputDirectory;
};

using SotcProgressCallback = std::function<void(const SotcPipelineProgress&)>;

bool runSotcOnDevicePipeline(const IsoSource& iso,
                             const std::string& appFilesDirectory,
                             std::atomic_bool& cancelRequested,
                             const SotcProgressCallback& onProgress,
                             std::string& error);
