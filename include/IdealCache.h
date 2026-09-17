#pragma once

#include <deque>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

#include "CacheLevel.h"

struct SIdealEntry {
    std::size_t NextUse = 0;
};

template <typename KeyType>
class TIdealCache {
    using Eviction = SCacheEviction<KeyType>;
    static constexpr std::size_t kNeverAgain = std::numeric_limits<std::size_t>::max();

    std::set<std::pair<std::size_t, KeyType>>               OrderedByNextUse_;
    std::unordered_map<KeyType, std::deque<std::size_t>>    FutureOccurrences_;
    std::unordered_map<KeyType, SIdealEntry>                Entries_;
    std::size_t Capacity_;
public:
    TIdealCache(std::size_t Capacity, const std::vector<KeyType>& DataStream)
        : Capacity_(Capacity) {
        for (std::size_t Index = 0; Index < DataStream.size(); ++Index) {
            FutureOccurrences_[DataStream[Index]].push_back(Index);
        }
    }

    bool Contains(const KeyType& Key) const {
        return Entries_.find(Key) != Entries_.end();
    }

    Eviction Insert(const KeyType& Key, std::size_t Position) {
        Eviction Result;

        // Текущее обращение "потребляет" ближайшее известное вхождение
        // этого ключа — дальше для него актуальны только будущие позиции.
        auto& OwnFutureUses = FutureOccurrences_[Key];
        if (!OwnFutureUses.empty() && OwnFutureUses.front() <= Position)
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
};
