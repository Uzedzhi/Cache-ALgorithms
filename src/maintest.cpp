#include <cstddef>
#include <iostream>
#include <vector>
#include <exception>

#include "../include/CacheTypes.h"
#include "../include/InputReader.h"
#include "../include/MultiLevelCache.h"

using KeyType = int;
void PrintResults(const TCacheRealHierarchy<KeyType>&  RealCacheHierarchy,
                  const TCacheIdealHierarchy<KeyType>& IdealCacheHierarchy,
                  const std::vector<SCacheLevelConfig> LevelConfigs) {
    const std::vector<std::size_t>& RealHits = RealCacheHierarchy.GetHitCounts();
    for (std::size_t Index = 0; Index < LevelConfigs.size(); ++Index) {
        std::cout << "Level " << (Index + 1) << " ("
                    << CacheAlgorithmToString(LevelConfigs[Index].Algorithm)
                    << ", capacity=" << LevelConfigs[Index].Capacity << "): "
                    << RealHits[Index] << " hits\n";
    }

    std::cout << "Level 1 (Ideal"
              << ", capacity=" << RealCacheHierarchy.GetSumCapacities() << "): "
              << IdealCacheHierarchy.HitCount_ << " hits\n";
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cout << "usage: " << argv[0] << " <config_file_name>\n";
        return 1;
    }
    const std::string ConfigFilePath = (argc == 2) ? argv[1] : "config.txt";

    try {
        std::vector<SCacheLevelConfig> LevelConfigs = ReadCacheConfig(ConfigFilePath);
        const std::vector<KeyType>     DataStream   = ReadDataAndCapacities(LevelConfigs);

        TCacheRealHierarchy<KeyType>  RealCacheHierarchy(LevelConfigs);
        TCacheIdealHierarchy<KeyType> IdealCacheHierarchy(RealCacheHierarchy.GetSumCapacities(), DataStream);

        for (std::size_t i = 0; i < DataStream.size(); ++i) {
            int Key = DataStream[i];

            RealCacheHierarchy.FindInsert(Key);
            IdealCacheHierarchy.FindInsert(Key, i);
        }

        PrintResults(RealCacheHierarchy, IdealCacheHierarchy, LevelConfigs);
    }
    catch (const std::exception& Error) {
        std::cerr << "Ошибка: " << Error.what() << '\n';
        return 1;
    }

    return 0;
}
