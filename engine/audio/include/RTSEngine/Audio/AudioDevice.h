#pragma once

#include <cstdint>

namespace rts::audio {

using AudioEventId = std::uint64_t;
using AudioClipId = std::uint64_t;

struct VoiceTag;

template<class Tag>
struct AudioHandle final {
    std::uint32_t index{};
    std::uint32_t generation{};

    constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(AudioHandle a,
                                     AudioHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
};

using VoiceHandle = AudioHandle<VoiceTag>;

struct AudioPlayCommand final {
    AudioEventId eventId{};
    AudioClipId clipId{};
    float volume{1.0f};
    float pan{};
    float x{};
    float y{};
    bool spatial{};
    bool loop{};
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual std::uint32_t deviceGeneration() const noexcept = 0;
    virtual VoiceHandle play(const AudioPlayCommand& command) = 0;
    virtual bool stop(VoiceHandle voice) = 0;
    virtual void stopAll() noexcept = 0;
    virtual void reset() noexcept = 0;
};

} // namespace rts::audio
