#pragma once

#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <RTSEngine/RtsScripting/RtsScriptSession.h>

#include <cstdint>
#include <vector>

namespace rts::gameplay::scripting {

class RtsScriptSessionArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x33535352u; // RSS3
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumGameBytes =
        RtsGameSessionArchive::kMaximumNestedBytes;
    static constexpr std::uint32_t kMaximumScriptBytes =
        32u * 1024u * 1024u;

    [[nodiscard]] static bool authoritativeHash(
        const RtsGameSession& session,
        const RtsScriptSession& scripts,
        std::uint64_t& output,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] static std::vector<std::uint8_t> encode(
        const RtsGameSession& session,
        const RtsScriptSession& scripts,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] static bool decode(
        const std::vector<std::uint8_t>& bytes,
        RtsGameSession& session,
        RtsScriptSession& scripts,
        realscript::runtime::RuntimeError& error);
};

} // namespace rts::gameplay::scripting
