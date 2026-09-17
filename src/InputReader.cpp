#include <iostream>
#include <string>
#include <fstream>
#include <stdexcept>

#include "../include/InputReader.h"
#include "../MyCppLibs/sassert.h"

std::vector<int> ReadDataAndCapacities(std::vector<SCacheLevelConfig>& LevelConfigs, std::istream& InputStream) {
    for (std::size_t Index = 0; Index < LevelConfigs.size(); ++Index) {
        int Capacity;
        CHECK(InputStream >> Capacity,
              "В конфиге заявлено " + std::to_string(LevelConfigs.size()) +
              " уровней кеша, но не хватает емкости для уровня " +
              std::to_string(Index + 1) + ".\n");
        
        LevelConfigs[Index].Capacity = Capacity;
    }
        
    std::size_t DataCount = 0;
    CHECK(InputStream >> DataCount,
          "В входном потоке нету данных после емкостей для кешей.\n");

    std::vector<int> DataStream;
    DataStream.reserve(DataCount);

    for (std::size_t DataIndex = 0; DataIndex < DataCount; ++DataIndex) {
        int Value;
        CHECK(InputStream >> Value,
              "Во входных данных заявлено " + std::to_string(DataCount) +
              " данных, но их не хватает: остановился на номере " +
              std::to_string(DataIndex + 1) + ".\n");

        DataStream.push_back(Value);
    }

    return DataStream;
}

std::vector<SCacheLevelConfig> ReadCacheConfig(const std::string& ConfigFilePath) {
    std::ifstream ConfigFile(ConfigFilePath);
    CHECK(ConfigFile.is_open(), "Не удалось открыть конфиг-файл: '" + ConfigFilePath + "'.");

    std::size_t LevelCount = 0;
    CHECK(ConfigFile >> LevelCount && LevelCount != 0, "Конфиг-файл '" + ConfigFilePath +
                                  "': ожидалось ненулевое число уровней кеша первой строкой.");

    std::vector<SCacheLevelConfig> LevelConfigs;
    LevelConfigs.reserve(LevelCount);

    for (std::size_t LevelIndex = 0; LevelIndex < LevelCount; ++LevelIndex) {
        std::string AlgorithmName;

        CHECK(ConfigFile >> AlgorithmName,
              "Конфиг-файл '" + ConfigFilePath + "': некорректные данные для уровня " +
              std::to_string(LevelIndex + 1) + " ожидалось корректное название алгоритма.");

        const ECacheAlgorithm Algorithm = ParseCacheAlgorithm(AlgorithmName);
        CHECK_EX(Algorithm != ECacheAlgorithm::Unknown, std::invalid_argument,
                 "Конфиг-файл '" + ConfigFilePath + "', уровень " +
                 std::to_string(LevelIndex + 1) + ": неизвестный алгоритм '" + AlgorithmName +
                 "'. Поддерживаются: LRU, LFU, ARC, 2Q, LIRS.");

        LevelConfigs.push_back({0, Algorithm});
    }

    return LevelConfigs;
}