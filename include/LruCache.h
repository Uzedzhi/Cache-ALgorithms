#pragma once

#include <list>
#include <unordered_map>
#include <vector>

#include "CacheLevel.h"
#include "CacheTypes.h"

// // Запись LRU: только позиция ключа в списке давности. Призрачных записей
// // у LRU нет — всё, что лежит в таблице, резидентно.
// template <typename KeyType>
// struct SLruEntry {
//     typename std::list<KeyType>::iterator Position;

//     bool IsResident() const {
//         return true;
//     }
// };

// Классическая реализация LRU на стандартных контейнерах:
// список хранит порядок использования (голова — самый свежий элемент),
// хеш-таблица базы даёт O(1) доступ к итератору элемента в списке.
template <typename KeyType>
class TLruCache : public TCacheLevelBase<KeyType> {
public:
    explicit TLruCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType>(ECacheAlgorithm::Lru, Capacity) {}

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end();
    }

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        SCacheEviction<KeyType> Result{};
        const auto FoundIt = Entries_.find(Key);

        if (FoundIt != Entries_.end()) {
            RecencyList_.splice(RecencyList_.begin(), RecencyList_, FoundIt->second);
            return Result;
        }

        // miss
        if (RecencyList_.size() >= this->Capacity_) { 
            const KeyType VictimKey = RecencyList_.back();
            Result.WasEvicted = true;
            Result.EvictedKey = VictimKey;
            Entries_.erase(VictimKey);
            RecencyList_.pop_back();
        }

        RecencyList_.push_front(Key);
        Entries_[Key] = RecencyList_.begin();
        return Result;
    }

private:
    // каждому ключу соответствует его позиция в списке на вытеснение
    std::unordered_map<KeyType, std::list<int>::iterator> Entries_;
    std::list<int> RecencyList_;
};
