#pragma once

#include <RTSEngine/Audio/AudioDevice.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::audio {

enum class AudioBus : std::uint8_t {
    Master,
    Music,
    Ambience,
    Sfx,
    Ui,
    Voice,
    Count
};

struct AudioListener2D final {
    float x{};
    float y{};
    float hearingRadius{24.0f};
};

struct MixedAudioPlayCommand final {
    AudioPlayCommand command{};
    AudioBus bus{AudioBus::Sfx};
    std::uint8_t priority{128};
    bool nonStealable{};
};

struct AudioMixerStats final {
    std::uint32_t activeVoices{};
    std::uint64_t playedVoices{};
    std::uint64_t rejectedVoices{};
    std::uint64_t stolenVoices{};
    std::uint64_t deviceResets{};
};

class AudioMixer final {
public:
    explicit AudioMixer(
        AudioDevice& device,
        std::uint32_t maximumVoices = 64) noexcept
        : device_(device),
          maximumVoices_(std::max<std::uint32_t>(1, maximumVoices)),
          deviceGeneration_(device.deviceGeneration()) {
        volumes_.fill(1.0f);
        muted_.fill(false);
    }

    void setListener(AudioListener2D listener) noexcept {
        if (!std::isfinite(listener.x)) listener.x = 0;
        if (!std::isfinite(listener.y)) listener.y = 0;
        if (!std::isfinite(listener.hearingRadius) ||
            listener.hearingRadius <= 0) {
            listener.hearingRadius = 1;
        }
        listener_ = listener;
    }

    AudioListener2D listener() const noexcept { return listener_; }

    void setBusVolume(AudioBus bus, float volume) noexcept {
        if (!validBus(bus) || !std::isfinite(volume)) return;
        volumes_[busIndex(bus)] = std::clamp(volume, 0.0f, 1.0f);
    }

    float busVolume(AudioBus bus) const noexcept {
        return validBus(bus) ? volumes_[busIndex(bus)] : 0.0f;
    }

    void setBusMuted(AudioBus bus, bool muted) noexcept {
        if (validBus(bus)) muted_[busIndex(bus)] = muted;
    }

    bool busMuted(AudioBus bus) const noexcept {
        return validBus(bus) && muted_[busIndex(bus)];
    }

    VoiceHandle play(MixedAudioPlayCommand request) {
        synchronizeDevice();
        if (!validBus(request.bus) || request.command.clipId == 0) {
            ++stats_.rejectedVoices;
            return {};
        }
        const auto volume = resolvedVolume(request);
        if (volume <= 0) {
            ++stats_.rejectedVoices;
            return {};
        }
        if (voices_.size() >= maximumVoices_ && !stealFor(request.priority)) {
            ++stats_.rejectedVoices;
            return {};
        }
        request.command.volume = volume;
        if (request.command.spatial) {
            request.command.pan = resolvedPan(request.command.x);
        }
        const auto handle = device_.play(request.command);
        if (!handle.valid()) {
            ++stats_.rejectedVoices;
            return {};
        }
        voices_.push_back({
            handle,
            request.command.eventId,
            request.bus,
            request.priority,
            request.nonStealable,
            ++ordinal_});
        ++stats_.playedVoices;
        refreshCount();
        return handle;
    }

    bool stop(VoiceHandle handle) {
        synchronizeDevice();
        const auto iterator = find(handle);
        if (iterator == voices_.end()) return false;
        const auto stopped = device_.stop(handle);
        voices_.erase(iterator);
        refreshCount();
        return stopped;
    }

    std::uint32_t stopEvent(AudioEventId eventId) {
        synchronizeDevice();
        std::uint32_t count = 0;
        auto iterator = voices_.begin();
        while (iterator != voices_.end()) {
            if (iterator->eventId != eventId) {
                ++iterator;
                continue;
            }
            (void)device_.stop(iterator->handle);
            iterator = voices_.erase(iterator);
            ++count;
        }
        refreshCount();
        return count;
    }

