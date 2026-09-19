// maintest.cpp — Google Test для cache_sim.
//
// Сборка и запуск:
//   cmake -S . -B build && cmake --build build
//   build/maintest                     (или ctest --test-dir build)
//   build/maintest --gtest_filter='Lru.*'

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "CacheTypes.h"
#include "InputReader.h"
#include "MultiLevelCache.h"

// ===================== хелперы =====================

// "Медленная" загрузка страницы: страницей служит сам ключ.
int SlowGetPage(const int& Key) {
    return Key;
}

// Прогоняет поток через кеш и считает попадания: Contains до Insert.
template <typename CacheType>
std::size_t CountHits(CacheType& Cache, const std::vector<int>& DataStream) {
    std::size_t Hits = 0;
    for (int Key : DataStream) {
        if (Cache.Contains(Key)) {
            ++Hits;
        }
        Cache.Insert(Key, SlowGetPage);
    }
    return Hits;
}

// Сколько ключей из диапазона [0, KeyRange) сейчас лежит в кеше.
template <typename CacheType>
std::size_t CountResident(const CacheType& Cache, int KeyRange) {
    std::size_t Count = 0;
    for (int Key = 0; Key < KeyRange; ++Key) {
        if (Cache.Contains(Key)) {
            ++Count;
        }
    }
    return Count;
}

std::vector<int> MakeRandomStream(std::size_t Length, int KeyRange, unsigned Seed) {
    std::mt19937 Generator(Seed);
    std::uniform_int_distribution<int> Distribution(0, KeyRange - 1);

    std::vector<int> DataStream(Length);
    for (int& Key : DataStream) {
        Key = Distribution(Generator);
    }
    return DataStream;
}

std::size_t CountIdealHits(std::size_t Capacity, const std::vector<int>& DataStream) {
    TCacheIdealHierarchy<int, int> Ideal(Capacity, DataStream);
    for (std::size_t Position = 0; Position < DataStream.size(); ++Position) {
        Ideal.FindInsert(DataStream[Position], Position, SlowGetPage);
    }
    return Ideal.HitCount_;
}

template <typename CacheType>
void CheckBasicContract(std::size_t Capacity, ECacheAlgorithm Algorithm) {
    SCOPED_TRACE("capacity = " + std::to_string(Capacity) + "cache = " + std::string(CacheAlgorithmToString(Algorithm)));

    CacheType Cache(Capacity);

    Cache.Insert(1, SlowGetPage);
    EXPECT_TRUE(Cache.Contains(1)) << "только что вставленный ключ должен быть в кеше";

    const SCacheEviction<int> Repeated = Cache.Insert(1, SlowGetPage);
    EXPECT_TRUE(Cache.Contains(1));
    EXPECT_FALSE(Repeated.WasEvicted) << "повторное обращение не должно ничего вытеснять";

    const int KeyRange = 50;
    for (int Key : MakeRandomStream(2000, KeyRange, 42)) {
        const SCacheEviction<int> Result = Cache.Insert(Key, SlowGetPage);
        ASSERT_LE(CountResident(Cache, KeyRange), Capacity) << "в кеше больше ключей, чем ёмкость";
        if (Result.WasEvicted && Result.EvictedKey != Key) {
            ASSERT_FALSE(Cache.Contains(Result.EvictedKey)) << "вытесненный ключ остался в кеше";
        }
    }
}

// Пишет Text во временный файл и возвращает путь к нему.
std::string WriteTempConfig(const std::string& Text) {
    const std::filesystem::path FilePath =
        std::filesystem::temp_directory_path() / "cache_sim_maintest_config.txt";
    std::ofstream File(FilePath);
    File << Text;
    return FilePath.string();
}

// ===================== конструкторы =====================

