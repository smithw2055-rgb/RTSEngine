#pragma once

#include <RTSEngine/TowerDefense/WaveDirector.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

using WaveSequenceId = std::uint32_t;

struct WaveSequenceDefinition final {
    WaveSequenceId id{};
    std::vector<WaveId> waves;
    std::uint32_t initialPreparationTicks{};
    std::uint32_t interWavePreparationTicks{30};
    bool allowEarlyStart{true};

    friend bool operator==(const WaveSequenceDefinition& a,
                           const WaveSequenceDefinition& b) noexcept {
        return a.id == b.id && a.waves == b.waves &&
               a.initialPreparationTicks == b.initialPreparationTicks &&
               a.interWavePreparationTicks == b.interWavePreparationTicks &&
               a.allowEarlyStart == b.allowEarlyStart;
    }
};

enum class WaveSequencePhase : std::uint8_t {
    Idle,
    Preparing,
    StartingWave,
    WaveActive,
    RewardPending,
    Complete,
    Failed
};

enum class WaveSequenceFailure : std::uint8_t {
    None,
    UnknownSequence,
    AlreadyActive,
    InvalidDefinition,
    UnknownWave,
    NotPreparing,
    EarlyStartDisabled,
    StaleSequenceCommand,
    RewardNotPending,
    TowerCommandRejected,
    WaveRejected,
    BaseCoreDestroyed,
    TickOverflow
};

struct WaveSequenceState final {
    WaveSequenceId sequenceId{};
    WaveSequencePhase phase{WaveSequencePhase::Idle};
    std::uint32_t waveIndex{};
    std::uint32_t completedWaves{};
    WaveId currentWave{};
    std::uint64_t preparationStartedTick{};
    std::uint64_t scheduledStartTick{};
};

struct WaveSequenceResult final {
    bool accepted{};
    WaveSequenceFailure failure{WaveSequenceFailure::None};
    WaveSequenceId sequenceId{};
    WaveId waveId{};
};

class WaveSequenceDirector final {
public:
    bool registerSequence(WaveSequenceDefinition definition) {
        if (!validDefinition(definition)) return false;
        replaceById(definitions_, std::move(definition));
        return true;
    }

    const WaveSequenceDefinition* definition(
        WaveSequenceId id) const noexcept {
        return findById(definitions_, id);
    }

    const std::vector<WaveSequenceDefinition>& definitions() const noexcept {
        return definitions_;
    }

    const WaveSequenceState& state() const noexcept { return state_; }

    bool active() const noexcept {
        return state_.phase == WaveSequencePhase::Preparing ||
               state_.phase == WaveSequencePhase::StartingWave ||
               state_.phase == WaveSequencePhase::WaveActive ||
               state_.phase == WaveSequencePhase::RewardPending;
    }

    bool due(std::uint64_t tick) const noexcept {
        return state_.phase == WaveSequencePhase::Preparing &&
               tick >= state_.scheduledStartTick;
    }

    bool canStartEarly() const noexcept {
        const auto* value = definition(state_.sequenceId);
        return state_.phase == WaveSequencePhase::Preparing && value &&
               value->allowEarlyStart;
    }

    WaveSequenceResult start(WaveSequenceId id, std::uint64_t tick) {
        if (active()) {
            return {false, WaveSequenceFailure::AlreadyActive, id,
                    state_.currentWave};
        }
        const auto* value = definition(id);
        if (!value) {
            return {false, WaveSequenceFailure::UnknownSequence, id, 0};
        }
        if (!validDefinition(*value)) {
            return {false, WaveSequenceFailure::InvalidDefinition, id, 0};
        }

        WaveSequenceState next;
        next.sequenceId = id;
        next.phase = WaveSequencePhase::Preparing;
        next.currentWave = value->waves.front();
        next.preparationStartedTick = tick;
        if (!addTicks(tick, value->initialPreparationTicks,
                      next.scheduledStartTick)) {
            return {false, WaveSequenceFailure::TickOverflow, id,
                    next.currentWave};
        }
        state_ = next;
        return {true, WaveSequenceFailure::None, id, next.currentWave};
    }

