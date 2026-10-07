#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace layoutfeed {
    // Open-addressing hash map keyed by pointer, for the per-sprite role
    // cache that is queried thousands of times per frame. Linear probing in a
    // flat array (one cache line per probe instead of a heap node per entry),
    // backward-shift deletion (no tombstones), load factor at most 1/2.
    template <class Value>
    class PointerMap final {
    public:
        PointerMap() { rehash(1024); }

        Value* find(void const* key) {
            if (!key) return nullptr;
            for (auto index = home(key);; index = (index + 1) & m_mask) {
                auto& slot = m_slots[index];
                if (slot.key == key) return &slot.value;
                if (!slot.key) return nullptr;
            }
        }

        enum class Assigned { Unchanged, Inserted, Changed };

        template <class Equal>
        Assigned assign(void const* key, Value const& value, Equal const& equal) {
            if (!key) return Assigned::Unchanged;
            if ((m_size + 1) * 2 > m_slots.size()) rehash(m_slots.size() * 2);
            for (auto index = home(key);; index = (index + 1) & m_mask) {
                auto& slot = m_slots[index];
                if (slot.key == key) {
                    if (equal(slot.value, value)) return Assigned::Unchanged;
                    slot.value = value;
                    return Assigned::Changed;
                }
                if (!slot.key) {
                    slot.key = key;
                    slot.value = value;
                    ++m_size;
                    return Assigned::Inserted;
                }
            }
        }

        void erase(void const* key) {
            if (!key) return;
            auto index = home(key);
            while (m_slots[index].key != key) {
                if (!m_slots[index].key) return;
                index = (index + 1) & m_mask;
            }
            // Backward-shift: pull later entries of the probe run into the
            // hole whenever their home slot does not lie between hole and them.
            auto hole = index;
            for (auto next = (hole + 1) & m_mask; m_slots[next].key; next = (next + 1) & m_mask) {
                auto const want = home(m_slots[next].key);
                auto const between = hole <= next ? (hole < want && want <= next) : (hole < want || want <= next);
                if (!between) {
                    m_slots[hole] = m_slots[next];
                    hole = next;
                }
            }
            m_slots[hole] = Slot{};
            --m_size;
        }

        void clear() {
            m_slots.assign(m_slots.size(), Slot{});
            m_size = 0;
        }

        void reserve(std::size_t count) {
            auto capacity = m_slots.size();
            while (capacity < count * 2) capacity *= 2;
            if (capacity != m_slots.size()) rehash(capacity);
        }

        std::size_t size() const { return m_size; }

    private:
        struct Slot {
            void const* key = nullptr;
            Value value{};
        };

        std::size_t home(void const* key) const {
            // Fibonacci hashing of the pointer without its alignment bits.
            auto const bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key)) >> 4;
            return static_cast<std::size_t>((bits * 0x9E3779B97F4A7C15ull) >> m_shift);
        }

        void rehash(std::size_t capacity) {
            std::vector<Slot> old;
            old.swap(m_slots);
            m_slots.assign(capacity, Slot{});
            m_mask = capacity - 1;
            m_shift = 64;
            for (auto size = capacity; size > 1; size >>= 1) --m_shift;
            m_size = 0;
            for (auto const& slot : old) {
                if (!slot.key) continue;
                auto index = home(slot.key);
                while (m_slots[index].key) index = (index + 1) & m_mask;
                m_slots[index] = slot;
                ++m_size;
            }
        }

        std::vector<Slot> m_slots;
        std::size_t m_size = 0;
        std::size_t m_mask = 0;
        unsigned m_shift = 64;
    };
}
