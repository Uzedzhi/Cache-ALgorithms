#pragma once

#include <list>
#include <unordered_map>

#include "CacheLevel.h"

// Запись LFU: текущая частота обращений и позиция ключа в списке своей
// частоты. Призрачных записей у LFU нет.
template <typename KeyType>
struct SLfuEntry {
    int Frequency = 0;
    typename std::list<KeyType>::iterator PositionInFrequencyList;

    bool IsResident() const { return true; }
};

// LFU с O(1) операциями (классическая схема Yang/Shao):
// каждому ключу сопоставлена частота обращений; ключи одной частоты лежат
// в своём списке (порядок внутри списка — recency, для tie-break при
// вытеснении); отдельно отслеживается минимальная используемая частота,
// чтобы не искать жертву перебором.
template <typename KeyType>
class TLfuCache : public TCacheLevelBase<KeyType, SLfuEntry<KeyType>> {
public:
    explicit TLfuCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType, SLfuEntry<KeyType>>(ECacheAlgorithm::Lfu, Capacity) {}

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        SCacheEviction<KeyType> Result;

        const auto FoundIt = this->Entries_.find(Key);
        if (FoundIt != this->Entries_.end()) {
            Touch(FoundIt);
            return Result;
        }

        if (this->Entries_.size() >= this->Capacity_) {
            auto& VictimList = FrequencyToKeys_[MinFrequency_];
            const KeyType VictimKey = VictimList.back();
            VictimList.pop_back();
            if (VictimList.empty()) {
                FrequencyToKeys_.erase(MinFrequency_);
            }
            this->Entries_.erase(VictimKey);

            Result.WasEvicted = true;
            Result.EvictedKey = VictimKey;
        }

        FrequencyToKeys_[1].push_front(Key);
        this->Entries_[Key] = SLfuEntry<KeyType>{1, FrequencyToKeys_[1].begin()};
        MinFrequency_ = 1;
        return Result;
    }

private:
    using TEntryIt = typename TCacheLevelBase<KeyType, SLfuEntry<KeyType>>::TEntryIt;

    void Touch(TEntryIt NodeIt) {
        const KeyType Key = NodeIt->first;
        const int OldFrequency = NodeIt->second.Frequency;
        const int NewFrequency = OldFrequency + 1;

        auto& OldList = FrequencyToKeys_[OldFrequency];
        OldList.erase(NodeIt->second.PositionInFrequencyList);
        if (OldList.empty()) {
            FrequencyToKeys_.erase(OldFrequency);
            if (MinFrequency_ == OldFrequency) {
                MinFrequency_ = NewFrequency;
            }
        }

        FrequencyToKeys_[NewFrequency].push_front(Key);
        NodeIt->second.Frequency = NewFrequency;
        NodeIt->second.PositionInFrequencyList = FrequencyToKeys_[NewFrequency].begin();
    }

    int MinFrequency_ = 0;
    std::unordered_map<int, std::list<KeyType>> FrequencyToKeys_;
};