    WaveSequenceResult requestEarlyStart(
        WaveSequenceId id,
        std::uint64_t tick) {
        if (state_.phase != WaveSequencePhase::Preparing) {
            return {false, WaveSequenceFailure::NotPreparing, id,
                    state_.currentWave};
        }
        if (id != state_.sequenceId) {
            return {false, WaveSequenceFailure::StaleSequenceCommand, id,
                    state_.currentWave};
        }
        const auto* value = definition(id);
        if (!value) {
            return {false, WaveSequenceFailure::UnknownSequence, id,
                    state_.currentWave};
        }
        if (!value->allowEarlyStart) {
            return {false, WaveSequenceFailure::EarlyStartDisabled, id,
                    state_.currentWave};
        }
        state_.scheduledStartTick = tick;
        return {true, WaveSequenceFailure::None, id, state_.currentWave};
    }

    bool markWaveStartQueued() noexcept {
        if (state_.phase != WaveSequencePhase::Preparing) return false;
        state_.phase = WaveSequencePhase::StartingWave;
        state_.scheduledStartTick = noScheduledTick();
        return true;
    }

    bool markWaveStarted(WaveId id) noexcept {
        if (state_.phase != WaveSequencePhase::StartingWave ||
            id != state_.currentWave) {
            return false;
        }
        state_.phase = WaveSequencePhase::WaveActive;
        return true;
    }

    WaveSequenceResult markWaveCompleted(
        WaveId id,
        std::uint64_t tick,
        bool rewardPending) {
        if (state_.phase != WaveSequencePhase::WaveActive ||
            id != state_.currentWave) {
            return {false, WaveSequenceFailure::WaveRejected,
                    state_.sequenceId, id};
        }
        if (rewardPending) {
            state_.phase = WaveSequencePhase::RewardPending;
            return {true, WaveSequenceFailure::None,
                    state_.sequenceId, id};
        }
        return advance(tick);
    }

    WaveSequenceResult markRewardChosen(std::uint64_t tick) {
        if (state_.phase != WaveSequencePhase::RewardPending) {
            return {false, WaveSequenceFailure::RewardNotPending,
                    state_.sequenceId, state_.currentWave};
        }
        return advance(tick);
    }

    bool fail(WaveSequenceFailure failure) noexcept {
        if (state_.phase == WaveSequencePhase::Idle ||
            state_.phase == WaveSequencePhase::Complete ||
            state_.phase == WaveSequencePhase::Failed) {
            return false;
        }
        state_.phase = WaveSequencePhase::Failed;
        state_.scheduledStartTick = noScheduledTick();
        lastFailure_ = failure;
        return true;
    }

    WaveSequenceFailure lastFailure() const noexcept { return lastFailure_; }

    bool restore(WaveSequenceState state,
                 WaveSequenceFailure failure = WaveSequenceFailure::None) {
        if (!validState(state)) return false;
        state_ = state;
        lastFailure_ = failure;
        return true;
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(state_.sequenceId);
        hash.WriteU8(static_cast<std::uint8_t>(state_.phase));
        hash.WriteU32(state_.waveIndex);
        hash.WriteU32(state_.completedWaves);
        hash.WriteU32(state_.currentWave);
        hash.WriteU64(state_.preparationStartedTick);
        hash.WriteU64(state_.scheduledStartTick);
        hash.WriteU8(static_cast<std::uint8_t>(lastFailure_));

        const auto* value = definition(state_.sequenceId);
        hash.WriteBool(value != nullptr);
        if (!value) return;
        hash.WriteU32(value->id);
        hash.WriteU32(static_cast<std::uint32_t>(value->waves.size()));
        for (const auto waveId : value->waves) hash.WriteU32(waveId);
        hash.WriteU32(value->initialPreparationTicks);
        hash.WriteU32(value->interWavePreparationTicks);
        hash.WriteBool(value->allowEarlyStart);
    }

