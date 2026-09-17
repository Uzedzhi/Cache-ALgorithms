#pragma once

#include <vector>
#include <iostream>

#include "CacheTypes.h"

std::vector<SCacheLevelConfig> ReadCacheConfig(const std::string& ConfigFilePath);
std::vector<int> ReadDataAndCapacities(std::vector<SCacheLevelConfig> &LevelConfigs, std::istream& InputStream = std::cin);