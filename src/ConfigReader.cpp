#include "../include/ConfigReader.h"

#include <fstream>
#include <stdexcept>

#include "../MyCppLibs/sassert.h"

std::vector<SCacheLevelConfig> ReadCacheConfig(const std::string& ConfigFilePath) {
    std::ifstream ConfigFile(ConfigFilePath);
    CHECK(ConfigFile.is_open(), "Не удалось открыть конфиг-файл: '" + ConfigFilePath + "'.");

    std::size_t LevelCount = 0;
    CHECK(ConfigFile >> LevelCount && LevelCount != 0, "Конфиг-файл '" + ConfigFilePath +
                                  "': ожидалось ненулевое число уровней кеша первой строкой.");

    std::vector<SCacheLevelConfig> LevelConfigs;
    LevelConfigs.reserve(LevelCount);

    for (std::size_t LevelIndex = 0; LevelIndex < LevelCount; ++LevelIndex) {
        std::size_t Capacity = 0;
        std::string AlgorithmName;

        CHECK(ConfigFile >> Capacity >> AlgorithmName && Capacity != 0,
              "Конфиг-файл '" + ConfigFilePath + "': некорректные данные для уровня " +
              std::to_string(LevelIndex + 1) + " (ожидались ненулевой размер и алгоритм).");

        const ECacheAlgorithm Algorithm = ParseCacheAlgorithm(AlgorithmName);
        CHECK_EX(Algorithm != ECacheAlgorithm::Unknown, std::invalid_argument,
                 "Конфиг-файл '" + ConfigFilePath + "', уровень " +
                 std::to_string(LevelIndex + 1) + ": неизвестный алгоритм '" + AlgorithmName +
                 "'. Поддерживаются: LRU, LFU, ARC, 2Q, LIRS.");

        LevelConfigs.push_back({Capacity, Algorithm});
    }

    return LevelConfigs;
}
