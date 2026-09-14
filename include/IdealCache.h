#pragma once

#include <cerrno>
#include <deque>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

#include "CacheLevel.h"
#include "CacheTypes.h"

// Запись идеального кеша: позиция ближайшего будущего обращения к ключу.
// Призрачных записей нет — в таблице лежат только резиденты.
struct SIdealEntry {
    std::size_t NextUse = 0;
};

// Идеальный кеш (алгоритм Белади / MIN): на каждом промахе вытесняет тот
// резидентный ключ, который понадобится позже всех остальных (либо не
// понадобится вообще). Это доказанно оптимальная offline-стратегия —
// она "подглядывает" в будущее, поэтому годится только как эталон для
// сравнения с реальными online-алгоритмами, а не как практический кеш.
//
// Наследуется от той же TCacheLevelBase, что и все остальные уровни, и
// получает от неё готовые Contains()/GetCapacity()/GetAlgorithm(). Из-за
// этого попадает в тот же std::variant (TCacheLevel) и обслуживается той
// же парой CacheFind()/CacheInsert() (см. MultiLevelCache.h).
//
// Единственное отличие — непустой NotifyStreamPosition(): он перекрывает
// (через сокрытие имени, не через virtual) пустой хук базы. Конкретный тип
// уровня всегда известен статически, поэтому нужная версия метода
// выбирается на этапе компиляции и никакого vtable не нужно.
//
// Жертва вытеснения ищется через std::set<{следующее использование, ключ}>
// за O(log C) — "никогда больше не понадобится" кодируется как +infinity,
// что автоматически ставит такой ключ в конец множества (самый выгодный
// кандидат на вытеснение), без отдельного разбора случаев.
template <typename KeyType>
class TIdealCache : public TCacheLevelBase<KeyType> {
    static constexpr std::size_t kNeverAgain = std::numeric_limits<std::size_t>::max();
    std::size_t CurrentPosition_ = 0;

    std::set<std::pair<std::size_t, KeyType>>               OrderedByNextUse_;
    std::unordered_map<KeyType, std::deque<std::size_t>>    FutureOccurrences_;
    std::unordered_map<KeyType, SIdealEntry>                Entries_;
    
public:
    TIdealCache(std::size_t Capacity, const std::vector<KeyType>& DataStream)
        : TCacheLevelBase<KeyType>(ECacheAlgorithm::Ideal, Capacity) {
        for (std::size_t Index = 0; Index < DataStream.size(); ++Index) {
            FutureOccurrences_[DataStream[Index]].push_back(Index);
        }
    }

    bool Contains(const KeyType& Key) const {
        return Entries_.find(Key) != Entries_.end();
    }

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        SCacheEviction<KeyType> Result;

        // Текущее обращение "потребляет" ближайшее известное вхождение
        // этого ключа — дальше для него актуальны только будущие позиции.
        auto& OwnFutureUses = FutureOccurrences_[Key];
        if (!OwnFutureUses.empty() && OwnFutureUses.front() <= CurrentPosition_)
            OwnFutureUses.pop_front();

        const std::size_t NewNextUse = OwnFutureUses.empty() ? kNeverAgain : OwnFutureUses.front();
        const auto FoundIt = Entries_.find(Key);
        if (FoundIt != Entries_.end()) {
            OrderedByNextUse_.erase({FoundIt->second.NextUse, Key});
            OrderedByNextUse_.insert({NewNextUse, Key});
            FoundIt->second.NextUse = NewNextUse;
            return Result;
        }

        if (Entries_.size() >= this->Capacity_) {
            const auto VictimIt = std::prev(OrderedByNextUse_.end());
            if (VictimIt->first <= NewNextUse) {
                Result.WasEvicted = true;
                Result.EvictedKey = Key;
                return Result;
            }
            const KeyType VictimKey = VictimIt->second;
            OrderedByNextUse_.erase(VictimIt);
            Entries_.erase(VictimKey);
            Result.WasEvicted = true;
            Result.EvictedKey = VictimKey;
        }

        Entries_[Key] = SIdealEntry{NewNextUse};
        OrderedByNextUse_.insert({NewNextUse, Key});
        return Result;
    }

    void NotifyStreamPosition(std::size_t CurrentPosition) {
        CurrentPosition_ = CurrentPosition;
    }
};
