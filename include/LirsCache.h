#pragma once

#include <algorithm>
#include <list>
#include <unordered_map>

#include "CacheLevel.h"

// Запись LIRS. В отличие от ARC/2Q "призрачность" тут не позиция в списке,
// а отдельный флаг: нерезидентный HIR-ключ продолжает жить в стеке S как
// история, но данных за ним уже нет.
template <typename KeyType>
struct SLirsEntry {
    bool IsLIR = false;
    bool Resident = false;

    bool IsResident() const { return Resident; }
};

// LIRS — использует межпосещенческую близость (IRR, Inter-Reference
// Recency) вместо простой хронологии обращений. Все замеченные ключи
// делятся на:
//   LIR (Low IRR) — "элитные", всегда резидентные, их L штук максимум;
//   HIR (High IRR) — обычные; резидентными могут быть лишь Hirs штук,
//                    остальные HIR помнятся как "история" в стеке S.
// Структуры:
//   S — стек recency-истории (и LIR, и HIR, резидентных и нет); особый
//       инвариант — после каждой операции низ стека "подрезается" так,
//       чтобы там всегда лежал LIR-блок (стековая подрезка).
//   Q — очередь резидентных HIR-блоков, используется для выбора жертвы.
// Реализация — переложение алгоритма из статьи Jiang & Zhang,
// "LIRS: An Efficient Low Inter-reference Recency Set Replacement
// Policy" (SIGMETRICS 2002).
template <typename KeyType>
class TLirsCache : public TCacheLevelBase<KeyType, SLirsEntry<KeyType>> {
public:
    explicit TLirsCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType, SLirsEntry<KeyType>>(ECacheAlgorithm::Lirs, Capacity) {
        if (this->Capacity_ >= 2) {
            HirsCapacity_ = std::max<std::size_t>(1, this->Capacity_ / 10);
            HirsCapacity_ = std::min(HirsCapacity_, this->Capacity_ - 1);
            LirsCapacity_ = this->Capacity_ - HirsCapacity_;
        } else {
            // Вырожденный случай ёмкости 1: честно поделить бюджет между
            // LIR- и HIR-резидентной областью невозможно — одна из них
            // неизбежно станет нулевой, и алгоритм вместо вытеснения
            // резидента начнёт просто отказывать новым ключам в приёме.
            // Это меняет саму модель кеша (сравнение с "идеальным"
            // Белади предполагает обязательную замену на промахе), поэтому
            // ёмкость 1 явно обрабатывается как тривиальный однослотовый
            // кеш с обязательной заменой — см. bSingleSlotMode_.
            bSingleSlotMode_ = true;
            HirsCapacity_ = 0;
            LirsCapacity_ = 0;
        }
    }

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        if (bSingleSlotMode_) {
            return InsertSingleSlot(Key);
        }

        auto InfoIt = this->Entries_.find(Key);
        const bool bExists = (InfoIt != this->Entries_.end());
        const bool bResident = bExists && InfoIt->second.Resident;

        if (bResident && InfoIt->second.IsLIR) {
            MoveToStackTop(Key);
            PruneStack();
            return {};
        }

        if (bResident && !InfoIt->second.IsLIR) {
            const bool bInStack = StackIterators_.find(Key) != StackIterators_.end();
            if (bInStack) {
                return PromoteHirToLir(Key);
            }
            RemoveFromQueueIfPresent(Key);
            QueueList_.push_front(Key);
            QueueIterators_[Key] = QueueList_.begin();
            MoveToStackTop(Key);
            PruneStack();
            return {};
        }

        const bool bHasHistory = bExists && StackIterators_.find(Key) != StackIterators_.end();
        if (bHasHistory) {
            return PromoteHirToLir(Key);
        }
        return InsertBrandNewKey(Key);
    }

