#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hub {
// Growth is serialized by the owning service's lock. Readers need no lock to
// locate a record: directories and records stay resident until process exit.
// Only registration allocates. Mutable record fields retain their own locking.
template<class T> class ResidentTable {
    struct Directory {
        size_t capacity;
        std::unique_ptr<std::atomic<T*>[]> entries;
        Directory* previous;
        Directory(size_t n, Directory* old) : capacity(n), entries(new std::atomic<T*>[n]{}), previous(old) {
            for (size_t i = 0; i < n; ++i) entries[i].store(nullptr, std::memory_order_relaxed);
            if (old) for (size_t i = 0; i < old->capacity; ++i)
                entries[i].store(old->entries[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    };
    std::atomic<Directory*> directory_{nullptr};
    std::atomic<size_t> size_{0};
    static_assert(std::atomic<Directory*>::is_always_lock_free && std::atomic<T*>::is_always_lock_free,
                  "fault-path lookup must be lock-free");
public:
    ResidentTable() = default;
    ResidentTable(const ResidentTable&) = delete;
    ResidentTable& operator=(const ResidentTable&) = delete;
    // Deliberately process-resident: CRT destruction may precede termination of
    // Core workers. Reclaiming these records then could invalidate native calls.
    ~ResidentTable() = default;
    size_t size() const noexcept { return size_.load(std::memory_order_acquire); }
    T* get(size_t index) const noexcept {
        auto d = directory_.load(std::memory_order_acquire);
        return d && index < d->capacity ? d->entries[index].load(std::memory_order_acquire) : nullptr;
    }
    T& operator[](size_t index) const noexcept { return *get(index); }
    // May leave an unused, zero-initialized prefix on allocation failure. No
    // published object is moved or discarded; callers publish their own state.
    T* ensure(size_t index) noexcept {
        if (index == (std::numeric_limits<size_t>::max)()) return nullptr;
        try {
            auto d = directory_.load(std::memory_order_relaxed);
            if (!d || index >= d->capacity) {
                size_t capacity = d ? d->capacity : 16; // allocation granularity, not a limit
                while (capacity <= index) {
                    if (capacity > (std::numeric_limits<size_t>::max)() / 2) { capacity = index + 1; break; }
                    capacity *= 2;
                }
                auto next = std::make_unique<Directory>(capacity, d);
                d = next.release();
                directory_.store(d, std::memory_order_release);
            }
            while (size_.load(std::memory_order_relaxed) <= index) {
                auto value = std::make_unique<T>();
                const auto n = size_.load(std::memory_order_relaxed);
                d->entries[n].store(value.release(), std::memory_order_release);
                size_.store(n + 1, std::memory_order_release);
            }
            return get(index);
        } catch (const std::bad_alloc&) { return nullptr; }
          catch (const std::length_error&) { return nullptr; }
    }
    struct Iterator {
        const ResidentTable* table;
        size_t index;
        T& operator*() const noexcept { return (*table)[index]; }
        Iterator& operator++() noexcept { ++index; return *this; }
        bool operator!=(const Iterator& other) const noexcept { return index != other.index; }
    };
    Iterator begin() const noexcept { return {this, 0}; }
    Iterator end() const noexcept { return {this, size()}; }
};

// Opaque serials do not reserve index bits. The owning service holds its lock
// for every operation. Forget only after the old record is closed and drained.
template<class T> class TokenIndex {
    using Map = std::unordered_map<uint64_t, T*>;
    Map* records_ = nullptr; // process-resident for the same reason as the table
    uint64_t sequence_ = 0;
public:
    uint64_t issue(T* record, uint64_t previous = 0) noexcept {
        if (sequence_ == UINT64_MAX) return 0;
        const auto token = ++sequence_;
        try {
            if (!records_) records_ = new Map;
            if (previous) {
                auto node = records_->extract(previous);
                if (!node.empty()) {
                    // Same map size/bucket capacity: renewing a drained record
                    // reuses its node, including per-frame temporary slot leases.
                    node.key() = token;
                    node.mapped() = record;
                    records_->insert(std::move(node));
                    return token;
                }
            }
            records_->emplace(token, record); return token;
        }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    void cancel(uint64_t token, uint64_t previous) noexcept {
        if (!records_) return;
        if (!previous) { records_->erase(token); return; }
        auto node = records_->extract(token);
        if (!node.empty()) { node.key() = previous; records_->insert(std::move(node)); }
    }
    T* find(uint64_t token) const noexcept {
        if (!records_) return nullptr;
        const auto found = records_->find(token);
        return found == records_->end() ? nullptr : found->second;
    }
    void forget(uint64_t token) noexcept { if (token && records_) records_->erase(token); }
};
}
