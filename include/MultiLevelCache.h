#pragma once

#include <cstddef>
#include <stdexcept>
#include <variant>
#include <vector>

#include "CacheTypes.h"
#include "CacheLevel.h"
#include "LruCache.h"
#include "LfuCache.h"
#include "TwoQCache.h"
#include "LirsCache.h"
#include "IdealCache.h"
#include "../MyCppLibs/sassert.h"

// Один уровень иерархии — ровно один из шести конкретных типов. Все они
// наследуют TCacheLevelBase, то есть гарантированно умеют
// Contains/GetCapacity/GetAlgorithm/NotifyStreamPosition/Insert, поэтому
// обобщённая лямбда в std::visit одинаково работает с любым из них.
//
// std::variant вместо "виртуальный интерфейс + unique_ptr": тип уровня
// известен статически, уровень лежит в векторе по значению (без кучи),
// диспетчеризация разворачивается компилятором, vtable не создаётся.
template <typename KeyType, typename PageType>
using TCacheLevel = std::variant<
    TLruCache<KeyType,  PageType>,
    TLfuCache<KeyType,  PageType>,
    TTwoQCache<KeyType, PageType>,
    TLirsCache<KeyType, PageType>>;

template <typename KeyType, typename PageType>
class TCacheRealHierarchy {
public:
    TCacheRealHierarchy(const std::vector<SCacheLevelConfig>& LevelConfigs) {
        CHECK_EX(!LevelConfigs.empty(), std::invalid_argument,
                 "TCacheRealHierarchy: должен быть хотя бы один уровень кеша.");

        Levels_.reserve(LevelConfigs.size());
        for (const SCacheLevelConfig& Config : LevelConfigs) {
            Levels_.push_back(MakeCacheLevel(Config));
            SumCapacities_ += Config.Capacity;
        }
        HitCounts_.assign(Levels_.size(), 0);
    }

    template <typename F> bool FindInsert(const KeyType& Key, F SlowGetPage) {
        bool bAnyHit = false;
        for (std::size_t Index = 0; Index < Levels_.size(); ++Index) {
            const bool bContains = std::visit([&Key](const auto &Impl){return Impl.Contains(Key);}, Levels_[Index]);
            if (bContains) {
                ++HitCounts_[Index];
                bAnyHit = true;
                break;
            }
        }

        insert(0, Key, SlowGetPage);
        return bAnyHit;
    }

    std::size_t GetSumCapacities() const {
        return SumCapacities_;
    }

    const std::vector<std::size_t>& GetHitCounts() const {
        return HitCounts_;
    }

    TCacheLevel<KeyType, PageType> GetLevel(std::size_t index) const {
        CHECK_EX(index < Levels_.size(), std::out_of_range,
                 "GetLevel: не существует такого уровня кеша");

        return Levels_[index];
    }

private:
    static TCacheLevel<KeyType, PageType> MakeCacheLevel(const SCacheLevelConfig& Config) {
        switch (Config.Algorithm) {
            case ECacheAlgorithm::Lru:  return TLruCache<KeyType,  PageType>(Config.Capacity);
            case ECacheAlgorithm::Lfu:  return TLfuCache<KeyType,  PageType>(Config.Capacity);
            case ECacheAlgorithm::TwoQ: return TTwoQCache<KeyType, PageType>(Config.Capacity);
            case ECacheAlgorithm::Lirs: return TLirsCache<KeyType, PageType>(Config.Capacity);
            default:
                throw std::runtime_error("Неправильный тип кеширования, не могу построить иерархию кешей");
        }
    }

    template <typename F> void insert(std::size_t LevelIndex, const KeyType& Key, F SlowGetPage) {
        if (LevelIndex >= Levels_.size()) {
            return;
        }

        const SCacheEviction<KeyType> EvictedKey = std::visit(
            [&Key, &SlowGetPage](auto& Impl) { // lambda
                return Impl.Insert(Key, SlowGetPage); 
            },
            Levels_[LevelIndex] // variant
        );

        if (EvictedKey.WasEvicted) {
            insert(LevelIndex + 1, EvictedKey.EvictedKey, SlowGetPage);
        }
    }

    std::vector<TCacheLevel<KeyType, PageType>> Levels_;
    std::vector<std::size_t> HitCounts_;
    std::size_t SumCapacities_ = 0;
};

template <typename KeyType, typename PageType>
class TCacheIdealHierarchy {
    public:
        TCacheIdealHierarchy(const std::size_t capacity, std::vector<KeyType> DataStream)
            : Cache_(capacity, DataStream), HitCount_(0) {}

        template <typename F> bool FindInsert(const KeyType& Key, std::size_t Position, F SlowGetPage) {
            bool hit = Cache_.Contains(Key);
            Cache_.Insert(Key, Position, SlowGetPage);
            if (hit) {
                ++HitCount_;
            }
            return hit;
        }
        std::size_t HitCount_;

    private:
        TIdealCache<KeyType, PageType> Cache_;
};