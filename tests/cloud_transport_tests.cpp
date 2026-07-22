#include <RTSEngine/Roguelite/RunSaveCloudBranchStore.h>
#include <RTSEngine/Roguelite/RunSaveCloudSync.h>
#include <RTSEngine/Roguelite/RunSaveSlotStore.h>
#include <RTSEngine/Roguelite/RunSaveStoragePolicy.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;

roguelite::RunSaveSchema makeSave(
    std::uint64_t tick,
    std::uint8_t marker) {
    roguelite::RunSaveSchema save;
    save.tick = tick;
    save.rootSeed = 0x55aau;
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
        roguelite::MakeSaveIdentifier("RTSEngine.transport-tests"),
        content,
        0x3333u,
        std::move(cloud)};
}

struct Envelope final {
    std::vector<std::uint8_t> bytes;
    roguelite::RunSaveEnvelopeDecodeResult decoded;
};

Envelope buildEnvelope(
    std::uint64_t sequence,
    std::uint64_t tick,
    std::uint8_t marker,
    roguelite::RunSaveCloudRevision cloud,
    std::uint64_t content = 0x2222u) {
    const auto payload = roguelite::EncodeRunSave(makeSave(tick, marker));
    const auto built = roguelite::RunSaveEnvelopeCodec::build(
        identity(std::move(cloud), content), sequence, payload);
    assert(built.error == roguelite::RunSaveEnvelopeError::None);
    Envelope result;
    result.bytes = built.bytes;
    result.decoded = roguelite::RunSaveEnvelopeCodec::decode(result.bytes);
    assert(result.decoded.error == roguelite::RunSaveEnvelopeError::None);
    return result;
}

std::filesystem::path uniqueDirectory() {
    const auto value = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("rtsengine_cloud_transport_tests_" + std::to_string(value));
}

class RacingTransport final : public roguelite::IRunSaveCloudTransport {
public:
    RacingTransport(
        std::string key,
        std::vector<std::uint8_t> initial,
        std::vector<std::uint8_t> racing)
        : key_(std::move(key)), racing_(std::move(racing)) {
        const auto seeded = inner_.compareExchange(key_, 0, initial);
        assert(seeded.applied);
    }

    roguelite::RunSaveCloudFetchResult fetch(
        std::string_view key) override {
        return inner_.fetch(key);
    }

    roguelite::RunSaveCloudCompareExchangeResult compareExchange(
        std::string_view key,
        std::uint64_t expectedRevisionId,
        const std::vector<std::uint8_t>& envelopeBytes) override {
        if (armed_) {
            const auto current = inner_.fetch(key_);
            assert(current.error ==
                   roguelite::RunSaveCloudTransportError::None);
            const auto raced = inner_.compareExchange(
                key_, current.object.revisionId(), racing_);
            assert(raced.applied);
            armed_ = false;
        }
        return inner_.compareExchange(
            key, expectedRevisionId, envelopeBytes);
    }

private:
    std::string key_;
    std::vector<std::uint8_t> racing_;
    bool armed_{true};
    roguelite::MemoryRunSaveCloudTransport inner_;
};

