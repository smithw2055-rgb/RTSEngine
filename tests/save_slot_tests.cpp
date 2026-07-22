#include <RTSEngine/Roguelite/RunSaveSlotStore.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

class InjectedDurability final : public roguelite::IRunSaveDurability {
public:
    roguelite::RunSaveDurabilityOperation failOperation{
        roguelite::RunSaveDurabilityOperation::None};
    std::int64_t nativeCode{4242};

    roguelite::RunSaveDurabilityResult syncFile(
        const std::filesystem::path& path) noexcept override {
        if (failOperation == roguelite::RunSaveDurabilityOperation::SyncFile) {
            return failure(
                roguelite::RunSaveDurabilityOperation::SyncFile,
                roguelite::RunSaveDurabilityError::SyncFailed);
        }
        return platform_.syncFile(path);
    }

    roguelite::RunSaveDurabilityResult replaceFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) noexcept override {
        if (failOperation ==
            roguelite::RunSaveDurabilityOperation::ReplaceFile) {
            return failure(
                roguelite::RunSaveDurabilityOperation::ReplaceFile,
                roguelite::RunSaveDurabilityError::ReplaceFailed);
        }
        return platform_.replaceFile(source, destination);
    }

    roguelite::RunSaveDurabilityResult removeFile(
        const std::filesystem::path& path) noexcept override {
        if (failOperation == roguelite::RunSaveDurabilityOperation::RemoveFile) {
            return failure(
                roguelite::RunSaveDurabilityOperation::RemoveFile,
                roguelite::RunSaveDurabilityError::RemoveFailed);
        }
        return platform_.removeFile(path);
    }

    roguelite::RunSaveDurabilityResult syncDirectory(
        const std::filesystem::path& path) noexcept override {
        if (failOperation ==
            roguelite::RunSaveDurabilityOperation::SyncDirectory) {
            return failure(
                roguelite::RunSaveDurabilityOperation::SyncDirectory,
                roguelite::RunSaveDurabilityError::DirectorySyncFailed);
        }
        return platform_.syncDirectory(path);
    }

private:
    roguelite::RunSaveDurabilityResult failure(
        roguelite::RunSaveDurabilityOperation operation,
        roguelite::RunSaveDurabilityError error) const noexcept {
        return {operation, error, nativeCode};
    }

    roguelite::PlatformRunSaveDurability platform_;
};

roguelite::RunSaveSchema makeSave(
    std::uint64_t tick,
    std::vector<std::uint8_t> authoritative) {
    roguelite::RunSaveSchema save;
    save.tick = tick;
    save.rootSeed = 0x12345678u;
    save.run = {1, roguelite::RunPhase::WaveActive, 0, 0, 10};
    save.resources = {100, 0, 25};
    save.gameplayProfile = {};
    save.runCommands.committedThrough = tick + 1;
    save.towerCommands.committedThrough = tick + 1;
    save.rtsCommands.committedThrough = tick + 1;
    save.authoritativeState = std::move(authoritative);
    return save;
}

std::vector<std::uint8_t> encodeLegacyV1(
    const roguelite::RunSaveSchema& save) {
    sim::BinaryWriter writer;
    sim::WriteSessionHeader(
        writer,
        {sim::kSessionArchiveMagic,
         1u,
         sim::SessionArchiveKind::RogueliteRunSave});
    writer.writeU64(save.tick);
    writer.writeU64(save.rootSeed);
    writer.writeU32(save.run.runId);
    writer.writeU8(static_cast<std::uint8_t>(save.run.phase));
    writer.writeU32(save.run.waveIndex);
    writer.writeU32(save.run.completedWaves);
    writer.writeU32(save.run.currentWave);
    writer.writeI32(save.resources.available);
    writer.writeI32(save.resources.reserved);
    writer.writeI32(save.resources.spent);
    roguelite::WriteTeamModifierProfile(writer, save.gameplayProfile);

    writer.writeU32(static_cast<std::uint32_t>(save.modifierStacks.size()));
    for (const auto& stack : save.modifierStacks) {
        writer.writeU32(stack.id);
        writer.writeU32(stack.stacks);
    }
    writer.writeU32(static_cast<std::uint32_t>(save.randomStreams.size()));
    for (const auto& state : save.randomStreams) {
        sim::WriteRandomStreamState(writer, state);
    }
    writer.writeU32(static_cast<std::uint32_t>(save.checkpoints.size()));
    for (const auto& checkpoint : save.checkpoints) {
        sim::WriteWorldHashCheckpoint(writer, checkpoint);
    }
    roguelite::WriteCommandStreamState(
        writer, save.runCommands, roguelite::WriteRunCommand);
    roguelite::WriteCommandStreamState(
        writer,
        save.towerCommands,
        roguelite::WriteTowerDefenseCommand);
    roguelite::WriteCommandStreamState(
        writer, save.rtsCommands, gameplay::WriteTickCommand);
    return writer.take();
}

