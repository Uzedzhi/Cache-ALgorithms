#pragma once

#include <string>
#include <vector>

#include "CacheTypes.h"

std::vector<SCacheLevelConfig> ReadCacheConfig(const std::string& ConfigFilePath);