void testTransportCasAndSync() {
    constexpr std::uint64_t lineage = 0x1000u;
    constexpr std::uint64_t deviceA = 0xa1u;
    constexpr std::uint64_t deviceB = 0xb2u;
    const std::string key = "account-1/campaign";

    const auto root = buildEnvelope(
        1, 10, 1,
        roguelite::MakeRunSaveCloudRevision(lineage, deviceA));
    const auto rootRevision =
        root.decoded.envelope.manifest.identity.cloud;
    const auto localChild = buildEnvelope(
        2, 20, 2,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA, {rootRevision}));
    const auto cloudChild = buildEnvelope(
        3, 30, 3,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceB, {rootRevision}));

    roguelite::MemoryRunSaveCloudTransport transport;
    auto sync = roguelite::SynchronizeRunSaveCloud(
        transport, {key, root.bytes});
    assert(sync.status == roguelite::RunSaveCloudSyncStatus::Uploaded);

    sync = roguelite::SynchronizeRunSaveCloud(
        transport, {key, localChild.bytes});
    assert(sync.status == roguelite::RunSaveCloudSyncStatus::Uploaded);
    auto fetched = transport.fetch(key);
    assert(fetched.object.revisionId() ==
           localChild.decoded.envelope.manifest.identity.cloud.revisionId);

    sync = roguelite::SynchronizeRunSaveCloud(
        transport, {key, root.bytes});
    assert(sync.status == roguelite::RunSaveCloudSyncStatus::Downloaded);
    assert(sync.selectedEnvelopeBytes == localChild.bytes);

    const auto stale = transport.compareExchange(
        key, rootRevision.revisionId, root.bytes);
    assert(!stale.applied);
    assert(stale.error ==
           roguelite::RunSaveCloudTransportError::RevisionMismatch);
    assert(stale.actualRevisionId == fetched.object.revisionId());

    roguelite::MemoryRunSaveCloudTransport divergent;
    assert(divergent.compareExchange(key, 0, root.bytes).applied);
    assert(divergent.compareExchange(
               key, rootRevision.revisionId, cloudChild.bytes).applied);

    sync = roguelite::SynchronizeRunSaveCloud(
        divergent, {key, localChild.bytes});
    assert(sync.status ==
           roguelite::RunSaveCloudSyncStatus::ConflictPreserved);
    assert(sync.resolution.relation ==
           roguelite::RunSaveCloudRelation::Diverged);
    assert(sync.preservedLocalBytes == localChild.bytes);
    assert(sync.preservedCloudBytes == cloudChild.bytes);

    roguelite::RunSaveCloudSyncRequest preferLocal;
    preferLocal.key = key;
    preferLocal.localEnvelopeBytes = localChild.bytes;
    preferLocal.divergencePolicy =
        roguelite::RunSaveCloudDivergencePolicy::PreferLocal;
    sync = roguelite::SynchronizeRunSaveCloud(
        divergent, std::move(preferLocal));
    assert(sync.status == roguelite::RunSaveCloudSyncStatus::Uploaded);
    fetched = divergent.fetch(key);
    assert(fetched.object.bytes == localChild.bytes);

    const auto untracked = roguelite::RunSaveEnvelopeCodec::build(
        identity(), 9, roguelite::EncodeRunSave(makeSave(90, 9)));
    assert(untracked.error == roguelite::RunSaveEnvelopeError::None);
    const auto missingRevision = divergent.compareExchange(
        "account-1/untracked", 0, untracked.bytes);
    assert(missingRevision.error ==
           roguelite::RunSaveCloudTransportError::MissingRevision);
    assert(divergent.fetch("../escape").error ==
           roguelite::RunSaveCloudTransportError::InvalidKey);
}

void testCompareExchangeRace() {
    constexpr std::uint64_t lineage = 0x2000u;
    constexpr std::uint64_t deviceA = 0xa1u;
    constexpr std::uint64_t deviceB = 0xb2u;
    const std::string key = "account-2/campaign";

    const auto root = buildEnvelope(
        1, 10, 1,
        roguelite::MakeRunSaveCloudRevision(lineage, deviceA));
    const auto rootRevision =
        root.decoded.envelope.manifest.identity.cloud;
    const auto local = buildEnvelope(
        2, 20, 2,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA, {rootRevision}));
    const auto racer = buildEnvelope(
        3, 30, 3,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceB, {rootRevision}));

    RacingTransport transport(key, root.bytes, racer.bytes);
    const auto result = roguelite::SynchronizeRunSaveCloud(
        transport, {key, local.bytes});
    assert(result.status ==
           roguelite::RunSaveCloudSyncStatus::CompareExchangeConflict);
    assert(result.transportError ==
           roguelite::RunSaveCloudTransportError::RevisionMismatch);
    assert(result.remoteAfter.bytes == racer.bytes);
    assert(result.latestResolution.relation ==
           roguelite::RunSaveCloudRelation::Diverged);
}