roguelite::RunSaveManifestIdentity identity(
    std::uint64_t content = 0x2222u,
    std::uint64_t build = 0x3333u) {
    return {
        roguelite::MakeSaveIdentifier("RTSEngine.tests"),
        content,
        build};
}

std::filesystem::path uniqueDirectory() {
    const auto value = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("rtsengine_save_slot_tests_" + std::to_string(value));
}

void overwriteFile(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    assert(stream);
}

void testMigrationPolicy() {
    auto legacySave = makeSave(9, {});
    legacySave.run.phase = roguelite::RunPhase::BetweenWaves;
    const auto legacyBytes = encodeLegacyV1(legacySave);

    const auto migrated = roguelite::MigrateRunSaveToCurrent(legacyBytes);
    assert(migrated.status ==
           roguelite::RunSaveMigrationStatus::MigratedLegacySummary);
    assert(migrated.sourceSchemaVersion == 1);
    assert(!migrated.resumable);
    assert(!migrated.currentBytes.empty());

    roguelite::RunSaveSchema current;
    assert(roguelite::DecodeRunSave(migrated.currentBytes, current));
    assert(current.tick == legacySave.tick);
    assert(current.authoritativeState.empty());

    const auto envelope = roguelite::RunSaveEnvelopeCodec::build(
        identity(), 1, legacyBytes);
    assert(envelope.error == roguelite::RunSaveEnvelopeError::None);
    assert(envelope.migration ==
           roguelite::RunSaveMigrationStatus::MigratedLegacySummary);
    assert(!envelope.resumable);

    const auto decoded = roguelite::RunSaveEnvelopeCodec::decode(
        envelope.bytes);
    assert(decoded.error == roguelite::RunSaveEnvelopeError::None);
    assert(!decoded.resumable);
    assert((decoded.envelope.manifest.flags &
            roguelite::kRunSaveFlagLegacySummary) != 0);
    assert(decoded.envelope.save.authoritativeState.empty());

    auto future = legacyBytes;
    future[4] = 99u;
    future[5] = 0u;
    const auto unsupported = roguelite::MigrateRunSaveToCurrent(future);
    assert(unsupported.status ==
           roguelite::RunSaveMigrationStatus::UnsupportedFuture);
}

void testEnvelopeCorruptionCorpus() {
    const auto payload = roguelite::EncodeRunSave(
        makeSave(42, {1, 2, 3, 4, 5, 6, 7}));
    const auto built = roguelite::RunSaveEnvelopeCodec::build(
        identity(), 7, payload);
    assert(built.error == roguelite::RunSaveEnvelopeError::None);
    assert(built.resumable);

    const auto decoded = roguelite::RunSaveEnvelopeCodec::decode(built.bytes);
    assert(decoded.error == roguelite::RunSaveEnvelopeError::None);
    assert(decoded.resumable);
    assert(decoded.envelope.manifest.sequence == 7);
    assert(decoded.envelope.manifest.saveTick == 42);

    for (std::size_t size = 0; size < built.bytes.size(); ++size) {
        std::vector<std::uint8_t> truncated(
            built.bytes.begin(), built.bytes.begin() + size);
        assert(roguelite::RunSaveEnvelopeCodec::decode(truncated).error !=
               roguelite::RunSaveEnvelopeError::None);
    }

    for (std::size_t index = 0; index < built.bytes.size(); ++index) {
        auto mutated = built.bytes;
        mutated[index] ^= 0x5au;
        assert(roguelite::RunSaveEnvelopeCodec::decode(mutated).error !=
               roguelite::RunSaveEnvelopeError::None);
    }

    auto trailing = built.bytes;
    trailing.push_back(0u);
    assert(roguelite::RunSaveEnvelopeCodec::decode(trailing).error ==
           roguelite::RunSaveEnvelopeError::TrailingData);
}

