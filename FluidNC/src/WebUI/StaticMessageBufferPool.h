#pragma once

#include <array>
#include <atomic>
#include <cstddef>

// Fixed storage for messages whose bytes are consumed asynchronously.  A
// lease remains reserved after it is transferred to an output queue and is
// released only by that queue's completion callback.
template <size_t BufferBytes, size_t SlotCount>
class StaticMessageBufferPool {
    static_assert(BufferBytes > 0, "message buffers must not be empty");
    static_assert(SlotCount > 0, "message buffer pools need at least one slot");

    struct Slot {
        std::array<char, BufferBytes> bytes {};
        std::atomic<bool>             reserved { false };
    };

public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : _slot(other._slot) { other._slot = nullptr; }

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                _slot       = other._slot;
                other._slot = nullptr;
            }
            return *this;
        }

        ~Lease() { release(); }

        explicit operator bool() const { return _slot != nullptr; }
        char* data() const { return _slot == nullptr ? nullptr : _slot->bytes.data(); }
        constexpr size_t capacity() const { return BufferBytes; }

        // Transfer responsibility for releasing this slot to an asynchronous
        // completion callback.
        void* transfer() {
            Slot* slot = _slot;
            _slot      = nullptr;
            return slot;
        }

    private:
        friend class StaticMessageBufferPool;

        explicit Lease(Slot* slot) : _slot(slot) {}

        void release() {
            if (_slot != nullptr) {
                StaticMessageBufferPool::release(_slot);
                _slot = nullptr;
            }
        }

        Slot* _slot = nullptr;
    };

    Lease tryAcquire() {
        for (auto& slot : _slots) {
            bool expected = false;
            if (slot.reserved.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return Lease(&slot);
            }
        }
        return Lease();
    }

    static void release(void* context) {
        if (context != nullptr) {
            static_cast<Slot*>(context)->reserved.store(false, std::memory_order_release);
        }
    }

private:
    std::array<Slot, SlotCount> _slots {};
};
