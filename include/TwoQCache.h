#pragma once

#include <algorithm>
#include <list>

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
class TTwoQCache : public TCacheLevelBase<KeyType, STwoQEntry<KeyType>> {
public:
    explicit TTwoQCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType, STwoQEntry<KeyType>>(ECacheAlgorithm::TwoQ, Capacity) {
        if (this->Capacity_ == 1) {
            // При ёмкости 1 честно разделить бюджет между A1in/A1out/Am
            // невозможно: Am вырождается в 0, и алгоритм начинает не
            // вытеснять резидента, а просто отказывать новым ключам в
            // приёме — это уже не та модель, с которой сравнивается
            // идеальный (беладиевский) кеш. Поэтому здесь честно
            // используется тривиальный однослотовый кеш с обязательной
            // заменой при промахе, как и полагается кешу ёмкости 1.
            bSingleSlotMode_ = true;
            return;
        }

        // Стандартные для 2Q пропорции: A1in ~ 1/4 общей ёмкости,
        // призрачная A1out ~ 1/2 общей ёмкости, остальное — под Am.
        A1inCapacity_ = std::max<std::size_t>(1, this->Capacity_ / 4);
        A1inCapacity_ = std::min(A1inCapacity_, this->Capacity_);
        A1outCapacity_ = std::max<std::size_t>(1, this->Capacity_ / 2);
        AmCapacity_ = (this->Capacity_ > A1inCapacity_) ? (this->Capacity_ - A1inCapacity_) : 0;
    }

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        if (bSingleSlotMode_) {
            return InsertSingleSlot(Key);
        }

        const auto FoundIt = this->Entries_.find(Key);

        if (FoundIt == this->Entries_.end()) {
            return InsertBrandNewKey(Key);
        }

        switch (FoundIt->second.Location) {
            case ETwoQLocation::Am:
                AmList_.splice(AmList_.begin(), AmList_, FoundIt->second.Position);
                return {};
            case ETwoQLocation::A1in:
                // По правилам 2Q повторное попадание, пока ключ ещё сидит в
                // "испытательной" FIFO-очереди, не переупорядочивает и не
                // повышает его — это осознанное поведение алгоритма.
                return {};
            case ETwoQLocation::A1out:
                return PromoteFromGhost(Key, FoundIt);
        }
        return {};
    }

private:
    using TBase    = TCacheLevelBase<KeyType, STwoQEntry<KeyType>>;
    using TResult  = typename TBase::TResult;
    using TEntryIt = typename TBase::TEntryIt;

    // Вырожденный режим ёмкости 1: в таблице всегда не больше одной записи,
    // поэтому унаследованный Contains() работает без единой правки.
    TResult InsertSingleSlot(const KeyType& Key) {
        TResult Result;
        if (this->Contains(Key)) {
            return Result; // touch, вытеснять некого
        }
        if (!this->Entries_.empty()) {
            Result.WasEvicted = true;
            Result.EvictedKey = this->Entries_.begin()->first;
            this->Entries_.clear();
        }
        this->Entries_[Key] = STwoQEntry<KeyType>{ETwoQLocation::Am, AmList_.end()};
        return Result;
    }

    TResult PromoteFromGhost(const KeyType& Key, TEntryIt FoundIt) {
        A1outList_.erase(FoundIt->second.Position);
        this->Entries_.erase(FoundIt);

        return InsertIntoMainArea(Key);
    }

    TResult InsertIntoMainArea(const KeyType& Key) {
        TResult Result;
        AmList_.push_front(Key);
        this->Entries_[Key] = STwoQEntry<KeyType>{ETwoQLocation::Am, AmList_.begin()};

        if (AmList_.size() > AmCapacity_) {
            const KeyType Victim = AmList_.back();
            AmList_.pop_back();
            this->Entries_.erase(Victim);
            Result.WasEvicted = true;
            Result.EvictedKey = Victim;
        }
        return Result;
    }

    TResult InsertBrandNewKey(const KeyType& Key) {
        TResult Result;
        A1inList_.push_front(Key);
        this->Entries_[Key] = STwoQEntry<KeyType>{ETwoQLocation::A1in, A1inList_.begin()};

        if (A1inList_.size() > A1inCapacity_) {
            const KeyType Overflow = A1inList_.back();
            A1inList_.pop_back();

            // Данные реально покидают резидентный кеш — это настоящее
            // вытеснение, которое должно каскадом уйти на следующий уровень.
            Result.WasEvicted = true;
            Result.EvictedKey = Overflow;

            A1outList_.push_front(Overflow);
            this->Entries_[Overflow] = STwoQEntry<KeyType>{ETwoQLocation::A1out, A1outList_.begin()};

            if (A1outList_.size() > A1outCapacity_) {
                const KeyType GhostDrop = A1outList_.back();
                A1outList_.pop_back();
                this->Entries_.erase(GhostDrop);
                // Чисто служебная запись-призрак, данных за ней нет — каскад не нужен.
            }
        }
        return Result;
    }

    std::size_t A1inCapacity_ = 0;
    std::size_t A1outCapacity_ = 0;
    std::size_t AmCapacity_ = 0;
    bool bSingleSlotMode_ = false;

    std::list<KeyType> A1inList_;
    std::list<KeyType> A1outList_;
    std::list<KeyType> AmList_;
};