void testPrimaryRecoverySlots() {
    const auto directory = uniqueDirectory();
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const auto expected = identity();
    const auto firstPayload = roguelite::EncodeRunSave(
        makeSave(10, {1, 2, 3}));
    const auto secondPayload = roguelite::EncodeRunSave(
        makeSave(20, {4, 5, 6}));
    const auto thirdPayload = roguelite::EncodeRunSave(
        makeSave(30, {7, 8, 9}));

    const auto firstWrite = roguelite::RunSaveSlotStore::write(
        directory, "slot_1", expected, 1, firstPayload);
    assert(firstWrite.error == roguelite::RunSaveSlotError::None);

    auto loaded = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", expected);
    assert(loaded.error == roguelite::RunSaveSlotError::None);
    assert(loaded.source == roguelite::RunSaveSlotSource::Primary);
    assert(loaded.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::LoadedPrimary);
    assert(loaded.decoded.envelope.manifest.sequence == 1);
    assert(loaded.decoded.envelope.save.tick == 10);

    const auto stale = roguelite::RunSaveSlotStore::write(
        directory, "slot_1", expected, 1, secondPayload);
    assert(stale.error == roguelite::RunSaveSlotError::StaleSequence);
    assert(stale.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::StaleSequence);

    const auto secondWrite = roguelite::RunSaveSlotStore::write(
        directory, "slot_1", expected, 2, secondPayload);
    assert(secondWrite.error == roguelite::RunSaveSlotError::None);

    loaded = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", expected);
    assert(loaded.source == roguelite::RunSaveSlotSource::Primary);
    assert(loaded.decoded.envelope.manifest.sequence == 2);
    assert(loaded.decoded.envelope.save.tick == 20);

    const auto primaryPath = directory / "slot_1.sav";
    overwriteFile(primaryPath, {1, 2, 3, 4});
    loaded = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", expected, false, true);
    assert(loaded.error == roguelite::RunSaveSlotError::None);
    assert(loaded.source == roguelite::RunSaveSlotSource::Recovery);
    assert(loaded.repairedPrimary);
    assert(loaded.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::LoadedRecovery);
    assert(loaded.diagnostic.fallbackUsed);
    assert(loaded.diagnostic.repairAttempted);
    assert(loaded.diagnostic.repairSucceeded);
    assert(loaded.decoded.envelope.manifest.sequence == 1);
    assert(loaded.decoded.envelope.save.tick == 10);

    loaded = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", expected);
    assert(loaded.source == roguelite::RunSaveSlotSource::Primary);
    assert(loaded.decoded.envelope.manifest.sequence == 1);

    const auto thirdWrite = roguelite::RunSaveSlotStore::write(
        directory, "slot_1", expected, 3, thirdPayload);
    assert(thirdWrite.error == roguelite::RunSaveSlotError::None);
    loaded = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", expected);
    assert(loaded.decoded.envelope.manifest.sequence == 3);
    assert(loaded.decoded.envelope.save.tick == 30);

    const auto wrongManifest = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", identity(0x9999u));
    assert(wrongManifest.error ==
           roguelite::RunSaveSlotError::ManifestMismatch);
    assert(wrongManifest.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::ManifestMismatch);

    const auto buildAgnostic = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", identity(0x2222u, 0x9999u), false);
    assert(buildAgnostic.error == roguelite::RunSaveSlotError::None);
    const auto exactBuild = roguelite::RunSaveSlotStore::load(
        directory, "slot_1", identity(0x2222u, 0x9999u), true);
    assert(exactBuild.error == roguelite::RunSaveSlotError::ManifestMismatch);

    const auto invalidName = roguelite::RunSaveSlotStore::write(
        directory, "../escape", expected, 4, thirdPayload);
    assert(invalidName.error ==
           roguelite::RunSaveSlotError::InvalidSlotName);

    std::filesystem::remove_all(directory, error);
}

