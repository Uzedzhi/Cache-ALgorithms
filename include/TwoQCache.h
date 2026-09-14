#pragma once

#include <algorithm>
#include <list>
#include <stdexcept>

#include "CacheLevel.h"

// В какой из трёх областей 2Q сейчас числится ключ.
// A1in/Am — резидентные, A1out — призрачная (только имя ключа).
enum class ETwoQLocation { A1in, A1out, Am };

template <typename KeyType>
struct STwoQEntry {
    ETwoQLocation Location = ETwoQLocation::A1in;
    typename std::list<KeyType>::iterator Position;

    bool IsResident() const {
        return Location == ETwoQLocation::A1in || Location == ETwoQLocation::Am;
    }
};

// 2Q (Johnson & Shasha, 1994) — три структуры:
//   A1in  — FIFO-очередь "новичков", ещё не доказавших повторную полезность;
//   A1out — призрачная FIFO-очередь ключей, вытесненных из A1in (без данных);
//   Am    — основная LRU-область для ключей, к которым уже было повторное
//           обращение (либо напрямую в Am, либо через ghost-hit в A1out).
// Идея: единственное обращение к странице не должно "засорять" основной
// LRU-кеш (в отличие от чистого LRU, где даже одноразовые сканы вытесняют
// полезные данные) — оно оседает в маленькой A1in, и только вторичное
// обращение поднимает ключ в Am.
template <typename KeyType>
class TTwoQCache : public TCacheLevelBase<KeyType> {
    using TResult  = SCacheEviction<KeyType>;
    using TMap     = std::unordered_map<KeyType, STwoQEntry<KeyType>>;
    TMap Entries_;

    std::size_t A1inCapacity_   = 0;
    std::size_t A1outCapacity_  = 0;
    std::size_t AmCapacity_     = 0;

    std::list<KeyType> A1inList_;
    std::list<KeyType> A1outList_;
    std::list<KeyType> AmList_;
public:
    explicit TTwoQCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType>(ECacheAlgorithm::TwoQ, Capacity) {
        CHECK_EX(Capacity >= 4, std::invalid_argument,  "Ошибка создания 2Q кеша: " 
                                                        "нельзя создать кеш с емкостью меньше 4");

        A1inCapacity_  = Capacity / 4;
        A1outCapacity_ = Capacity / 2;
        AmCapacity_    = Capacity - A1inCapacity_ - A1outCapacity_;
    }

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end() && FoundEl->second.Location != ETwoQLocation::A1out;
    }

    TResult Insert(const KeyType& Key) {
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
    TResult PromoteFromOutToAm(const KeyType& Key, typename TMap::iterator FoundIt) {
        A1outList_.erase(FoundIt->second.Position);
        Entries_.erase(FoundIt);

        return InsertIntoMainArea(Key);
    }

    TResult InsertIntoMainArea(const KeyType& Key) {
        TResult Result{};
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

    TResult InsertNewKey(const KeyType& Key) {
        TResult Result{};
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
