#include "../include/CacheTypes.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace CacheTypesHash {
    constexpr std::uint32_t FNV1AOffsetBasis = 2166136261u;
    constexpr std::uint32_t FNV1APrime       = 16777619u;
    constexpr void FNV1A_Hash_step(std::uint32_t &hash, std::uint8_t ch) {
        hash ^= ch;
        hash *= FNV1APrime;
    }

    constexpr std::uint32_t FNV1A_Hash(const char* Text) {
        std::uint32_t Hash = FNV1AOffsetBasis;

        for (; *Text != '\0'; ++Text) {
            std::uint8_t uch8 = static_cast<std::uint8_t>(*Text);
            FNV1A_Hash_step(Hash, uch8);
        }

        return Hash;
    }

    std::uint32_t FNV1A_HashUpperCase(std::string_view Text) {
        std::uint32_t Hash = FNV1AOffsetBasis;

        for (char Character : Text) {
            std::uint8_t uch8 = static_cast<std::uint8_t>(
                std::toupper(static_cast<unsigned char>(Character)));
            FNV1A_Hash_step(Hash, uch8);
        }

        return Hash;
    }
}

ECacheAlgorithm ParseCacheAlgorithm(std::string_view AlgorithmName) {
    switch (CacheTypesHash::FNV1A_HashUpperCase(AlgorithmName)) {
        case CacheTypesHash::FNV1A_Hash("LRU"):     return ECacheAlgorithm::Lru;
        case CacheTypesHash::FNV1A_Hash("LFU"):     return ECacheAlgorithm::Lfu;
        case CacheTypesHash::FNV1A_Hash("LIRS"):    return ECacheAlgorithm::Lirs;

        case CacheTypesHash::FNV1A_Hash("2Q"):
        case CacheTypesHash::FNV1A_Hash("TWOQ"):
        case CacheTypesHash::FNV1A_Hash("TWO-Q"):   return ECacheAlgorithm::TwoQ;
        default:                    return ECacheAlgorithm::Unknown;
    }
}

std::string_view CacheAlgorithmToString(ECacheAlgorithm Algorithm) {
    static constexpr std::array<
        std::string_view,
        static_cast<std::size_t>(ECacheAlgorithm::Unknown)
    > CacheAlgorithmNames = {"LRU", "LFU", "2Q", "LIRS", "IDEAL"};

    return CacheAlgorithmNames[static_cast<std::size_t>(Algorithm)];
}