    bool retire(VoiceHandle handle) noexcept {
        const auto iterator = find(handle);
        if (iterator == voices_.end()) return false;
        voices_.erase(iterator);
        refreshCount();
        return true;
    }

    void stopAll() noexcept {
        device_.stopAll();
        voices_.clear();
        refreshCount();
    }

    void reset() noexcept {
        device_.reset();
        voices_.clear();
        deviceGeneration_ = device_.deviceGeneration();
        ++stats_.deviceResets;
        refreshCount();
    }

    const AudioMixerStats& stats() const noexcept { return stats_; }
    std::uint32_t maximumVoices() const noexcept { return maximumVoices_; }

private:
    struct ActiveVoice final {
        VoiceHandle handle{};
        AudioEventId eventId{};
        AudioBus bus{AudioBus::Sfx};
        std::uint8_t priority{};
        bool nonStealable{};
        std::uint64_t ordinal{};
    };

    static constexpr std::size_t busIndex(AudioBus bus) noexcept {
        return static_cast<std::size_t>(bus);
    }

    static constexpr bool validBus(AudioBus bus) noexcept {
        return bus < AudioBus::Count;
    }

    float resolvedVolume(const MixedAudioPlayCommand& request) const noexcept {
        if (muted_[busIndex(AudioBus::Master)] ||
            muted_[busIndex(request.bus)]) {
            return 0;
        }
        auto volume = std::clamp(request.command.volume, 0.0f, 1.0f) *
                      volumes_[busIndex(AudioBus::Master)] *
                      volumes_[busIndex(request.bus)];
        if (!request.command.spatial) return volume;
        const auto dx = request.command.x - listener_.x;
        const auto dy = request.command.y - listener_.y;
        const auto distance = std::sqrt(dx * dx + dy * dy);
        const auto attenuation = std::clamp(
            1.0f - distance / listener_.hearingRadius, 0.0f, 1.0f);
        return volume * attenuation * attenuation;
    }

    float resolvedPan(float x) const noexcept {
        return std::clamp(
            (x - listener_.x) / std::max(1.0f, listener_.hearingRadius),
            -1.0f,
            1.0f);
    }

    bool stealFor(std::uint8_t incomingPriority) {
        auto candidate = voices_.end();
        for (auto iterator = voices_.begin(); iterator != voices_.end(); ++iterator) {
            if (iterator->nonStealable || iterator->priority > incomingPriority) {
                continue;
            }
            if (candidate == voices_.end() ||
                iterator->priority < candidate->priority ||
                (iterator->priority == candidate->priority &&
                 iterator->ordinal < candidate->ordinal)) {
                candidate = iterator;
            }
        }
        if (candidate == voices_.end()) return false;
        (void)device_.stop(candidate->handle);
        voices_.erase(candidate);
        ++stats_.stolenVoices;
        return true;
    }

    std::vector<ActiveVoice>::iterator find(VoiceHandle handle) noexcept {
        return std::find_if(
            voices_.begin(), voices_.end(),
            [&](const ActiveVoice& voice) {
                return voice.handle == handle;
            });
    }

    void synchronizeDevice() noexcept {
        const auto generation = device_.deviceGeneration();
        if (generation == deviceGeneration_) return;
        voices_.clear();
        deviceGeneration_ = generation;
        ++stats_.deviceResets;
        refreshCount();
    }

    void refreshCount() noexcept {
        stats_.activeVoices = static_cast<std::uint32_t>(voices_.size());
    }

    AudioDevice& device_;
    std::uint32_t maximumVoices_{64};
    std::uint32_t deviceGeneration_{};
    AudioListener2D listener_{};
    std::array<float, static_cast<std::size_t>(AudioBus::Count)> volumes_{};
    std::array<bool, static_cast<std::size_t>(AudioBus::Count)> muted_{};
    std::vector<ActiveVoice> voices_;
    std::uint64_t ordinal_{};
    AudioMixerStats stats_{};
};

} // namespace rts::audio
