#pragma once

#include <algorithm>
#include <cassert>
#include <list>
#include <stdexcept>
#include <unordered_map>

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
template <typename KeyType>
struct SLirsEntry {
    bool IsLIR    = false;
    bool Resident = false;
    bool InStack  = false;
    bool InQueue  = false;

    typename std::list<KeyType>::iterator QueuePos;
    typename std::list<KeyType>::iterator StackPos;
};

// LIRS — использует межпосещенческую близость (IRR, Inter-Reference
// Recency) вместо простой хронологии обращений. Все замеченные ключи
// делятся на:
//   LIR (Low IRR) — "элитные", всегда резидентные, их LirsCapacity_ максимум;
//   HIR (High IRR) — обычные; резидентными могут быть лишь HirsCapacity_ штук,
//                    остальные HIR помнятся как "история" в стеке S.
// Структуры:
//   S — стек recency-истории (и LIR, и HIR, резидентные и нет). Голова списка
//       это вершина стека (самое свежее), хвост — дно (самое старое).
//       Инвариант: на дне всегда LIR-блок, что и обеспечивает подрезка.
//       Именно этот инвариант превращает проверку "ключ лежит в S"
//       в сравнение его IRR с порогом — числа IRR нигде не хранятся.
//   Q — очередь резидентных HIR-блоков. Голова — самый свежий,
//       хвост — жертва.
// Реализация — переложение алгоритма из статьи Jiang & Zhang,
// "LIRS: An Efficient Low Inter-reference Recency Set Replacement
// Policy" (SIGMETRICS 2002).
template <typename KeyType>
class TLirsCache : public TCacheLevelBase<KeyType> {
    using TResult = SCacheEviction<KeyType>;
    using TMap    = typename std::unordered_map<KeyType, SLirsEntry<KeyType>>;

    TMap Entries_;

    std::size_t LirsCapacity_ = 0;
    std::size_t HirsCapacity_ = 0;
    std::size_t LirCount_     = 0;

    std::list<KeyType> Stack_;
    std::list<KeyType> QueueList_;

public:
    explicit TLirsCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType>(ECacheAlgorithm::Lirs, Capacity) {
        CHECK_EX(Capacity >= 3, std::invalid_argument, "Ошибка создания LIRS кеша: "
                                                       "нельзя создать кеш с емкостью меньше 3");

        // В статье под HIR-резидентов отводится ~1% ёмкости, но не меньше
        // одного кадра: при HirsCapacity_ == 0 очередь вытесняла бы блок
        // в тот же момент, когда он туда попал, и испытательного срока
        // не существовало бы вовсе.
        HirsCapacity_ = std::max<std::size_t>(1, Capacity / 100);
        LirsCapacity_ = Capacity - HirsCapacity_;
    }

    bool Contains(const KeyType& Key) const {
        const auto FoundEl = Entries_.find(Key);
        return FoundEl != Entries_.end() && FoundEl->second.Resident;
    }

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        const auto FoundIt = Entries_.find(Key);
        const bool bKeyExists   = (FoundIt != Entries_.end());
        const bool bKeyResident = bKeyExists && FoundIt->second.Resident;

        TResult Result;

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
        } else {
            // Случай 4: ключ неизвестен вовсе (либо призрак уже подрезан).
            Result = InsertBrandNewKey(Key);
        }
        return Result;
    }

private:
    // --- операции над списками; только они трогают флаги и итераторы ---

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
        RemoveFromStack(It);
        PushToStackTop(It, Key);
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

    // Вытеснение хвоста очереди — единственное место во всём классе,
    // где освобождаются данные. Подрезка стека память не освобождает.
    TResult EvictQueueTail() {
        TResult Result;
        if (QueueList_.empty())
            return Result;

        const KeyType EvictedKey = QueueList_.back();
        const auto EvictedIt = Entries_.find(EvictedKey);

        QueueList_.pop_back();
        EvictedIt->second.InQueue  = false;
        EvictedIt->second.Resident = false;

        // Запись в S намеренно оставляем: она и есть призрак, по которому
        // будет опознано повторное обращение.
        if (!EvictedIt->second.InStack)
            Entries_.erase(EvictedIt);

        Result.WasEvicted = true;
        Result.EvictedKey = EvictedKey;
        return Result;
    }

    // Подрезка: снимаем дно, пока там не окажется LIR-блок. HIR-запись ниже
    // самого старого LIR уже никогда не пройдёт тест на малый IRR честно,
    // так что её хранение — источник ложных повышений, а не полезная история.
    void PruneStack() {
        while (!Stack_.empty()) {
            const KeyType BottomKey = Stack_.back();
            const auto It = Entries_.find(BottomKey);

            if (It->second.IsLIR)
                break;

            Stack_.pop_back();
            It->second.InStack = false;

            // Нерезидент вне стека и вне очереди — след простыл, забываем.
            if (!It->second.Resident && !It->second.InQueue)
                Entries_.erase(It);
        }
    }

    // --- переходы состояний ---

    // Дно стека по инварианту — самый холодный LIR-блок. Разжалуем его
    // в HIR: данные остаются, но теперь он кандидат на вылет.
    TResult DemoteStackBottom() {
        const KeyType VictimKey = Stack_.back();
        const auto VictimIt = Entries_.find(VictimKey);

        RemoveFromStack(VictimIt);
        VictimIt->second.IsLIR = false;
        --LirCount_;

        // В статье разжалованный блок встаёт в конец Q, то есть в позицию
        // максимальной защиты. У нас жертва берётся с хвоста, значит это голова.
        PushToQueueFront(VictimIt, VictimKey);

        TResult Result;
        if (QueueList_.size() > HirsCapacity_)
            Result = EvictQueueTail();
        return Result;
    }

    // Повышение до LIR. Вызывается и для резидентного HIR, найденного в S,
    // и для призрака — разница только в том, что призраку надо выдать кадр,
    // а кадр берётся из цепочки "демоция дна -> вытеснение хвоста Q".
    TResult PromoteToLir(const KeyType& Key) {
        const auto It = Entries_.find(Key);

        RemoveFromQueueIfPresent(It);
        MoveToStackTop(Key);

        It->second.IsLIR    = true;
        It->second.Resident = true;
        ++LirCount_;

        // Подрезать надо ДО выбора жертвы: MoveToStackTop мог снять блок
        // со дна, и без подрезки DemoteStackBottom разжаловал бы HIR-запись,
        // уронив LirCount_ ниже реального числа LIR-блоков.
        PruneStack();

        TResult Result;
        if (LirCount_ > LirsCapacity_)
            Result = DemoteStackBottom();

        PruneStack();
        return Result;
    }

    TResult InsertBrandNewKey(const KeyType& Key) {
        auto [It, bInserted] = Entries_.try_emplace(Key);
        It->second.Resident = true;

        TResult Result;
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