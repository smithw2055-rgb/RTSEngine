#pragma once

#include <RTSEngine/Audio/AudioDevice.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::audio {

struct RecordedAudioPlay final {
    VoiceHandle voice{};
    AudioPlayCommand command{};
};

class NullAudioDevice final : public AudioDevice {
public:
    std::uint32_t deviceGeneration() const noexcept override {
        return deviceGeneration_;
    }

    VoiceHandle play(const AudioPlayCommand& command) override {
        if (command.eventId == 0 || command.clipId == 0 ||
            !std::isfinite(command.volume) || command.volume < 0.0f ||
            !std::isfinite(command.pan) || command.pan < -1.0f ||
            command.pan > 1.0f || !std::isfinite(command.x) ||
            !std::isfinite(command.y)) {
            return {};
        }

        std::size_t index = 0;
        for (; index < voices_.size(); ++index) {
            if (!voices_[index].alive) break;
        }
        if (index == voices_.size()) voices_.push_back({});
        auto& slot = voices_[index];
        if (slot.generation == 0) slot.generation = 1;
        slot.alive = true;
        slot.command = command;
        const VoiceHandle handle{
            static_cast<std::uint32_t>(index + 1u), slot.generation};
        recorded_.push_back({handle, command});
        return handle;
    }

    bool stop(VoiceHandle voice) override {
        auto* slot = find(voice);
        if (!slot) return false;
        slot->alive = false;
        slot->command = {};
        slot->generation = nextGeneration(slot->generation);
        return true;
    }

    void stopAll() noexcept override {
        for (auto& slot : voices_) {
            if (!slot.alive) continue;
            slot.alive = false;
            slot.command = {};
            slot.generation = nextGeneration(slot.generation);
        }
    }

    void reset() noexcept override {
        stopAll();
        recorded_.clear();
        ++deviceGeneration_;
        if (deviceGeneration_ == 0) deviceGeneration_ = 1;
    }

    bool valid(VoiceHandle voice) const noexcept {
        return find(voice) != nullptr;
    }

    std::size_t activeVoiceCount() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            voices_.begin(), voices_.end(),
            [](const VoiceSlot& slot) { return slot.alive; }));
    }

    const std::vector<RecordedAudioPlay>& recordedPlays() const noexcept {
        return recorded_;
    }

    void clearRecordedPlays() { recorded_.clear(); }

private:
    struct VoiceSlot final {
        std::uint32_t generation{1};
        bool alive{};
        AudioPlayCommand command{};
    };

    static std::uint32_t nextGeneration(std::uint32_t value) noexcept {
        ++value;
        return value == 0 ? 1u : value;
    }

    VoiceSlot* find(VoiceHandle handle) noexcept {
        if (!handle.valid() || handle.index > voices_.size()) return nullptr;
        auto& slot = voices_[handle.index - 1u];
        return slot.alive && slot.generation == handle.generation
            ? &slot : nullptr;
    }

    const VoiceSlot* find(VoiceHandle handle) const noexcept {
        if (!handle.valid() || handle.index > voices_.size()) return nullptr;
        const auto& slot = voices_[handle.index - 1u];
        return slot.alive && slot.generation == handle.generation
            ? &slot : nullptr;
    }

    std::vector<VoiceSlot> voices_;
    std::vector<RecordedAudioPlay> recorded_;
    std::uint32_t deviceGeneration_{1};
};

} // namespace rts::audio
