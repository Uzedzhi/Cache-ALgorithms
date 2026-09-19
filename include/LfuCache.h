#pragma once

#include <cstddef>
#include <list>
#include <unordered_map>
#include <optional>

#include "CacheLevel.h"

template <typename KeyType, typename PageType>
struct SLfuEntry {
    std::size_t Frequency = 0;
    typename std::list<KeyType>::iterator PositionInFrequencyList;
    
    std::optional<PageType> Page = std::nullopt;
};

//   - Entries_         : ключ -> {частота, позиция в списке частоты};
//   - FrequencyToKeys_ : частота -> ключи с этой частотой, голова списка —
//                        самый свежий, хвост — самый давний (tie-break);
//   - MinFrequency_    : минимальная частота среди лежащих в кеше ключей,
//                        из её списка и берётся жертва.
template <typename KeyType, typename PageType>
class TLfuCache {
    using Eviction = SCacheEviction<KeyType>;
    using TEntry   = SLfuEntry<KeyType, PageType>;
    using TEntryIt = typename std::unordered_map<KeyType, SLfuEntry<KeyType, PageType>>::iterator;
    using TKeyList = typename std::list<KeyType>;

    std::size_t MinFrequency_ = 0;
    std::unordered_map<std::size_t, TKeyList> FrequencyToKeys_;
    std::unordered_map<KeyType, SLfuEntry<KeyType, PageType>> Entries_;
    std::size_t Capacity_;
public:
    TLfuCache(std::size_t Capacity) : Capacity_(Capacity) {}

    // Хит — ключ сейчас лежит в кеше.
    bool Contains(const KeyType& Key) const {
        return Entries_.find(Key) != Entries_.end();
    }

    // Обращение к Key: при хите повышает частоту, при промахе вставляет
    // ключ (вытеснив наименее часто используемый, если кеш полон).
    template <typename F>  Eviction Insert(const KeyType& Key, F SlowGetPage) {
        const auto FoundIt = Entries_.find(Key);
        if (FoundIt != Entries_.end()) { // hit
            IncrementFrequency(FoundIt);
            return {};
        }

        Eviction Result;
        if (Entries_.size() >= Capacity_) { // miss
            Result = EvictLeastFrequent();
        }

        AddNewKey(Key, SlowGetPage);
        return Result;
    }

private:
    void IncrementFrequency(TEntryIt FoundIt) {
        TEntry& Entry = FoundIt->second;
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

    template <typename F> void AddNewKey(const KeyType& Key, F SlowGetPage) {
        TKeyList& FirstList = FrequencyToKeys_[1];
        FirstList.push_front(Key);
        Entries_[Key] = TEntry{1, FirstList.begin(), SlowGetPage(Key)};
        MinFrequency_ = 1;

    }
};
