#pragma once

#include <list>
#include <unordered_map>
#include <optional>

#include "CacheLevel.h"

// // Запись LRU: только позиция ключа в списке давности. Призрачных записей
// // у LRU нет — всё, что лежит в таблице, резидентно.
template <typename KeyType, typename PageType>
struct SLruEntry {
    typename std::list<KeyType>::iterator Position;

    std::optional<PageType> Page = std::nullopt;
};

// Классическая реализация LRU на стандартных контейнерах:
// список хранит порядок использования (голова — самый свежий элемент),
// хеш-таблица базы даёт O(1) доступ к итератору элемента в списке.
template <typename KeyType, typename PageType>
class TLruCache {
    using list = std::list<KeyType>;
    using Eviction = SCacheEviction<KeyType>;
    std::unordered_map<KeyType, SLruEntry<KeyType, PageType>> Entries_;
    list RecencyList_;
    std::size_t Capacity_;
public:
    explicit TLruCache(std::size_t Capacity) : Capacity_(Capacity) {}

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end();
    }

    template <typename F>Eviction Insert(const KeyType& Key, F SlowGetPage) {
        Eviction Result{};
        const auto FoundIt = Entries_.find(Key);

        if (FoundIt != Entries_.end()) {
            RecencyList_.splice(RecencyList_.begin(), RecencyList_, FoundIt->second.Position);
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


        PageType Page = SlowGetPage(Key);
        RecencyList_.emplace_front(Key);
        Entries_[Key] = SLruEntry<KeyType, PageType>{RecencyList_.begin(), Page};
        return Result;
    }
};
