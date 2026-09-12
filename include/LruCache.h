#pragma once

#include <list>

#include "CacheLevel.h"

// Запись LRU: только позиция ключа в списке давности. Призрачных записей
// у LRU нет — всё, что лежит в таблице, резидентно.
template <typename KeyType>
struct SLruEntry {
    typename std::list<KeyType>::iterator Position;

    bool IsResident() const {
        return true;
    }
};  

// Классическая реализация LRU на стандартных контейнерах:
// список хранит порядок использования (голова — самый свежий элемент),
// хеш-таблица базы даёт O(1) доступ к итератору элемента в списке.
template <typename KeyType>
class TLruCache : public TCacheLevelBase<KeyType, SLruEntry<KeyType>> {
public:
    explicit TLruCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType, SLruEntry<KeyType>>(ECacheAlgorithm::Lru, Capacity) {}

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        SCacheEviction<KeyType> Result{};

        auto &Entries = this->GetEntries();
        const auto FoundIt = Entries.find(Key);
        if (FoundIt != Entries.end()) { // hit
            RecencyList_.erase(FoundIt->second.Position);
            RecencyList_.push_front(Key);
            return Result;
        }

        // miss
        if (RecencyList_.size() >= this->Capacity_) { 
            const KeyType VictimKey = RecencyList_.back();
            Result.WasEvicted = true;
            Result.EvictedKey = VictimKey;
            Entries.erase(VictimKey);
            RecencyList_.pop_back();
        }

        RecencyList_.push_front(Key);
        Entries[Key] = SLruEntry<KeyType>{RecencyList_.begin()};
        return Result;
    }

private:
    std::list<KeyType> RecencyList_;
};
