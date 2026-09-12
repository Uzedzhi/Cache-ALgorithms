#include <climits>
#include <iostream>
#include <vector>
#include <exception>
#include <array>

#include "../include/CacheTypes.h"
#include "../include/ConfigReader.h"
#include "../include/InputReader.h"
#include "../include/IdealCache.h"
#include "../include/MultiLevelCache.h"

std::size_t SumOfCapacities(const std::vector<SCacheLevelConfig>& LevelConfigs) {
    std::size_t sum = 0;
    for (const SCacheLevelConfig &Config : LevelConfigs) {
        sum += Config.Capacity;
    }

    return sum;
}

TCacheHierarchy<int> BuildHierarchy(const std::vector<SCacheLevelConfig>& LevelConfigs, bool IsIdeal, const std::vector<int> &DataStream) {
    if (IsIdeal) {
        SCacheLevelConfig arr[1] = {SumOfCapacities(LevelConfigs), ECacheAlgorithm::Ideal};
        return TCacheHierarchy<int>(std::vector<SCacheLevelConfig>(std::begin(arr), std::end(arr)), DataStream);
    }

    return TCacheHierarchy<int>(LevelConfigs, DataStream);
}

std::size_t SumCapacities(const std::vector<SCacheLevelConfig>& LevelConfigs) {
    std::size_t Total = 0;
    for (const SCacheLevelConfig& Config : LevelConfigs) {
        Total += Config.Capacity;
    }
    return Total;
}

void RunStream(TCacheHierarchy<int>& Hierarchy, const std::vector<int>& DataStream) {
    for (std::size_t Position = 0; Position < DataStream.size(); ++Position) {
        Hierarchy.CacheFind(DataStream[Position], Position);
    }
}

void PrintResults(const TCacheHierarchy<int>& RealHierarchy,
                  const TCacheHierarchy<int>& IdealHierarchy) {
    const std::vector<std::size_t>& RealHits = RealHierarchy.GetHitCountsPerLevel();
    for (std::size_t Index = 0; Index < RealHierarchy.GetLevelCount(); ++Index) {
        std::cout << "Level " << (Index + 1) << " ("
                    << CacheAlgorithmToString(RealHierarchy.GetLevelAlgorithm(Index))
                    << ", capacity=" << RealHierarchy.GetLevelCapacity(Index) << "): "
                    << RealHits[Index] << " hits\n";
    }

    const std::vector<std::size_t>& IdealHits = IdealHierarchy.GetHitCountsPerLevel();
        for (std::size_t Index = 0; Index < IdealHierarchy.GetLevelCount(); ++Index) {
        std::cout << "Level " << (Index + 1) << " ("
                    << "Ideal"
                    << ", capacity=" << IdealHierarchy.GetLevelCapacity(Index) << "): "
                    << IdealHits[Index] << " hits\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cout << "usage: " << argv[0] << " <config_file_name>\n";
    }
    const std::string ConfigFilePath = (argc == 2) ? argv[1] : "config.txt";

    try {
        const std::vector<SCacheLevelConfig> LevelConfigs = ReadCacheConfig(ConfigFilePath);
        const std::vector<int>               DataStream   = ReadDataStream();

        TCacheHierarchy<int> IdealHierarchy = BuildHierarchy(LevelConfigs, true, DataStream);
        RunStream(IdealHierarchy, DataStream);

        TCacheHierarchy<int> RealHierarchy = BuildHierarchy(LevelConfigs, false, DataStream);
        RunStream(RealHierarchy, DataStream);

        PrintResults(RealHierarchy, IdealHierarchy);
    }
    catch (const std::exception& Error) {
        std::cerr << "Ошибка: " << Error.what() << '\n';
        return 1;
    }

    return 0;
}
