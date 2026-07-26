#include <RTSEngine/Roguelite/RunSaveCloudConflict.h>
#include <RTSEngine/Roguelite/RunSaveSlotStore.h>
#include <rts/foundation/ArchiveChecksum.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

roguelite::RunSaveSchema makeSave(
    std::uint64_t tick,
    std::uint8_t marker) {
    roguelite::RunSaveSchema save;
    save.tick = tick;
    save.rootSeed = 0xabcdefu;
    save.run = {1, roguelite::RunPhase::WaveActive, 0, 0, 1};
    save.resources = {100, 0, 0};
    save.runCommands.committedThrough = tick + 1;
    save.towerCommands.committedThrough = tick + 1;
    save.rtsCommands.committedThrough = tick + 1;
    save.authoritativeState = {marker, static_cast<std::uint8_t>(marker + 1u)};
    return save;
}

roguelite::RunSaveManifestIdentity identity(
    roguelite::RunSaveCloudRevision cloud = {},
    std::uint64_t content = 0x2222u) {
    return {
        roguelite::MakeSaveIdentifier("RTSEngine.cloud-tests"),
        content,
        0x3333u,
        std::move(cloud)};
}

roguelite::RunSaveEnvelopeDecodeResult buildAndDecode(
    std::uint64_t sequence,
    std::uint64_t tick,
    std::uint8_t marker,
    roguelite::RunSaveCloudRevision cloud = {},
    std::uint64_t content = 0x2222u) {
    const auto payload = roguelite::EncodeRunSave(makeSave(tick, marker));
    const auto built = roguelite::RunSaveEnvelopeCodec::build(
        identity(std::move(cloud), content), sequence, payload);
    assert(built.error == roguelite::RunSaveEnvelopeError::None);
    const auto decoded = roguelite::RunSaveEnvelopeCodec::decode(built.bytes);
    assert(decoded.error == roguelite::RunSaveEnvelopeError::None);
    return decoded;
}

std::vector<std::uint8_t> encodeEnvelopeV1(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t sequence,
    std::uint64_t tick) {
    const auto currentIdentity = identity();
    sim::BinaryWriter protectedWriter;
    protectedWriter.writeU16(roguelite::RunSaveEnvelopeCodec::kKind);
    protectedWriter.writeU64(currentIdentity.productId);
    protectedWriter.writeU64(currentIdentity.contentManifestId);
    protectedWriter.writeU64(currentIdentity.buildId);
    protectedWriter.writeU64(sequence);
    protectedWriter.writeU64(tick);
    protectedWriter.writeU16(roguelite::RunSaveSchema::kSchemaVersion);
    protectedWriter.writeU32(roguelite::kRunSaveFlagAuthoritative);
    protectedWriter.writeU32(static_cast<std::uint32_t>(payload.size()));
    protectedWriter.writeBytes(payload);
    const auto checksum = foundation::ArchiveChecksum(
        protectedWriter.bytes());

    sim::BinaryWriter writer;
    writer.writeU32(roguelite::RunSaveEnvelopeCodec::kMagic);
    writer.writeU16(1u);
    writer.writeU16(roguelite::RunSaveEnvelopeCodec::kKind);
    writer.writeU64(currentIdentity.productId);
    writer.writeU64(currentIdentity.contentManifestId);
    writer.writeU64(currentIdentity.buildId);
    writer.writeU64(sequence);
    writer.writeU64(tick);
    writer.writeU16(roguelite::RunSaveSchema::kSchemaVersion);
    writer.writeU32(roguelite::kRunSaveFlagAuthoritative);
    writer.writeU32(static_cast<std::uint32_t>(payload.size()));
    writer.writeU64(checksum);
    writer.writeBytes(payload);
    return writer.take();
}

std::filesystem::path uniqueDirectory() {
    const auto value = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("rtsengine_cloud_save_tests_" + std::to_string(value));
}

void testEnvelopeV1Compatibility() {
    const auto payload = roguelite::EncodeRunSave(makeSave(5, 1));
    const auto bytes = encodeEnvelopeV1(payload, 1, 5);
    const auto decoded = roguelite::RunSaveEnvelopeCodec::decode(bytes);
    assert(decoded.error == roguelite::RunSaveEnvelopeError::None);
    assert(!decoded.envelope.manifest.identity.cloud.tracked());
    assert(decoded.envelope.save.tick == 5);
}

