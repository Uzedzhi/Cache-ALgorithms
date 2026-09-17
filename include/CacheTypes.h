#pragma once

#include <cstddef>
#include <string_view>

enum ECacheAlgorithm {
    Lru,
    Lfu,
    TwoQ,
    Lirs,
    Ideal,
    Unknown
};

struct SCacheLevelConfig {
    std::size_t Capacity = 0;
    ECacheAlgorithm Algorithm = ECacheAlgorithm::Lru;
};

ECacheAlgorithm ParseCacheAlgorithm(std::string_view AlgorithmName);
std::string_view CacheAlgorithmToString(ECacheAlgorithm Algorithm);