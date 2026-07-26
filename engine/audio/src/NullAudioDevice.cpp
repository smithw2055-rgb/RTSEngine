#include <RTSEngine/Audio/NullAudioDevice.h>

#include <algorithm>
#include <cmath>

namespace rts::audio {

std::uint32_t NullAudioDevice::deviceGeneration() const noexcept {
    return deviceGeneration_;
}

VoiceHandle NullAudioDevice::play(const AudioPlayCommand& command) {
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

bool NullAudioDevice::stop(VoiceHandle voice) {
    auto* slot = find(voice);
    if (!slot) return false;
    slot->alive = false;
    slot->command = {};
    slot->generation = nextGeneration(slot->generation);
    return true;
}

void NullAudioDevice::stopAll() noexcept {
    for (auto& slot : voices_) {
        if (!slot.alive) continue;
        slot.alive = false;
        slot.command = {};
        slot.generation = nextGeneration(slot.generation);
    }
}

void NullAudioDevice::reset() noexcept {
    stopAll();
    recorded_.clear();
    ++deviceGeneration_;
    if (deviceGeneration_ == 0) deviceGeneration_ = 1;
}

bool NullAudioDevice::valid(VoiceHandle voice) const noexcept {
    return find(voice) != nullptr;
}

std::size_t NullAudioDevice::activeVoiceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        voices_.begin(), voices_.end(),
        [](const VoiceSlot& slot) { return slot.alive; }));
}

const std::vector<RecordedAudioPlay>&
NullAudioDevice::recordedPlays() const noexcept {
    return recorded_;
}

void NullAudioDevice::clearRecordedPlays() {
    recorded_.clear();
}

std::uint32_t NullAudioDevice::nextGeneration(std::uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1u : value;
}

NullAudioDevice::VoiceSlot* NullAudioDevice::find(
    VoiceHandle handle) noexcept {
    if (!handle.valid() || handle.index > voices_.size()) return nullptr;
    auto& slot = voices_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

const NullAudioDevice::VoiceSlot* NullAudioDevice::find(
    VoiceHandle handle) const noexcept {
    if (!handle.valid() || handle.index > voices_.size()) return nullptr;
    const auto& slot = voices_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

} // namespace rts::audio
