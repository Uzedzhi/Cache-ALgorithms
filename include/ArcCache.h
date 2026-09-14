#pragma once

#include <algorithm>
#include <list>

#include "CacheLevel.h"

// В каком из четырёх списков ARC сейчас числится ключ.
// T1/T2 — резидентные, B1/B2 — призрачные (только имя ключа, данных нет).
enum class EArcLocation { T1, T2, B1, B2 };

template <typename KeyType>
struct SArcEntry {
    EArcLocation Location = EArcLocation::T1;
    typename std::list<KeyType>::iterator Position;

    bool IsResident() const {
        return Location == EArcLocation::T1 || Location == EArcLocation::T2;
    }
};

// ARC — адаптивный кеш с четырьмя списками:
//   T1 — резидентные ключи с одним недавним обращением ("recency"),
//   T2 — резидентные ключи с повторными обращениями ("frequency"),
//   B1 — "призрачный" список недавно вытесненных из T1 (только ключи),
//   B2 — "призрачный" список недавно вытесненных из T2 (только ключи).
// Параметр p адаптивно балансирует целевой размер T1 в зависимости от
// того, какой из призрачных списков "стреляет" чаще — это и есть
// самонастройка ARC. Реализация — прямое переложение псевдокода из
// оригинальной статьи Megiddo & Modha, "ARC: A Self-Tuning, Low
// Overhead Replacement Cache" (FAST 2003).
template <typename KeyType>
class TArcCache : public TCacheLevelBase<KeyType> {
public:
    explicit TArcCache(std::size_t Capacity)
        : TCacheLevelBase<KeyType>(ECacheAlgorithm::Arc, Capacity) {}

    SCacheEviction<KeyType> Insert(const KeyType& Key) {
        const auto FoundIt = this->Entries_.find(Key);

        if (FoundIt == this->Entries_.end()) {
            return InsertNewKey(Key);
        }

        switch (FoundIt->second.Location) {
            case EArcLocation::T1:
            case EArcLocation::T2:
                PromoteToT2Front(Key, FoundIt);
                return {};
            case EArcLocation::B1:
                return HandleGhostHit(Key, FoundIt, /*bWasInB2=*/false);
            case EArcLocation::B2:
                return HandleGhostHit(Key, FoundIt, /*bWasInB2=*/true);
        }
        return {};
    }

private:
    using TBase    = TCacheLevelBase<KeyType>;
    using TResult  = SCacheEviction<KeyType>;
    using TEntryIt = std::unordered_map<KeyType, EntryType>::iterator;

    void PromoteToT2Front(const KeyType& Key, TEntryIt FoundIt) {
        if (FoundIt->second.Location == EArcLocation::T1) {
            T1_.erase(FoundIt->second.Position);
        } else {
            T2_.erase(FoundIt->second.Position);
        }
        T2_.push_front(Key);
        FoundIt->second.Location = EArcLocation::T2;
        FoundIt->second.Position = T2_.begin();
    }

    TResult HandleGhostHit(const KeyType& Key, TEntryIt FoundIt, bool bWasInB2) {
        if (!bWasInB2) {
            const double Delta = std::max(1.0, static_cast<double>(B2_.size()) / static_cast<double>(B1_.size()));
            TargetSizeT1_ = std::min(static_cast<double>(this->Capacity_), TargetSizeT1_ + Delta);
        } else {
            const double Delta = std::max(1.0, static_cast<double>(B1_.size()) / static_cast<double>(B2_.size()));
            TargetSizeT1_ = std::max(0.0, TargetSizeT1_ - Delta);
        }

        TResult Result = Replace(bWasInB2);

        if (bWasInB2) {
            B2_.erase(FoundIt->second.Position);
        } else {
            B1_.erase(FoundIt->second.Position);
        }
        this->Entries_.erase(FoundIt);

        T2_.push_front(Key);
        this->Entries_[Key] = SArcEntry<KeyType>{EArcLocation::T2, T2_.begin()};
        return Result;
    }

    TResult InsertNewKey(const KeyType& Key) {
        TResult Result;
        const std::size_t T1Size = T1_.size();
        const std::size_t B1Size = B1_.size();
        const std::size_t T2Size = T2_.size();
        const std::size_t B2Size = B2_.size();

        if (T1Size + B1Size == this->Capacity_) {
            if (T1Size < this->Capacity_) {
                const KeyType GhostVictim = B1_.back();
                B1_.pop_back();
                this->Entries_.erase(GhostVictim);
                Result = Replace(false);
            } else {
                const KeyType Victim = T1_.back();
                T1_.pop_back();
                this->Entries_.erase(Victim);
                Result.WasEvicted = true;
                Result.EvictedKey = Victim;
            }
        } else if (T1Size + B1Size < this->Capacity_) {
            const std::size_t Total = T1Size + T2Size + B1Size + B2Size;
            if (Total >= this->Capacity_) {
                if (Total == 2 * this->Capacity_) {
                    const KeyType GhostVictim = B2_.back();
                    B2_.pop_back();
                    this->Entries_.erase(GhostVictim);
                }
                Result = Replace(false);
            }
        }

        T1_.push_front(Key);
        this->Entries_[Key] = SArcEntry<KeyType>{EArcLocation::T1, T1_.begin()};
        return Result;
    }

    // bKeyCameFromB2 — участвовал ли в вызове ключ, только что найденный в B2
    // (влияет на условие выбора жертвы между T1 и T2, см. статью).
    TResult Replace(bool bKeyCameFromB2) {
        TResult Result;
        const bool bEvictFromT1 =
            !T1_.empty() &&
            ((bKeyCameFromB2 && T1_.size() == static_cast<std::size_t>(TargetSizeT1_)) ||
             (T1_.size() > static_cast<std::size_t>(TargetSizeT1_)));

        if (bEvictFromT1) {
            const KeyType Victim = T1_.back();
            T1_.pop_back();
            B1_.push_front(Victim);
            this->Entries_[Victim] = SArcEntry<KeyType>{EArcLocation::B1, B1_.begin()};
            Result.WasEvicted = true;
            Result.EvictedKey = Victim;
        } else if (!T2_.empty()) {
            const KeyType Victim = T2_.back();
            T2_.pop_back();
            B2_.push_front(Victim);
            this->Entries_[Victim] = SArcEntry<KeyType>{EArcLocation::B2, B2_.begin()};
            Result.WasEvicted = true;
            Result.EvictedKey = Victim;
        }
        // Если оба списка пусты (вырожденный случай малой ёмкости) — вытеснять
        // нечего, возвращаем "без вытеснения" вместо падения по assert.
        return Result;
    }

    double TargetSizeT1_ = 0.0;
    std::list<KeyType> T1_;
    std::list<KeyType> T2_;
    std::list<KeyType> B1_;
    std::list<KeyType> B2_;
};
