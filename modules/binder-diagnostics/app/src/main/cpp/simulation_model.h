#ifndef NATIVE_SIMULATION_MODEL_H
#define NATIVE_SIMULATION_MODEL_H

#include <cstdint>
#include <limits>
#include <mutex>

namespace simulation {

// Synthetic addresses describe this toy model, never host or kernel memory.
inline constexpr std::uint64_t kProcAddress = UINT64_C(0x10000000);
inline constexpr std::uint64_t kExpectedOffset = 64;
inline constexpr std::uint64_t kSearchRange = 256;
inline constexpr std::uint64_t kAddressAlignment = 8;
inline constexpr std::uint64_t kInitialLockState = UINT64_C(0xDEADBEEFCAFE0001);
inline constexpr std::uint64_t kInitialBasePtr = UINT64_C(0xAAAAAAAA0000);
inline constexpr std::uint64_t kActiveFlag = 0x01;
inline constexpr std::uint64_t kAppliedFlag = 0x02;
inline constexpr std::uint64_t kHealthPassedFlag = 0x04;

struct State {
    std::uint64_t innerLockState = kInitialLockState;
    std::uint64_t basePtr = kInitialBasePtr;
    std::uint64_t transactionCount = 0;
    std::uint64_t flags = kActiveFlag;
    std::int32_t referenceCount = 1;
};

class Model {
public:
    // A target is a number in the synthetic search window, not a pointer.
    static constexpr bool isValidTarget(std::uint64_t target) noexcept {
        return target != 0 && target % kAddressAlignment == 0 &&
               target >= kProcAddress - kSearchRange &&
               target <= kProcAddress + kSearchRange;
    }

    bool execute(std::uint64_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isValidTarget(target) ||
            state_.transactionCount == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        state_.basePtr = target;
        state_.innerLockState = lockStateFor(target);
        ++state_.transactionCount;
        // A new transaction invalidates the previous health result.
        state_.flags = kActiveFlag | kAppliedFlag;
        return true;
    }

    bool checkHealth() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool healthy = isHealthy(state_);
        if (healthy) {
            state_.flags |= kHealthPassedFlag;
        } else {
            state_.flags &= ~kHealthPassedFlag;
        }
        return healthy;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State{};
    }

    State snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    static constexpr std::uint64_t lockStateFor(std::uint64_t target) noexcept {
        return (kInitialLockState & UINT64_C(0xFFFFFFFF00000000)) |
               (target & UINT64_C(0xFFFFFFFF));
    }

    // Only the deliberately documented +64 offset completes the simulation.
    // Exact consistency checks keep changed-but-invalid state from passing.
    static constexpr bool isHealthy(const State& state) noexcept {
        constexpr std::uint64_t requiredFlags = kActiveFlag | kAppliedFlag;
        return state.basePtr == kProcAddress + kExpectedOffset &&
               state.innerLockState == lockStateFor(state.basePtr) &&
               state.transactionCount > 0 && state.referenceCount == 1 &&
               (state.flags & ~kHealthPassedFlag) == requiredFlags;
    }

private:
    mutable std::mutex mutex_;
    State state_;
};

}  // namespace simulation

#endif