    static constexpr std::uint64_t noScheduledTick() noexcept {
        return std::numeric_limits<std::uint64_t>::max();
    }

private:
    static bool validDefinition(
        const WaveSequenceDefinition& definition) noexcept {
        return definition.id != 0 && !definition.waves.empty() &&
               std::all_of(
                   definition.waves.begin(), definition.waves.end(),
                   [](WaveId id) { return id != 0; });
    }

    bool validState(const WaveSequenceState& state) const noexcept {
        if (state.phase == WaveSequencePhase::Idle) {
            return state.sequenceId == 0 && state.waveIndex == 0 &&
                   state.completedWaves == 0 && state.currentWave == 0 &&
                   state.preparationStartedTick == 0 &&
                   state.scheduledStartTick == 0;
        }
        const auto* value = definition(state.sequenceId);
        if (!value || state.completedWaves != state.waveIndex ||
            state.waveIndex > value->waves.size()) {
            return false;
        }
        if (state.phase == WaveSequencePhase::Complete) {
            return state.waveIndex == value->waves.size() &&
                   state.currentWave == 0 &&
                   state.scheduledStartTick == noScheduledTick();
        }
        if (state.waveIndex >= value->waves.size() ||
            state.currentWave != value->waves[state.waveIndex]) {
            return false;
        }
        if (state.phase == WaveSequencePhase::Preparing) {
            return state.scheduledStartTick != noScheduledTick() &&
                   state.scheduledStartTick >= state.preparationStartedTick;
        }
        return state.scheduledStartTick == noScheduledTick();
    }

    WaveSequenceResult advance(std::uint64_t tick) {
        const auto* value = definition(state_.sequenceId);
        if (!value || state_.waveIndex >= value->waves.size()) {
            fail(WaveSequenceFailure::InvalidDefinition);
            return {false, WaveSequenceFailure::InvalidDefinition,
                    state_.sequenceId, state_.currentWave};
        }

        ++state_.completedWaves;
        ++state_.waveIndex;
        if (state_.waveIndex == value->waves.size()) {
            state_.phase = WaveSequencePhase::Complete;
            state_.currentWave = 0;
            state_.scheduledStartTick = noScheduledTick();
            lastFailure_ = WaveSequenceFailure::None;
            return {true, WaveSequenceFailure::None,
                    state_.sequenceId, 0};
        }

        state_.phase = WaveSequencePhase::Preparing;
        state_.currentWave = value->waves[state_.waveIndex];
        state_.preparationStartedTick = tick;
        if (!addTicks(tick, value->interWavePreparationTicks,
                      state_.scheduledStartTick)) {
            fail(WaveSequenceFailure::TickOverflow);
            return {false, WaveSequenceFailure::TickOverflow,
                    state_.sequenceId, state_.currentWave};
        }
        lastFailure_ = WaveSequenceFailure::None;
        return {true, WaveSequenceFailure::None,
                state_.sequenceId, state_.currentWave};
    }

    static bool addTicks(
        std::uint64_t tick,
        std::uint32_t duration,
        std::uint64_t& result) noexcept {
        if (tick > std::numeric_limits<std::uint64_t>::max() - duration) {
            return false;
        }
        result = tick + duration;
        return true;
    }

    template<class T>
    static const T* findById(
        const std::vector<T>& values,
        std::uint32_t id) noexcept {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), id,
            [](const T& value, std::uint32_t key) {
                return value.id < key;
            });
        return iterator != values.end() && iterator->id == id
            ? &*iterator : nullptr;
    }

    template<class T>
    static void replaceById(std::vector<T>& values, T value) {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), value.id,
            [](const T& current, std::uint32_t key) {
                return current.id < key;
            });
        if (iterator != values.end() && iterator->id == value.id) {
            *iterator = std::move(value);
        } else {
            values.insert(iterator, std::move(value));
        }
    }

    std::vector<WaveSequenceDefinition> definitions_;
    WaveSequenceState state_;
    WaveSequenceFailure lastFailure_{WaveSequenceFailure::None};
};

} // namespace rts::tower_defense
