#pragma once

#include <RTSEngine/Audio/AudioDevice.h>

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
    std::uint32_t deviceGeneration() const noexcept override;
    VoiceHandle play(const AudioPlayCommand& command) override;
    bool stop(VoiceHandle voice) override;
    void stopAll() noexcept override;
    void reset() noexcept override;

    bool valid(VoiceHandle voice) const noexcept;
    std::size_t activeVoiceCount() const noexcept;
    const std::vector<RecordedAudioPlay>& recordedPlays() const noexcept;
    void clearRecordedPlays();

private:
    struct VoiceSlot final {
        std::uint32_t generation{1};
        bool alive{};
        AudioPlayCommand command{};
    };

    static std::uint32_t nextGeneration(std::uint32_t value) noexcept;
    VoiceSlot* find(VoiceHandle handle) noexcept;
    const VoiceSlot* find(VoiceHandle handle) const noexcept;

    std::vector<VoiceSlot> voices_;
    std::vector<RecordedAudioPlay> recorded_;
    std::uint32_t deviceGeneration_{1};
};

} // namespace rts::audio