void writeBytes(
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

void testConflictBranchesAndRetention() {
    constexpr std::uint64_t lineage = 0x3000u;
    constexpr std::uint64_t deviceA = 0xa1u;
    constexpr std::uint64_t deviceB = 0xb2u;
    const auto directory = uniqueDirectory();
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const auto root = buildEnvelope(
        1, 10, 1,
        roguelite::MakeRunSaveCloudRevision(lineage, deviceA));
    const auto rootRevision =
        root.decoded.envelope.manifest.identity.cloud;
    const auto local = buildEnvelope(
        2, 20, 2,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA, {rootRevision}));
    const auto cloud = buildEnvelope(
        3, 30, 3,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceB, {rootRevision}));
    const auto local2 = buildEnvelope(
        4, 40, 4,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA,
            {local.decoded.envelope.manifest.identity.cloud}));
    const auto cloud2 = buildEnvelope(
        5, 50, 5,
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceB,
            {cloud.decoded.envelope.manifest.identity.cloud}));

    const auto firstPayload = roguelite::EncodeRunSave(makeSave(10, 1));
    const auto secondPayload = roguelite::EncodeRunSave(makeSave(20, 2));
    auto firstIdentity = identity(
        roguelite::MakeRunSaveCloudRevision(lineage, deviceA));
    auto firstWrite = roguelite::RunSaveSlotStore::write(
        directory, "campaign", firstIdentity, 1, firstPayload);
    assert(firstWrite.error == roguelite::RunSaveSlotError::None);
    const auto loadedRoot = roguelite::RunSaveSlotStore::load(
        directory, "campaign", firstIdentity);
    assert(loadedRoot.error == roguelite::RunSaveSlotError::None);
    auto secondIdentity = identity(
        roguelite::MakeRunSaveCloudRevision(
            lineage, deviceA,
            {loadedRoot.decoded.envelope.manifest.identity.cloud}));
    const auto secondWrite = roguelite::RunSaveSlotStore::write(
        directory, "campaign", secondIdentity, 2, secondPayload);
    assert(secondWrite.error == roguelite::RunSaveSlotError::None);

    const std::vector<Envelope> branches{local, cloud, local2, cloud2};
    const std::vector<std::string> labels{
        "Local Laptop", "Cloud Desktop", "Local New", "Cloud New"};
    for (std::size_t index = 0; index < branches.size(); ++index) {
        const auto side = index % 2u == 0u
            ? roguelite::RunSaveCloudSide::Local
            : roguelite::RunSaveCloudSide::Cloud;
        const auto preserved = roguelite::PreserveRunSaveCloudBranch(
            directory, "campaign", labels[index], side,
            branches[index].bytes);
        assert(preserved.error ==
               roguelite::RunSaveCloudBranchError::None);
        assert(preserved.branchName.find("--branch--") !=
               std::string::npos);
    }

    const auto repeated = roguelite::PreserveRunSaveCloudBranch(
        directory, "campaign", "Local Laptop",
        roguelite::RunSaveCloudSide::Local, local.bytes);
    assert(repeated.error == roguelite::RunSaveCloudBranchError::None);
    assert(repeated.alreadyPresent);

    writeBytes(
        directory /
            "campaign--branch--broken--0000000000000001.branch.sav",
        {1, 2, 3, 4});
    writeBytes(directory / "campaign.tmp", {5, 6, 7});

    auto inventory = roguelite::InspectRunSaveStorage(directory);
    assert(inventory.primaryCount == 1);
    assert(inventory.recoveryCount == 1);
    assert(inventory.conflictBranchCount == 5);
    assert(inventory.temporaryCount == 1);
    assert(inventory.invalidCount >= 1);

    roguelite::RunSaveStoragePolicy policy;
    policy.maximumConflictBranchesPerSlot = 2;
    auto plan = roguelite::BuildRunSaveStorageRetentionPlan(
        inventory, policy);
    assert(plan.filesToRemove.size() >= 4);
    for (const auto& path : plan.filesToRemove) {
        const auto filename = path.filename().string();
        assert(filename != "campaign.sav");
        assert(filename != "campaign.recovery.sav");
    }
    auto cleanup = roguelite::ApplyRunSaveStorageRetentionPlan(plan);
    assert(cleanup.failures.empty());
    assert(cleanup.removedFiles == cleanup.plannedFiles);

    inventory = roguelite::InspectRunSaveStorage(directory);
    assert(inventory.primaryCount == 1);
    assert(inventory.recoveryCount == 1);
    assert(inventory.conflictBranchCount == 2);
    assert(inventory.temporaryCount == 0);

    policy.maximumConflictBranchesPerSlot = 8;
    policy.maximumConflictBranchBytes = 0;
    plan = roguelite::BuildRunSaveStorageRetentionPlan(
        inventory, policy);
    assert(!plan.filesToRemove.empty());
    cleanup = roguelite::ApplyRunSaveStorageRetentionPlan(plan);
    assert(cleanup.failures.empty());

    inventory = roguelite::InspectRunSaveStorage(directory);
    assert(inventory.conflictBranchCount == 0);
    assert(inventory.primaryCount == 1);
    assert(inventory.recoveryCount == 1);

    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    testTransportCasAndSync();
    testCompareExchangeRace();
    testConflictBranchesAndRetention();
    std::cout << "cloud transport tests passed\n";
    return 0;
}