TEST(Constructor, TwoQRejectsTooSmallCapacity) {
    EXPECT_THROW((TTwoQCache<int, int>(0)), std::invalid_argument);
    EXPECT_THROW((TTwoQCache<int, int>(1)), std::invalid_argument);
    EXPECT_NO_THROW((TTwoQCache<int, int>(2)));
}

TEST(Constructor, LirsRejectsTooSmallCapacity) {
    EXPECT_THROW((TLirsCache<int, int>(0)), std::invalid_argument);
    EXPECT_THROW((TLirsCache<int, int>(1)), std::invalid_argument);
    EXPECT_NO_THROW((TLirsCache<int, int>(2)));
}

TEST(Constructor, HierarchyRejectsEmptyConfig) {
    EXPECT_THROW((TCacheRealHierarchy<int, int>({})), std::invalid_argument);
}

TEST(Constructor, HierarchyRejectsNonRealAlgorithms) {
    EXPECT_THROW((TCacheRealHierarchy<int, int>({{4, ECacheAlgorithm::Ideal}})), std::runtime_error);
    EXPECT_THROW((TCacheRealHierarchy<int, int>({{4, ECacheAlgorithm::Unknown}})), std::runtime_error);
}

TEST(Constructor, HierarchyBuildsAllAlgorithms) {
    EXPECT_NO_THROW((TCacheRealHierarchy<int, int>({{4, ECacheAlgorithm::Lru},
                                                    {4, ECacheAlgorithm::Lfu},
                                                    {4, ECacheAlgorithm::TwoQ},
                                                    {4, ECacheAlgorithm::Lirs}})));
}

// ===================== LRU =====================

TEST(Lru, BasicContract) {
    CheckBasicContract<TLruCache<int, int>>(2,  ECacheAlgorithm::Lru);
    CheckBasicContract<TLruCache<int, int>>(10, ECacheAlgorithm::Lru);
}

TEST(Lru, EvictsLeastRecentlyUsed) {
    TLruCache<int, int> Cache(2);
    Cache.Insert(1, SlowGetPage);
    Cache.Insert(2, SlowGetPage);
    Cache.Insert(1, SlowGetPage);

    const SCacheEviction<int> Result = Cache.Insert(3, SlowGetPage);
    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 2);
    EXPECT_TRUE(Cache.Contains(1));
    EXPECT_FALSE(Cache.Contains(2));
    EXPECT_TRUE(Cache.Contains(3));
}

TEST(Lru, HitCountOnKnownStream) {
    TLruCache<int, int> Cache(2);
    EXPECT_EQ(CountHits(Cache, {1, 2, 1, 3, 1, 2, 1}), 3u);
}

// ===================== LFU =====================

TEST(Lfu, BasicContract) {
    CheckBasicContract<TLfuCache<int, int>>(2 , ECacheAlgorithm::Lfu);
    CheckBasicContract<TLfuCache<int, int>>(10, ECacheAlgorithm::Lfu);
}

TEST(Lfu, EvictsLeastFrequentlyUsed) {
    TLfuCache<int, int> Cache(2);
    Cache.Insert(1, SlowGetPage);
    Cache.Insert(1, SlowGetPage);                      // частота 1 -> 2
    Cache.Insert(2, SlowGetPage);                      // частота 1

    const SCacheEviction<int> Result = Cache.Insert(3, SlowGetPage);
    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 2);
    EXPECT_TRUE(Cache.Contains(1));
}

TEST(Lfu, EqualFrequencyEvictsOldest) {
    TLfuCache<int, int> Cache(2);
    Cache.Insert(1, SlowGetPage);
    Cache.Insert(2, SlowGetPage);

    const SCacheEviction<int> Result = Cache.Insert(3, SlowGetPage);
    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 1);
}

TEST(Lfu, HitCountOnKnownStream) {
    TLfuCache<int, int> Cache(2);
    EXPECT_EQ(CountHits(Cache, {1, 3, 2, 5, 4, 3}), 0u);
}

// ===================== 2Q =====================

