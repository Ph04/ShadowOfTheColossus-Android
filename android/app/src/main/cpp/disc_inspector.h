#pragma once

#include "iso_source.h"

#include <string>

struct DiscInspection {
    bool supported = false;
    std::string serial;
    std::string version;
    std::string bootExecutable;
    std::string summary;
};

bool inspectShadowOfTheColossusDisc(const IsoSource& source,
                                    DiscInspection& inspection,
                                    std::string& error);
