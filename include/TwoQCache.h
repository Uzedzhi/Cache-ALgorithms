#pragma once

#include <list>
#include <stdexcept>
#include <unordered_map>

#include "../MyCppLibs/sassert.h"
#include "CacheLevel.h"

enum class ETwoQLocation { A1in, A1out, Am };
template <typename KeyType>
struct STwoQEntry {
    ETwoQLocation Location = ETwoQLocation::A1in;
    typename std::list<KeyType>::iterator Position;

    bool IsResident() const {
        return Location == ETwoQLocation::A1in || Location == ETwoQLocation::Am;
    }
};

template <typename KeyType>
class TTwoQCache {
    using Eviction  = SCacheEviction<KeyType>;
    using TMap     = std::unordered_map<KeyType, STwoQEntry<KeyType>>;
    TMap Entries_;

    std::size_t A1inCapacity_   = 0;
    std::size_t A1outCapacity_  = 0;
    std::size_t AmCapacity_     = 0;

    std::list<KeyType> A1inList_;
    std::list<KeyType> A1outList_;
    std::list<KeyType> AmList_;
    std::size_t Capacity_;
public:
    explicit TTwoQCache(std::size_t Capacity) : Capacity_(Capacity) {
        CHECK_EX(Capacity >= 2, std::invalid_argument,  "Ошибка создания 2Q кеша: " 
                                                        "нельзя создать кеш с емкостью меньше 2");

        A1inCapacity_  = Capacity / 4;
        A1outCapacity_ = Capacity / 2;
        AmCapacity_    = Capacity - A1inCapacity_ - A1outCapacity_;
    }

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end() && FoundEl->second.Location != ETwoQLocation::A1out;
    }

    Eviction Insert(const KeyType& Key) {
        auto FoundIt = Entries_.find(Key);

        // miss
        if (FoundIt == Entries_.end()) {
            return InsertNewKey(Key);
        }

        // hit
        switch (FoundIt->second.Location) {
            // если из Am, то он находится в горячей точке(LRU) просто передвигаем его в начало
            case ETwoQLocation::Am:
                AmList_.splice(AmList_.begin(), AmList_, FoundIt->second.Position);
                return {};
            // если из A1in, то ничего не делаем
            case ETwoQLocation::A1in:
                return {};
            // если из A1out, то он доказал свою полезности и повышается в Am
            case ETwoQLocation::A1out:
                return PromoteFromOutToAm(Key, FoundIt);
        }
        return {};
    }

private:
    Eviction PromoteFromOutToAm(const KeyType& Key, typename TMap::iterator FoundIt) {
        A1outList_.erase(FoundIt->second.Position);
        Entries_.erase(FoundIt);

        return InsertIntoMainArea(Key);
    }

    Eviction InsertIntoMainArea(const KeyType& Key) {
        Eviction Result{};
        AmList_.push_front(Key);
        Entries_[Key] = STwoQEntry<KeyType>{ETwoQLocation::Am, AmList_.begin()};

        if (AmList_.size() > AmCapacity_) {
            const KeyType Victim = AmList_.back();
            AmList_.pop_back();
            Entries_.erase(Victim);
            Result.WasEvicted = true;
            Result.EvictedKey = Victim;
        }
        return Result;
    }

    Eviction InsertNewKey(const KeyType& Key) {
        Eviction Result{};
        A1inList_.push_front(Key);
        Entries_[Key] = STwoQEntry<KeyType>{ETwoQLocation::A1in, A1inList_.begin()};

        if (A1inList_.size() > A1inCapacity_) {
            const KeyType EvictedKey = A1inList_.back();
            A1inList_.pop_back();

            // Данные реально покидают резидентный кеш — это настоящее
            // вытеснение, которое должно каскадом уйти на следующий уровень.
            Result.WasEvicted = true;
            Result.EvictedKey = EvictedKey;

            A1outList_.push_front(EvictedKey);
            Entries_[EvictedKey] = STwoQEntry<KeyType>{ETwoQLocation::A1out, A1outList_.begin()};

            if (A1outList_.size() > A1outCapacity_) {
                const KeyType GhostDrop = A1outList_.back();
                A1outList_.pop_back();
                Entries_.erase(GhostDrop);
            }
        }
        return Result;
    }
};
