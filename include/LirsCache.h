#pragma once

#include <algorithm>
#include <cassert>
#include <list>
#include <optional>
#include <unordered_map>

#include "../MyCppLibs/sassert.h"
#include "CacheLevel.h"

// Запись LIRS. "Призрачность" тут не позиция в списке, а флаг Resident:
// нерезидентный HIR-ключ продолжает жить в стеке S как история обращений,
// но данных за ним уже нет.
//
// Инвариант, который обязаны поддерживать все методы: InStack/InQueue
// строго соответствуют фактическому членству ключа в Stack_/QueueList_,
// а StackPos/QueuePos валидны ровно тогда, когда соответствующий флаг true.
// Поэтому поля здесь нельзя перезаписывать агрегатной инициализацией —
// это молча сбросит флаги, оставив узлы в списках.
template <typename KeyType, typename PageType>
struct SLirsEntry {
    bool IsLIR    = false;
    bool Resident = false;
    bool InStack  = false;
    bool InQueue  = false;

    typename std::list<KeyType>::iterator QueuePos;
    typename std::list<KeyType>::iterator StackPos;

    std::optional<PageType> Page = std::nullopt; 
};

template <typename KeyType, typename PageType>
class TLirsCache {
    using Eviction = SCacheEviction<KeyType>;
    using TMap     = typename std::unordered_map<KeyType, SLirsEntry<KeyType, PageType>>;

    TMap Entries_;
    std::size_t LirsCapacity_ = 0;
    std::size_t HirsCapacity_ = 0;
    std::size_t LirCount_     = 0;

    std::list<KeyType> Stack_;
    std::list<KeyType> QueueList_;
    std::size_t Capacity_;

public:
    explicit TLirsCache(std::size_t Capacity) : Capacity_(Capacity) {
        CHECK_EX(Capacity >= 2, std::invalid_argument, "Ошибка создания LIRS кеша: "
                                                       "нельзя создать кеш с емкостью меньше 2");

        HirsCapacity_ = std::max<std::size_t>(1, Capacity / 100);
        LirsCapacity_ = Capacity - HirsCapacity_;
    }

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end() && FoundEl->second.Resident;
    }

    template <typename F> Eviction Insert(const KeyType& Key, F SlowGetPage) {
        const auto FoundIt      = Entries_.find(Key);
        const bool bKeyExists   = (FoundIt != Entries_.end());
        const bool bKeyResident = bKeyExists && FoundIt->second.Resident;

        Eviction Result;
        if (bKeyResident && FoundIt->second.IsLIR) {
            // Случай 1: попадание в LIR. Двигаем наверх; если блок был на дне,
            // подрезка снимет оголившийся хвост из HIR-записей.
            MoveToStackTop(Key);
            PruneStack();
        } else if (bKeyResident) {
            // Случай 2: попадание в резидентный HIR. Читаем InStack ДО переноса
            // наверх — после MoveToStackTop флаг всегда true и сигнал потерян.
            if (FoundIt->second.InStack) {
                Result = PromoteToLir(Key);
            } else {
                MoveToStackTop(Key);
                TouchQueue(Key);
                PruneStack();
            }
        } else if (bKeyExists && FoundIt->second.InStack) {
            // Случай 3: промах по нерезиденту, но призрак в S уцелел —
            // значит IRR мал, блок минует очередь и сразу становится LIR.
            Result = PromoteToLir(Key);
            FoundIt->second.Page = SlowGetPage(Key);
        } else {
            // Случай 4: ключ неизвестен вовсе (либо призрак уже подрезан).
            // FoundIt здесь == end(), а вставка могла сделать рехеш —
            // берём итератор заново.
            Result = InsertBrandNewKey(Key);
            Entries_.find(Key)->second.Page = SlowGetPage(Key);
        }
        return Result;
    }

private:
    void PushToStackTop(typename TMap::iterator It, const KeyType& Key) {
        Stack_.push_front(Key);
        It->second.StackPos = Stack_.begin();
        It->second.InStack  = true;
    }

    void RemoveFromStack(typename TMap::iterator It) {
        if (!It->second.InStack)
            return;
        Stack_.erase(It->second.StackPos);
        It->second.InStack = false;
    }

    void MoveToStackTop(const KeyType& Key) {
        const auto It = Entries_.find(Key);
        if (!It->second.InStack) {
            PushToStackTop(It, Key);
            return;
        }
        Stack_.splice(Stack_.begin(), Stack_, It->second.StackPos);
    }

    void PushToQueueFront(typename TMap::iterator It, const KeyType& Key) {
        QueueList_.push_front(Key);
        It->second.QueuePos = QueueList_.begin();
        It->second.InQueue  = true;
    }

    void RemoveFromQueueIfPresent(typename TMap::iterator It) {
        if (!It->second.InQueue)
            return;
        QueueList_.erase(It->second.QueuePos);
        It->second.InQueue = false;
    }

    void TouchQueue(const KeyType& Key) {
        const auto It = Entries_.find(Key);
        RemoveFromQueueIfPresent(It);
        PushToQueueFront(It, Key);
    }

    Eviction EvictQueueTail() {
        Eviction Result;
        if (QueueList_.empty())
            return Result;

        const KeyType EvictedKey = QueueList_.back();
        const auto EvictedIt = Entries_.find(EvictedKey);

        QueueList_.pop_back();
        EvictedIt->second.InQueue  = false;
        EvictedIt->second.Resident = false;
        EvictedIt->second.Page.reset();

        if (!EvictedIt->second.InStack)
            Entries_.erase(EvictedIt);

        Result.WasEvicted = true;
        Result.EvictedKey = EvictedKey;
        return Result;
    }

    void PruneStack() {
        while (!Stack_.empty()) {
            const KeyType BottomKey = Stack_.back();
            const auto It = Entries_.find(BottomKey);

            if (It->second.IsLIR)
                break;

            Stack_.pop_back();
            It->second.InStack = false;

            if (!It->second.Resident && !It->second.InQueue)
                Entries_.erase(It);
        }
    }

    Eviction DemoteStackBottom() {
        const KeyType VictimKey = Stack_.back();
        const auto VictimIt = Entries_.find(VictimKey);

        RemoveFromStack(VictimIt);
        VictimIt->second.IsLIR = false;
        --LirCount_;

        PushToQueueFront(VictimIt, VictimKey);

        Eviction Result;
        if (QueueList_.size() > HirsCapacity_)
            Result = EvictQueueTail();
        return Result;
    }

    Eviction PromoteToLir(const KeyType& Key) {
        const auto It = Entries_.find(Key);

        RemoveFromQueueIfPresent(It);
        MoveToStackTop(Key);

        It->second.IsLIR    = true;
        It->second.Resident = true;
        ++LirCount_;

        PruneStack();

        Eviction Result;
        if (LirCount_ > LirsCapacity_)
            Result = DemoteStackBottom();

        PruneStack();
        return Result;
    }

    Eviction InsertBrandNewKey(const KeyType& Key) {
        auto [It, bInserted] = Entries_.try_emplace(Key);
        It->second.Resident = true;

        Eviction Result;
        if (LirCount_ < LirsCapacity_) {
            // Прогрев: пока LIR-множество не заполнено, новые блоки попадают
            // туда сразу — конкурировать всё равно не с кем.
            It->second.IsLIR = true;
            ++LirCount_;
            PushToStackTop(It, Key);
        } else {
            PushToStackTop(It, Key);
            PushToQueueFront(It, Key);
            if (QueueList_.size() > HirsCapacity_)
                Result = EvictQueueTail();
        }

        PruneStack();
        return Result;
    }
};