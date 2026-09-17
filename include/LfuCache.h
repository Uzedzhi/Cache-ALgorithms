#pragma once

#include <cstddef>
#include <list>
#include <unordered_map>

#include "CacheLevel.h"

template <typename KeyType>
struct SLfuEntry {
    std::size_t Frequency = 0;
    typename std::list<KeyType>::iterator PositionInFrequencyList;
};

//   - Entries_         : ключ -> {частота, позиция в списке частоты};
//   - FrequencyToKeys_ : частота -> ключи с этой частотой, голова списка —
//                        самый свежий, хвост — самый давний (tie-break);
//   - MinFrequency_    : минимальная частота среди лежащих в кеше ключей,
//                        из её списка и берётся жертва.
template <typename KeyType>
class TLfuCache {
    using Eviction = SCacheEviction<KeyType>;
    using TEntryIt = typename std::unordered_map<KeyType, SLfuEntry<KeyType>>::iterator;
    using TKeyList = typename std::list<KeyType>;

    std::size_t MinFrequency_ = 0;
    std::unordered_map<std::size_t, TKeyList> FrequencyToKeys_;
    std::unordered_map<KeyType, SLfuEntry<KeyType>> Entries_;
    std::size_t Capacity_;
public:
    TLfuCache(std::size_t Capacity) : Capacity_(Capacity) {}

    // Хит — ключ сейчас лежит в кеше.
    bool Contains(const KeyType& Key) const {
        return Entries_.find(Key) != Entries_.end();
    }

    // Обращение к Key: при хите повышает частоту, при промахе вставляет
    // ключ (вытеснив наименее часто используемый, если кеш полон).
    Eviction Insert(const KeyType& Key) {
        const auto FoundIt = Entries_.find(Key);
        if (FoundIt != Entries_.end()) { // hit
            IncrementFrequency(FoundIt);
            return {};
        }

        Eviction Result;
        if (Entries_.size() >= Capacity_) { // miss
            Result = EvictLeastFrequent();
        }
        AddNewKey(Key);
        return Result;
    }

private:
    void IncrementFrequency(TEntryIt FoundIt) {
        SLfuEntry<KeyType>& Entry = FoundIt->second;
        const std::size_t OldFrequency = Entry.Frequency;

        // Ссылки на значения unordered_map переживают рехеш, так что
        // обращение к NewList (возможная вставка) не портит OldList.
        TKeyList& NewList = FrequencyToKeys_[OldFrequency + 1];
        TKeyList& OldList = FrequencyToKeys_[OldFrequency];

        // из old_list[Entry.Position] -> New_list.front()
        NewList.splice(NewList.begin(), OldList, Entry.PositionInFrequencyList);
        Entry.Frequency = OldFrequency + 1;

        if (OldList.empty()) {
            FrequencyToKeys_.erase(OldFrequency);
            if (MinFrequency_ == OldFrequency) {
                MinFrequency_ = OldFrequency + 1;
            }
        }
    }

    Eviction EvictLeastFrequent() {
        const auto MinListIt = FrequencyToKeys_.find(MinFrequency_);
        
        TKeyList& VictimList = MinListIt->second;
        Eviction Result;
        Result.WasEvicted = true;
        Result.EvictedKey = VictimList.back();

        VictimList.pop_back();
        if (VictimList.empty()) {
            FrequencyToKeys_.erase(MinListIt);
        }
        Entries_.erase(Result.EvictedKey);
        return Result;
    }

    void AddNewKey(const KeyType& Key) {
        TKeyList& FirstList = FrequencyToKeys_[1];
        FirstList.push_front(Key);
        Entries_[Key] = SLfuEntry<KeyType>{1, FirstList.begin()};
        MinFrequency_ = 1;
    }
};
