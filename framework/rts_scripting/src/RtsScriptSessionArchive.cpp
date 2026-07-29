#include <RTSEngine/RtsScripting/RtsScriptSessionArchive.h>

#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <utility>

namespace rts::gameplay::scripting {
namespace {

void fail(realscript::runtime::RuntimeError& error, std::string message) {
    error.code = realscript::runtime::ErrorCode::InvalidProgram;
    error.message = std::move(message);
    error.stackTrace.clear();
}

bool sameIdentity(
    const ::rts::scripting::ScriptProgramIdentity& first,
    const ::rts::scripting::ScriptProgramIdentity& second) noexcept {
    return first.bundle == second.bundle &&
           first.bundlePayloadHash == second.bundlePayloadHash &&
           first.script.sdkCompatibilityVersion ==
               second.script.sdkCompatibilityVersion &&
           first.script.gameSdkPackageVersion ==
               second.script.gameSdkPackageVersion &&
           first.script.hostApiHash == second.script.hostApiHash &&
           first.script.programContentHash == second.script.programContentHash;
}

void writeIdentity(
    foundation::BinaryWriter& writer,
    const ::rts::scripting::ScriptProgramIdentity& identity) {
    writer.writeU16(static_cast<std::uint16_t>(identity.bundle.type));
    writer.writeU64(identity.bundle.id);
    writer.writeU64(identity.bundlePayloadHash);
    writer.writeU32(identity.script.sdkCompatibilityVersion);
    writer.writeU32(identity.script.gameSdkPackageVersion);
    writer.writeU64(identity.script.hostApiHash);
    writer.writeU64(identity.script.programContentHash);
}

bool readIdentity(
    foundation::BinaryReader& reader,
    ::rts::scripting::ScriptProgramIdentity& identity) {
    std::uint16_t type = 0;
    if (!reader.readU16(type) ||
        type != static_cast<std::uint16_t>(assets::AssetType::ScriptBundle) ||
        !reader.readU64(identity.bundle.id) || identity.bundle.id == 0 ||
        !reader.readU64(identity.bundlePayloadHash) ||
        identity.bundlePayloadHash == 0 ||
        !reader.readU32(identity.script.sdkCompatibilityVersion) ||
        !reader.readU32(identity.script.gameSdkPackageVersion) ||
        !reader.readU64(identity.script.hostApiHash) ||
        !reader.readU64(identity.script.programContentHash)) {
        return false;
    }
    identity.bundle.type = static_cast<assets::AssetType>(type);
    return identity.script.valid();
}

} // namespace

bool RtsScriptSessionArchive::authoritativeHash(
    const RtsGameSession& session,
    const RtsScriptSession& scripts,
    std::uint64_t& output,
    realscript::runtime::RuntimeError& error) {
    std::uint64_t scriptHash = 0;
    if (!scripts.authoritativeHash(scriptHash, error)) return false;
    foundation::CanonicalHash hash;
    hash.WriteU16(kVersion);
    hash.WriteU64(RtsGameSessionArchive::authoritativeHash(session));
    hash.WriteU64(scriptHash);
    output = hash.Value();
    return true;
}

std::vector<std::uint8_t> RtsScriptSessionArchive::encode(
    const RtsGameSession& session,
    const RtsScriptSession& scripts,
    realscript::runtime::RuntimeError& error) {
    const auto* identity = scripts.programIdentity();
    const auto gameBytes = RtsGameSessionArchive::encode(session);
    const auto scriptBytes = scripts.encodeState(error);
    std::uint64_t hash = 0;
    if (!identity || gameBytes.empty() || gameBytes.size() > kMaximumGameBytes ||
        scriptBytes.empty() || scriptBytes.size() > kMaximumScriptBytes ||
        !authoritativeHash(session, scripts, hash, error)) {
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "RTS script session cannot be encoded");
        }
        return {};
    }

    foundation::BinaryWriter writer;
    writer.writeU32(kMagic);
    writer.writeU16(kVersion);
    writeIdentity(writer, *identity);
    writer.writeU64(hash);
    writer.writeU32(static_cast<std::uint32_t>(gameBytes.size()));
    writer.writeBytes(gameBytes);
    writer.writeU32(static_cast<std::uint32_t>(scriptBytes.size()));
    writer.writeBytes(scriptBytes);
    return writer.take();
}

bool RtsScriptSessionArchive::decode(
    const std::vector<std::uint8_t>& bytes,
    RtsGameSession& session,
    RtsScriptSession& scripts,
    realscript::runtime::RuntimeError& error) {
    const auto* currentIdentity = scripts.programIdentity();
    if (!currentIdentity) {
        fail(error, "RTS script runtime identity is unavailable");
        return false;
    }

    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint64_t storedHash = 0;
    std::uint32_t gameSize = 0;
    std::uint32_t scriptSize = 0;
    ::rts::scripting::ScriptProgramIdentity identity;
    std::vector<std::uint8_t> gameBytes;
    std::vector<std::uint8_t> scriptBytes;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != kMagic || version != kVersion ||
        !readIdentity(reader, identity) ||
        !sameIdentity(identity, *currentIdentity) ||
        !reader.readU64(storedHash) || storedHash == 0 ||
        !reader.readU32(gameSize) || gameSize == 0 ||
        gameSize > kMaximumGameBytes ||
        !reader.readBytes(gameSize, gameBytes, kMaximumGameBytes) ||
        !reader.readU32(scriptSize) || scriptSize == 0 ||
        scriptSize > kMaximumScriptBytes ||
        !reader.readBytes(scriptSize, scriptBytes, kMaximumScriptBytes) ||
        !reader.atEnd()) {
        fail(error, "invalid or incompatible RTS script session archive");
        return false;
    }

    realscript::runtime::RuntimeError backupError;
    const auto previousGame = RtsGameSessionArchive::encode(session);
    const auto previousScript = scripts.encodeState(backupError);
    if (previousGame.empty() || previousScript.empty()) {
        fail(error, "current RTS script session cannot be backed up for restore");
        return false;
    }

    const auto rollback = [&]() {
        realscript::runtime::RuntimeError ignored;
        (void)RtsGameSessionArchive::decode(previousGame, session);
        (void)scripts.restoreEncodedState(previousScript, ignored);
    };

    if (!RtsGameSessionArchive::decode(gameBytes, session) ||
        !scripts.restoreEncodedState(scriptBytes, error)) {
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "RTS game or script session restore failed");
        }
        rollback();
        return false;
    }

    std::uint64_t restoredHash = 0;
    if (!authoritativeHash(session, scripts, restoredHash, error) ||
        restoredHash != storedHash) {
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "RTS script session authoritative hash mismatch");
        }
        rollback();
        return false;
    }
    return true;
}

} // namespace rts::gameplay::scripting