private:
    using TBase   = TCacheLevelBase<KeyType, SLirsEntry<KeyType>>;
    using TResult = typename TBase::TResult;

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
        this->Entries_[Key] = SLirsEntry<KeyType>{/*IsLIR=*/true, /*Resident=*/true};
        return Result;
    }

    void MoveToStackTop(const KeyType& Key) {
        const auto It = StackIterators_.find(Key);
        if (It != StackIterators_.end()) {
            Stack_.erase(It->second);
        }
        Stack_.push_front(Key);
        StackIterators_[Key] = Stack_.begin();
    }

    void RemoveFromQueueIfPresent(const KeyType& Key) {
        const auto It = QueueIterators_.find(Key);
        if (It != QueueIterators_.end()) {
            QueueList_.erase(It->second);
            QueueIterators_.erase(It);
        }
    }

    // Поддерживает инвариант "низ стека — всегда LIR-блок (или стек пуст)".
    void PruneStack() {
        while (!Stack_.empty()) {
            const KeyType BackKey = Stack_.back();
            const auto It = this->Entries_.find(BackKey);
            const bool bBottomIsLir = (It != this->Entries_.end()) && It->second.IsLIR;
            if (bBottomIsLir) {
                break;
            }

            StackIterators_.erase(BackKey);
            Stack_.pop_back();

            if (It != this->Entries_.end() && !It->second.Resident) {
                // Нерезидентный HIR без истории в стеке — забываем совсем,
                // дальше он неотличим от "никогда не виденного" ключа.
                this->Entries_.erase(It);
            }
        }
    }

    TResult DemoteStackBottomLirToHir() {
        TResult Result;
        if (Stack_.empty()) {
            return Result;
        }

        const KeyType VictimKey = Stack_.back();
        StackIterators_.erase(VictimKey);
        Stack_.pop_back();

        this->Entries_[VictimKey] = SLirsEntry<KeyType>{/*IsLIR=*/false, /*Resident=*/true};
        --LirCount_;

        QueueList_.push_front(VictimKey);
        QueueIterators_[VictimKey] = QueueList_.begin();

        if (QueueList_.size() > HirsCapacity_) {
            const KeyType Overflow = QueueList_.back();
            QueueList_.pop_back();
            QueueIterators_.erase(Overflow);
            this->Entries_[Overflow].Resident = false;
            Result.WasEvicted = true;
            Result.EvictedKey = Overflow;
        }
        return Result;
    }

    TResult PromoteHirToLir(const KeyType& Key) {
        RemoveFromQueueIfPresent(Key);

        this->Entries_[Key] = SLirsEntry<KeyType>{/*IsLIR=*/true, /*Resident=*/true};
        ++LirCount_;

        MoveToStackTop(Key);

        TResult Result;
        if (LirCount_ > LirsCapacity_) {
            Result = DemoteStackBottomLirToHir();
        }
        PruneStack();
        return Result;
    }

    TResult InsertBrandNewKey(const KeyType& Key) {
        TResult Result;
        if (LirCount_ < LirsCapacity_) {
            this->Entries_[Key] = SLirsEntry<KeyType>{/*IsLIR=*/true, /*Resident=*/true};
            ++LirCount_;
            MoveToStackTop(Key);
        } else {
            this->Entries_[Key] = SLirsEntry<KeyType>{/*IsLIR=*/false, /*Resident=*/true};
            MoveToStackTop(Key);
            QueueList_.push_front(Key);
            QueueIterators_[Key] = QueueList_.begin();

            if (QueueList_.size() > HirsCapacity_) {
                const KeyType Overflow = QueueList_.back();
                QueueList_.pop_back();
                QueueIterators_.erase(Overflow);
                this->Entries_[Overflow].Resident = false;
                Result.WasEvicted = true;
                Result.EvictedKey = Overflow;
            }
        }
        PruneStack();
        return Result;
    }

    std::size_t LirsCapacity_ = 0;
    std::size_t HirsCapacity_ = 0;
    std::size_t LirCount_ = 0;
    bool bSingleSlotMode_ = false;

    std::list<KeyType> Stack_;
    std::unordered_map<KeyType, typename std::list<KeyType>::iterator> StackIterators_;

    std::list<KeyType> QueueList_;
    std::unordered_map<KeyType, typename std::list<KeyType>::iterator> QueueIterators_;
};