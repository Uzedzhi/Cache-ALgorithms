#pragma once

#include <list>
#include <stdexcept>
#include <optional>
#include <unordered_map>

#include "../MyCppLibs/sassert.h"
#include "CacheLevel.h"

enum class ETwoQLocation { A1in, A1out, Am };
template <typename KeyType, typename PageType>
struct STwoQEntry {
    ETwoQLocation Location = ETwoQLocation::A1in;
    typename std::list<KeyType>::iterator Position;

    bool IsResident() const {
        return Location == ETwoQLocation::A1in || Location == ETwoQLocation::Am;
    }

    std::optional<PageType> Page = std::nullopt;
};

template <typename KeyType, typename PageType>
class TTwoQCache {
    using TEntry   = STwoQEntry<KeyType, PageType>;
    using Eviction = SCacheEviction<KeyType>;
    using TMap     = std::unordered_map<KeyType, TEntry>;
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

        A1inCapacity_  = std::max<int>(1, Capacity / 4);
        A1outCapacity_ = Capacity / 2;
        AmCapacity_    = Capacity - A1inCapacity_ - A1outCapacity_;
    }

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end() && FoundEl->second.Location != ETwoQLocation::A1out;
    }

    template <typename F> Eviction Insert(const KeyType& Key, F SlowGetPage) {
        auto FoundIt = Entries_.find(Key);

        // miss
        if (FoundIt == Entries_.end()) {
            return InsertNewKey(Key, SlowGetPage);
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
                return PromoteFromOutToAm(Key, FoundIt, SlowGetPage);
        }
        return {};
    }

private:
    template <typename F>
    Eviction PromoteFromOutToAm(const KeyType& Key, typename TMap::iterator FoundIt, F SlowGetPage) {
        A1outList_.erase(FoundIt->second.Position);
        Entries_.erase(FoundIt);

        return InsertIntoMainArea(Key, SlowGetPage);
    }

    template <typename F> Eviction InsertIntoMainArea(const KeyType& Key, F SlowGetPage) {
        Eviction Result{};
        AmList_.push_front(Key);
        Entries_[Key] = TEntry{ETwoQLocation::Am, AmList_.begin(), SlowGetPage(Key)};

        if (AmList_.size() > AmCapacity_) {
            const KeyType Victim = AmList_.back();
            AmList_.pop_back();
            Entries_.erase(Victim);
            Result.WasEvicted = true;
            Result.EvictedKey = Victim;
        }
        return Result;
    }

    template <typename F> Eviction InsertNewKey(const KeyType& Key, F SlowGetPage) {
        Eviction Result{};
        A1inList_.push_front(Key);
        Entries_[Key] = TEntry{ETwoQLocation::A1in, A1inList_.begin(), SlowGetPage(Key)};

        if (A1inList_.size() > A1inCapacity_) {
            const KeyType EvictedKey = A1inList_.back();
            auto VictimIt = Entries_.find(EvictedKey);

            VictimIt->second.Location = ETwoQLocation::A1out;
            VictimIt->second.Page.reset(); // A1out хранит только историю, не данные
            A1outList_.splice(A1outList_.begin(), A1inList_, VictimIt->second.Position);
            
            if (A1outList_.size() > A1outCapacity_) {
                const KeyType GhostDrop = A1outList_.back();
                A1outList_.pop_back();
                Entries_.erase(GhostDrop);
            }

            Result.WasEvicted = true;
            Result.EvictedKey = EvictedKey;

        }
        return Result;
    }
};