TEST(TwoQ, BasicContract) {
    CheckBasicContract<TTwoQCache<int, int>>(2 , ECacheAlgorithm::TwoQ);
    CheckBasicContract<TTwoQCache<int, int>>(10, ECacheAlgorithm::TwoQ);
}

TEST(TwoQ, KeyFromA1outIsNotResidentButPromotedOnRepeat) {
    TTwoQCache<int, int> Cache(8);                          // A1in = 2, A1out = 4, Am = 2
    Cache.Insert(1, SlowGetPage);
    Cache.Insert(2, SlowGetPage);

    const SCacheEviction<int> Result = Cache.Insert(3, SlowGetPage);   // 1 уходит из A1in в A1out
    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 1);
    EXPECT_FALSE(Cache.Contains(1)) << "A1out хранит только историю, не данные";

    Cache.Insert(1, SlowGetPage);                      // повтор из A1out -> Am
    EXPECT_TRUE(Cache.Contains(1));
}

TEST(TwoQ, RepeatedKeyInA1inIsHit) {
    TTwoQCache<int, int> Cache(8);
    EXPECT_EQ(CountHits(Cache, {1, 1, 1}), 2u);
}

// ===================== LIRS =====================

TEST(Lirs, BasicContract) {
    CheckBasicContract<TLirsCache<int, int>>(2 , ECacheAlgorithm::Lirs);
    CheckBasicContract<TLirsCache<int, int>>(10, ECacheAlgorithm::Lirs);
}

TEST(Lirs, HitCountOnKnownStream) {
    TLirsCache<int, int> Cache(2);                          // 1 LIR-кадр + 1 HIR-кадр
    EXPECT_EQ(CountHits(Cache, {1, 2, 1, 2}), 2u);
}

// ===================== идеальный кеш =====================

TEST(Ideal, HitCountOnKnownStreams) {
    EXPECT_EQ(CountIdealHits(2, {1, 3, 2, 5, 4, 3}), 1u);
    EXPECT_EQ(CountIdealHits(2, {1, 2, 1, 3, 2, 1}), 3u);
    EXPECT_EQ(CountIdealHits(3, {1, 2, 3, 1, 2, 3}), 3u);
}

TEST(Ideal, DoesNotInsertKeyThatNeverRepeats) {
    const std::vector<int> DataStream = {1, 2, 1};
    TIdealCache<int, int> Cache(1, DataStream);

    Cache.Insert(1, 0, SlowGetPage);
    const SCacheEviction<int> Result = Cache.Insert(2, 1, SlowGetPage);   // 2 больше не встретится

    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 2);
    EXPECT_FALSE(Cache.Contains(2));
    EXPECT_TRUE(Cache.Contains(1));
}

TEST(Ideal, EvictsKeyUsedFarthestInFuture) {
    const std::vector<int> DataStream = {1, 2, 3, 2, 3, 1};
    TIdealCache<int, int> Cache(2, DataStream);

    Cache.Insert(1, 0, SlowGetPage);
    Cache.Insert(2, 1, SlowGetPage);
    const SCacheEviction<int> Result = Cache.Insert(3, 2, SlowGetPage);   // 1 нужен позже всех (позиция 5)

    EXPECT_TRUE(Result.WasEvicted);
    EXPECT_EQ(Result.EvictedKey, 1);
    EXPECT_TRUE(Cache.Contains(2));
}

// ===================== иерархия =====================

TEST(Hierarchy, EvictedKeyIsFoundOnSecondLevel) {
    TCacheRealHierarchy<int, int> Hierarchy({{1, ECacheAlgorithm::Lru}, {1, ECacheAlgorithm::Lru}});

    EXPECT_FALSE(Hierarchy.FindInsert(1, SlowGetPage));
    EXPECT_FALSE(Hierarchy.FindInsert(2, SlowGetPage));             // 1 уходит на уровень 2
    EXPECT_TRUE(Hierarchy.FindInsert(1, SlowGetPage));

    const std::vector<std::size_t> Expected = {0, 1};
    EXPECT_EQ(Hierarchy.GetHitCounts(), Expected);
}

