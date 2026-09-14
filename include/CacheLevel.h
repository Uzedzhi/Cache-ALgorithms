#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "CacheTypes.h"
#include "../MyCppLibs/sassert.h"

// Результат одной операции вставки/касания ключа на уровне кеша:
//   - WasEvicted == false — ключ помещён/обновлён без вытеснения;
//   - WasEvicted == true  — по правилам алгоритма пришлось вытеснить
//     EvictedKey; вызывающая сторона (TCacheHierarchy) каскадом опустит
//     его на следующий уровень.
template <typename KeyType>
struct SCacheEviction {
    bool WasEvicted = false;
    KeyType EvictedKey{};
};

// Общая база всех уровней кеша.
//
// Наблюдение, ради которого она существует: чем бы ни отличались LRU, LFU,
// ARC, 2Q, LIRS и идеальный кеш, устроены они одинаково — хеш-таблица
// "ключ -> метаданные", где запись бывает двух сортов:
//   - РЕЗИДЕНТНАЯ  — ключ реально лежит в кеше, обращение к нему = хит;
//   - ПРИЗРАЧНАЯ   — ключ только помнится как история, данных за ним нет
//                    (списки B1/B2 у ARC, A1out у 2Q, нерезидентный HIR
//                    у LIRS). Обращение к ней = промах, но алгоритм по
//                    этому факту подстраивается.
// Значит и Contains() у всех один и тот же: "запись нашлась И она
// резидентная". Отличается только КАК запись отвечает на вопрос
// "я резидентная?" — это единственное, что отдано наследникам: тип
// EntryType обязан иметь метод bool IsResident() const.
//
// Сюда же уехало всё остальное общее: ёмкость, тег алгоритма, проверка
// "ёмкость больше нуля" и пустой хук позиции в потоке. У наследников
// остаётся ровно одно — Insert(), то есть сама политика вытеснения.
//
// Наследование НЕвиртуальное и без vtable: конкретный тип уровня всегда
// известен статически (уровни лежат в std::variant, см. MultiLevelCache.h),
// поэтому вызовы разрешаются на этапе компиляции. Деструктор protected —
// удалять наследника через указатель на базу нельзя, и компилятор это
// подтвердит.
template <typename KeyType>
class TCacheLevelBase {
public:
    // bool Contains(const KeyType& Key) const {
    //     const auto FoundEl = Entries_.find(Key);
    //     return FoundEl != Entries_.end() && FoundEl->second.IsResident();
    // }

    std::size_t GetCapacity()      const { return Capacity_; }
    ECacheAlgorithm GetAlgorithm() const { return Algorithm_; }

protected:
    // using TEntryMap = std::unordered_map<KeyType, EntryType>;
    // using TEntryIt  = typename TEntryMap::iterator;
    // using TResult   = SCacheEviction<KeyType>;

    // TEntryMap &GetEntries() { return Entries_; }
    TCacheLevelBase(ECacheAlgorithm Algorithm, std::size_t Capacity)
        : Algorithm_(Algorithm), Capacity_(Capacity) {
        CHECK_EX(Capacity_ != 0, std::invalid_argument,
                 std::string(CacheAlgorithmToString(Algorithm)) +
                 ": размер кеша должен быть больше нуля.");
    }

    ~TCacheLevelBase() = default;

    ECacheAlgorithm Algorithm_;
    std::size_t Capacity_;
    // TEntryMap Entries_;
};