void testDurabilityFailuresAndDiagnosticCodes() {
    const auto directory = uniqueDirectory();
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const auto expected = identity();
    const auto firstPayload = roguelite::EncodeRunSave(
        makeSave(10, {1, 2, 3}));
    const auto secondPayload = roguelite::EncodeRunSave(
        makeSave(20, {4, 5, 6}));

    InjectedDurability injected;
    injected.failOperation = roguelite::RunSaveDurabilityOperation::SyncFile;
    const auto failedSync = roguelite::RunSaveSlotStore::write(
        directory, "durable", expected, 1, firstPayload, injected);
    assert(failedSync.error ==
           roguelite::RunSaveSlotError::TemporarySyncFailed);
    assert(failedSync.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::TemporarySyncFailed);
    assert(failedSync.diagnostic.stage ==
           roguelite::RunSaveDiagnosticStage::SyncTemporary);
    assert(failedSync.diagnostic.durability.error ==
           roguelite::RunSaveDurabilityError::SyncFailed);
    assert(failedSync.diagnostic.nativeCode == injected.nativeCode);
    assert(!std::filesystem::exists(directory / "durable.sav"));

    injected.failOperation = roguelite::RunSaveDurabilityOperation::None;
    assert(roguelite::RunSaveSlotStore::write(
               directory, "durable", expected, 1, firstPayload, injected)
               .error == roguelite::RunSaveSlotError::None);
    assert(roguelite::RunSaveSlotStore::write(
               directory, "durable", expected, 2, secondPayload, injected)
               .error == roguelite::RunSaveSlotError::None);

    overwriteFile(directory / "durable.sav", {9, 8, 7});
    injected.failOperation =
        roguelite::RunSaveDurabilityOperation::ReplaceFile;
    const auto repairFailed = roguelite::RunSaveSlotStore::load(
        directory, "durable", expected, false, true, injected);
    assert(repairFailed.error == roguelite::RunSaveSlotError::None);
    assert(repairFailed.source == roguelite::RunSaveSlotSource::Recovery);
    assert(!repairFailed.repairedPrimary);
    assert(repairFailed.diagnostic.code ==
           roguelite::RunSaveDiagnosticCode::LoadedRecoveryRepairFailed);
    assert(repairFailed.diagnostic.fallbackUsed);
    assert(repairFailed.diagnostic.repairAttempted);
    assert(!repairFailed.diagnostic.repairSucceeded);
    assert(repairFailed.diagnostic.durability.error ==
           roguelite::RunSaveDurabilityError::ReplaceFailed);
    assert(repairFailed.diagnostic.nativeCode == injected.nativeCode);

    assert(roguelite::RunSaveDiagnosticCodeName(
               repairFailed.diagnostic.code) ==
           "save.loaded_recovery_repair_failed");
    assert(roguelite::RunSaveEnvelopeErrorName(
               roguelite::RunSaveEnvelopeError::ChecksumMismatch) ==
           "checksum_mismatch");
    assert(roguelite::RunSaveDurabilityErrorName(
               roguelite::RunSaveDurabilityError::SyncFailed) ==
           "sync_failed");

    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    testMigrationPolicy();
    testEnvelopeCorruptionCorpus();
    testPrimaryRecoverySlots();
    testDurabilityFailuresAndDiagnosticCodes();
    std::cout << "save slot tests passed\n";
    return 0;
}
