#pragma once

template <typename KeyType>
struct SCacheEviction {
    bool WasEvicted = false;
    KeyType EvictedKey{};
};