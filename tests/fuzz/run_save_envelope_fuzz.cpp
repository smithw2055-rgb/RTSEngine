#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    constexpr std::size_t maximumInput = 8u * 1024u * 1024u;
    if (size > maximumInput) return 0;

    const std::vector<std::uint8_t> bytes(data, data + size);
    const auto migration = rts::roguelite::MigrateRunSaveToCurrent(bytes);
    (void)migration;

    const auto decoded = rts::roguelite::RunSaveEnvelopeCodec::decode(bytes);
    if (decoded.error == rts::roguelite::RunSaveEnvelopeError::None) {
        const auto rebuilt = rts::roguelite::RunSaveEnvelopeCodec::build(
            decoded.envelope.manifest.identity,
            decoded.envelope.manifest.sequence,
            decoded.envelope.payload);
        if (rebuilt.error == rts::roguelite::RunSaveEnvelopeError::None) {
            const auto canonical =
                rts::roguelite::RunSaveEnvelopeCodec::decode(rebuilt.bytes);
            (void)canonical;
        }
    }
    return 0;
}
