#pragma once

#include <list>
#include <unordered_map>

#include "CacheLevel.h"

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
class TLruCache {
    using Eviction = SCacheEviction<KeyType>;
    std::unordered_map<KeyType, std::list<int>::iterator> Entries_;
    std::list<int> RecencyList_;
    std::size_t Capacity_;
public:
    explicit TLruCache(std::size_t Capacity) : Capacity_(Capacity) {}

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end();
    }

    Eviction Insert(const KeyType& Key) {
        Eviction Result{};
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
};