TEST(Hierarchy, SumOfCapacities) {
    TCacheRealHierarchy<int, int> Hierarchy({{3, ECacheAlgorithm::Lru},
                                        {5, ECacheAlgorithm::Lfu},
                                        {10, ECacheAlgorithm::Lirs}});
    EXPECT_EQ(Hierarchy.GetSumCapacities(), 18u);
}

TEST(Hierarchy, HitOnFirstLevelIsCountedOnce) {
    TCacheRealHierarchy<int, int> Hierarchy({{2, ECacheAlgorithm::Lfu}, {2, ECacheAlgorithm::Lru}});

    Hierarchy.FindInsert(7, SlowGetPage);
    EXPECT_TRUE(Hierarchy.FindInsert(7, SlowGetPage));

    const std::vector<std::size_t> Expected = {1, 0};
    EXPECT_EQ(Hierarchy.GetHitCounts(), Expected);
}

// ===================== свойства на случайных данных =====================

// Идеальный кеш по определению не может проиграть ни одному алгоритму той же ёмкости.
TEST(Property, IdealIsNeverWorseThanRealAlgorithms) {
    for (unsigned Seed = 1; Seed <= 5; ++Seed) {
        for (std::size_t Capacity : {2u, 5u, 20u}) {
            SCOPED_TRACE("seed = " + std::to_string(Seed) +
                         ", capacity = " + std::to_string(Capacity));

            const std::vector<int> DataStream = MakeRandomStream(3000, 60, Seed);
            const std::size_t IdealHits = CountIdealHits(Capacity, DataStream);

            TLruCache<int, int>   Lru(Capacity);
            TLfuCache<int, int>   Lfu(Capacity);
            TTwoQCache<int, int>  TwoQ(Capacity);
            TLirsCache<int, int>  Lirs(Capacity);

            EXPECT_LE(CountHits(Lru,  DataStream), IdealHits) << "LRU";
            EXPECT_LE(CountHits(Lfu,  DataStream), IdealHits) << "LFU";
            EXPECT_LE(CountHits(TwoQ, DataStream), IdealHits) << "2Q";
            EXPECT_LE(CountHits(Lirs, DataStream), IdealHits) << "LIRS";
        }
    }
}

// ===================== разбор названий алгоритмов =====================

TEST(Parser, KnownNamesIgnoreCase) {
    EXPECT_EQ(ParseCacheAlgorithm("lru"),   ECacheAlgorithm::Lru);
    EXPECT_EQ(ParseCacheAlgorithm("LfU"),   ECacheAlgorithm::Lfu);
    EXPECT_EQ(ParseCacheAlgorithm("lirs"),  ECacheAlgorithm::Lirs);
    EXPECT_EQ(ParseCacheAlgorithm("2q"),    ECacheAlgorithm::TwoQ);
    EXPECT_EQ(ParseCacheAlgorithm("two-q"), ECacheAlgorithm::TwoQ);
}

TEST(Parser, UnknownNames) {
    EXPECT_EQ(ParseCacheAlgorithm(""),      ECacheAlgorithm::Unknown);
    EXPECT_EQ(ParseCacheAlgorithm("ARC"),   ECacheAlgorithm::Unknown);
    EXPECT_EQ(ParseCacheAlgorithm("LRU2"),  ECacheAlgorithm::Unknown);
}

TEST(Parser, NameRoundTrip) {
    for (ECacheAlgorithm Algorithm : {ECacheAlgorithm::Lru, ECacheAlgorithm::Lfu,
                                      ECacheAlgorithm::TwoQ, ECacheAlgorithm::Lirs}) {
        EXPECT_EQ(ParseCacheAlgorithm(CacheAlgorithmToString(Algorithm)), Algorithm);
    }
}