void testRevisionGraphAndConflictPolicy() {
    constexpr std::uint64_t lineage = 0x1000u;
    constexpr std::uint64_t deviceA = 0xa1u;
    constexpr std::uint64_t deviceB = 0xb2u;

    const auto root = buildAndDecode(
        1, 10, 1,
        roguelite::MakeRunSaveCloudRevision(lineage, deviceA));
    const auto& rootRevision = root.envelope.manifest.identity.cloud;
    assert(rootRevision.revisionId != 0);
    assert(rootRevision.logicalClock == 1);
    assert(rootRevision.parentRevisionIds.empty());

    const auto local = buildAndDecode(
        2, 20, 2,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA, {rootRevision}));
    const auto& localRevision = local.envelope.manifest.identity.cloud;
    assert(localRevision.parentRevisionIds ==
           std::vector<std::uint64_t>{rootRevision.revisionId});
    assert(roguelite::RunSaveCloudClockDominates(
        localRevision, rootRevision));

    auto resolution = roguelite::ResolveRunSaveCloudConflict(
        &local, &root);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::LocalDescendant);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::UploadLocal);
    assert(resolution.selected == roguelite::RunSaveCloudSide::Local);

    const auto cloud = buildAndDecode(
        3, 30, 3,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceB, {rootRevision}));
    const auto& cloudRevision = cloud.envelope.manifest.identity.cloud;
    assert(!roguelite::RunSaveCloudClockDominates(
        localRevision, cloudRevision));
    assert(!roguelite::RunSaveCloudClockDominates(
        cloudRevision, localRevision));

    resolution = roguelite::ResolveRunSaveCloudConflict(
        &local, &cloud);
    assert(resolution.relation == roguelite::RunSaveCloudRelation::Diverged);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::PreserveBoth);
    assert(resolution.conflict);

    resolution = roguelite::ResolveRunSaveCloudConflict(
        &local,
        &cloud,
        roguelite::RunSaveCloudDivergencePolicy::PreferDeterministicLatest);
    assert(resolution.action == roguelite::RunSaveCloudAction::UseCloud);
    assert(resolution.selected == roguelite::RunSaveCloudSide::Cloud);

    const auto merged = buildAndDecode(
        4, 40, 4,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA, {localRevision, cloudRevision}));
    const auto& mergedRevision = merged.envelope.manifest.identity.cloud;
    assert(mergedRevision.parentRevisionIds.size() == 2);
    assert(roguelite::RunSaveCloudClockDominates(
        mergedRevision, localRevision));
    assert(roguelite::RunSaveCloudClockDominates(
        mergedRevision, cloudRevision));

    resolution = roguelite::ResolveRunSaveCloudConflict(
        &merged, &local);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::LocalDescendant);

    resolution = roguelite::ResolveRunSaveCloudConflict(
        &merged, &merged);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::IdenticalRevision);
    assert(resolution.action == roguelite::RunSaveCloudAction::None);

    const auto unrelated = buildAndDecode(
        5, 50, 5,
        roguelite::MakeRunSaveCloudRevision(0x2000u, deviceB));
    resolution = roguelite::ResolveRunSaveCloudConflict(
        &local, &unrelated);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::UnrelatedLineage);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::PreserveBoth);

    const auto incompatible = buildAndDecode(
        6, 60, 6,
        roguelite::MakeRunSaveCloudRevision(lineage, deviceB),
        0x9999u);
    resolution = roguelite::ResolveRunSaveCloudConflict(
        &local, &incompatible);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::IncompatibleManifest);
    assert(resolution.action == roguelite::RunSaveCloudAction::Reject);
}

void testUntrackedAndInvalidCandidates() {
    const auto first = buildAndDecode(1, 10, 7);
    const auto same = buildAndDecode(2, 10, 7);
    const auto different = buildAndDecode(3, 11, 8);

    auto resolution = roguelite::ResolveRunSaveCloudConflict(
        &first, &same);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::EquivalentUntracked);

    resolution = roguelite::ResolveRunSaveCloudConflict(
        &first, &different);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::AncestryUnavailable);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::PreserveBoth);

    roguelite::RunSaveEnvelopeDecodeResult invalid;
    resolution = roguelite::ResolveRunSaveCloudConflict(
        &invalid, &first);
    assert(resolution.relation ==
           roguelite::RunSaveCloudRelation::LocalInvalid);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::DownloadCloud);

    resolution = roguelite::ResolveRunSaveCloudConflict(nullptr, &first);
    assert(resolution.relation == roguelite::RunSaveCloudRelation::CloudOnly);
    assert(resolution.action ==
           roguelite::RunSaveCloudAction::DownloadCloud);
}

void testCloudMetadataThroughSlotStore() {
    constexpr std::uint64_t lineage = 0xabcdu;
    constexpr std::uint64_t device = 0x1234u;
    const auto directory = uniqueDirectory();
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    auto rootIdentity = identity(
        roguelite::MakeRunSaveCloudRevision(lineage, device));
    const auto firstPayload = roguelite::EncodeRunSave(makeSave(10, 1));
    const auto firstWrite = roguelite::RunSaveSlotStore::write(
        directory, "cloud_slot", rootIdentity, 1, firstPayload);
    assert(firstWrite.error == roguelite::RunSaveSlotError::None);

    auto loaded = roguelite::RunSaveSlotStore::load(
        directory, "cloud_slot", rootIdentity);
    assert(loaded.error == roguelite::RunSaveSlotError::None);
    const auto rootRevision = loaded.decoded.envelope.manifest.identity.cloud;
    assert(rootRevision.revisionId != 0);

    auto childIdentity = identity(
        roguelite::MakeRunSaveCloudRevision(
            lineage, device, {rootRevision}));
    const auto secondPayload = roguelite::EncodeRunSave(makeSave(20, 2));
    const auto secondWrite = roguelite::RunSaveSlotStore::write(
        directory, "cloud_slot", childIdentity, 2, secondPayload);
    assert(secondWrite.error == roguelite::RunSaveSlotError::None);

    loaded = roguelite::RunSaveSlotStore::load(
        directory, "cloud_slot", childIdentity);
    assert(loaded.error == roguelite::RunSaveSlotError::None);
    const auto& childRevision =
        loaded.decoded.envelope.manifest.identity.cloud;
    assert(childRevision.parentRevisionIds ==
           std::vector<std::uint64_t>{rootRevision.revisionId});
    assert(roguelite::RunSaveCloudClockDominates(
        childRevision, rootRevision));

    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    testEnvelopeV1Compatibility();
    testRevisionGraphAndConflictPolicy();
    testUntrackedAndInvalidCandidates();
    testCloudMetadataThroughSlotStore();
    std::cout << "cloud save tests passed\n";
    return 0;
}
