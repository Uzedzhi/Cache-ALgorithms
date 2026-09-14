#pragma once

#include <cstddef>
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
template <typename KeyType>
using TCacheLevel = std::variant<
    TLruCache<KeyType>,
    TLfuCache<KeyType>,
    TTwoQCache<KeyType>,
    TLirsCache<KeyType>,
    TIdealCache<KeyType>>;

// Обёртка-интерфейс: собрать уровень, зная только его конфиг. Поток данных
// нужен исключительно идеальному кешу (он заглядывает в будущее); все
// остальные алгоритмы его игнорируют.
template <typename KeyType>
TCacheLevel<KeyType> MakeCacheLevel(const SCacheLevelConfig& Config,
                                    const std::vector<KeyType>& DataStream) {
    switch (Config.Algorithm) {
        case ECacheAlgorithm::Lru:   return TLruCache<KeyType>(Config.Capacity);
        case ECacheAlgorithm::Lfu:   return TLfuCache<KeyType>(Config.Capacity);
        case ECacheAlgorithm::TwoQ:  return TTwoQCache<KeyType>(Config.Capacity);
        case ECacheAlgorithm::Lirs:  return TLirsCache<KeyType>(Config.Capacity);
        case ECacheAlgorithm::Ideal: return TIdealCache<KeyType>(Config.Capacity, DataStream);
        case ECacheAlgorithm::Unknown: break;
    }

    CHECK_EX(false, std::invalid_argument,
             "MakeCacheLevel: нераспознанный алгоритм уровня кеша.");
    return TLruCache<KeyType>(Config.Capacity); // недостижимо, глушит -Wreturn-type
}

// Группа уровней кеша ("заранее инициализированная структура с кешами и
// методами", как и было задумано в архитектуре). Один и тот же класс
// используется и для реальной N-уровневой иерархии из конфига, и для
// эталонного прогона, где каждый уровень заменён идеальным кешем той же
// ёмкости — CacheFind()/CacheInsert() при этом не меняются вообще.
template <typename KeyType>
class TCacheHierarchy {
public:
    TCacheHierarchy(const std::vector<SCacheLevelConfig>& LevelConfigs,
                    const std::vector<KeyType>& DataStream) {
        CHECK_EX(!LevelConfigs.empty(), std::invalid_argument,
                 "TCacheHierarchy: должен быть хотя бы один уровень кеша.");

        Levels_.reserve(LevelConfigs.size());
        for (const SCacheLevelConfig& Config : LevelConfigs) {
            Levels_.push_back(MakeCacheLevel<KeyType>(Config, DataStream));
        }
        HitCounts_.assign(Levels_.size(), 0);
    }

    // Обрабатывает одно обращение к Key на позиции Position во входном
    // потоке. Возвращает true, если хит произошёл хоть на одном уровне.
    bool CacheFind(const KeyType& Key, std::size_t Position) {
        for (TCacheLevel<KeyType>& Level : Levels_) {
            if (auto *Algorithm = std::get_if<TIdealCache<KeyType>>(&Level))
                Algorithm->NotifyStreamPosition(Position);
        }

        bool bAnyHit = false;
        for (std::size_t Index = 0; Index < Levels_.size(); ++Index) {
            const bool bContains = std::visit([&Key](const auto &Impl){return Impl.Contains(Key);}, Levels_[Index]);
            if (bContains) {
                ++HitCounts_[Index];
                bAnyHit = true;
                break;
            }
        }

        // Вне зависимости от того, был хит или полный промах, — поднимаем
        // ключ на самый верхний уровень (0). Если он там уже лежит, это
        // просто "touch" без вытеснений. Если хит случился глубже —
        // операция каскадом протолкнёт ключ наверх, спуская вытесняемые по
        // пути элементы вниз. Если это был полный промах по всем уровням —
        // ключ просто вставляется в уровень 0, как и требовалось.
        CacheInsert(0, Key);

        return bAnyHit;
    }

    const std::vector<std::size_t>& GetHitCountsPerLevel() const {
        return HitCounts_;
    
    }
    std::size_t GetLevelCount() const { 
        return Levels_.size();
    }

    ECacheAlgorithm GetLevelAlgorithm(std::size_t Index) const {
        return std::visit([](const auto& Impl) { return Impl.GetAlgorithm(); }, Levels_.at(Index));
    }

    std::size_t GetLevelCapacity(std::size_t Index) const {
        return std::visit([](const auto& Impl) { return Impl.GetCapacity(); }, Levels_.at(Index));
    }

private:
    // Рекурсивно (каскадом) вставляет Key на уровень LevelIndex; если это
    // вызывает вытеснение, вытесненный ключ передаётся на следующий
    // уровень тем же способом. Если LevelIndex вышел за пределы иерархии,
    // данные считаются окончательно покинувшими симулируемую систему.
    void CacheInsert(std::size_t LevelIndex, const KeyType& Key) {
        if (LevelIndex >= Levels_.size()) {
            return;
        }

        const SCacheEviction<KeyType> EvictedKey = std::visit(
            [&Key](auto& Impl) { // lambda
                return Impl.Insert(Key); 
            },
            Levels_[LevelIndex] // variant
        );

        if (EvictedKey.WasEvicted) {
            CacheInsert(LevelIndex + 1, EvictedKey.EvictedKey);
        }
    }

    std::vector<TCacheLevel<KeyType>> Levels_;
    std::vector<std::size_t> HitCounts_;
};