// ===================== входные данные =====================

TEST(InputReader, ReadsCapacitiesAndData) {
    std::vector<SCacheLevelConfig> LevelConfigs = {{0, ECacheAlgorithm::Lru},
                                                   {0, ECacheAlgorithm::Lfu}};
    std::istringstream Input("3 5\n4\n10 20 10 30");

    const std::vector<int> DataStream = ReadDataAndCapacities(LevelConfigs, Input);

    EXPECT_EQ(LevelConfigs[0].Capacity, 3u);
    EXPECT_EQ(LevelConfigs[1].Capacity, 5u);
    EXPECT_EQ(DataStream, (std::vector<int>{10, 20, 10, 30}));
}

TEST(InputReader, ReadsSingleLevelExample) {
    std::vector<SCacheLevelConfig> LevelConfigs = {{0, ECacheAlgorithm::Lfu}};
    std::istringstream Input("2 6 1 3 2 5 4 3");

    const std::vector<int> DataStream = ReadDataAndCapacities(LevelConfigs, Input);

    EXPECT_EQ(LevelConfigs[0].Capacity, 2u);
    EXPECT_EQ(DataStream, (std::vector<int>{1, 3, 2, 5, 4, 3}));
}

TEST(InputReader, ZeroElementsIsValid) {
    std::vector<SCacheLevelConfig> LevelConfigs = {{0, ECacheAlgorithm::Lru}};
    std::istringstream Input("4 0");
    EXPECT_TRUE(ReadDataAndCapacities(LevelConfigs, Input).empty());
}

TEST(InputReader, BadInputThrows) {
    std::vector<SCacheLevelConfig> LevelConfigs = {{0, ECacheAlgorithm::Lru},
                                                   {0, ECacheAlgorithm::Lru}};

    std::istringstream Empty("");
    EXPECT_THROW(ReadDataAndCapacities(LevelConfigs, Empty), std::runtime_error);

    std::istringstream MissingCapacity("4");
    EXPECT_THROW(ReadDataAndCapacities(LevelConfigs, MissingCapacity), std::runtime_error);

    std::istringstream NotEnoughData("4 4 5 1 2 3");
    EXPECT_THROW(ReadDataAndCapacities(LevelConfigs, NotEnoughData), std::runtime_error);

    std::istringstream NotANumber("4 4 3 1 x 2");
    EXPECT_THROW(ReadDataAndCapacities(LevelConfigs, NotANumber), std::runtime_error);
}

// ===================== конфиг-файл =====================

TEST(ConfigReader, ReadsValidConfig) {
    const std::vector<SCacheLevelConfig> LevelConfigs =
        ReadCacheConfig(WriteTempConfig("3\nLRU\n2Q\nlirs\n"));

    ASSERT_EQ(LevelConfigs.size(), 3u);
    EXPECT_EQ(LevelConfigs[0].Algorithm, ECacheAlgorithm::Lru);
    EXPECT_EQ(LevelConfigs[1].Algorithm, ECacheAlgorithm::TwoQ);
    EXPECT_EQ(LevelConfigs[2].Algorithm, ECacheAlgorithm::Lirs);
}

TEST(ConfigReader, BadConfigThrows) {
    EXPECT_THROW(ReadCacheConfig(WriteTempConfig("0\n")),        std::runtime_error);
    EXPECT_THROW(ReadCacheConfig(WriteTempConfig("")),           std::runtime_error);
    EXPECT_THROW(ReadCacheConfig(WriteTempConfig("2\nLRU\n")),   std::runtime_error);
    EXPECT_THROW(ReadCacheConfig(WriteTempConfig("1\nARC\n")),   std::invalid_argument);
    EXPECT_THROW(ReadCacheConfig("/nonexistent/cache_sim.cfg"), std::runtime_error);
}