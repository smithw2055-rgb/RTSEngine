#pragma once

#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <cstdint>
#include <string_view>

namespace rts::roguelite {

enum class RunSaveCloudRelation : std::uint8_t {
    BothMissing,
    LocalOnly,
    CloudOnly,
    IdenticalRevision,
    EquivalentUntracked,
    LocalDescendant,
    CloudDescendant,
    Diverged,
    AncestryUnavailable,
    UnrelatedLineage,
    IncompatibleManifest,
    LocalInvalid,
    CloudInvalid,
    BothInvalid
};

enum class RunSaveCloudAction : std::uint8_t {
    None,
    UploadLocal,
    DownloadCloud,
    PreserveBoth,
    UseLocal,
    UseCloud,
    Reject
};

enum class RunSaveCloudSide : std::uint8_t {
    None,
    Local,
    Cloud
};

enum class RunSaveCloudDivergencePolicy : std::uint8_t {
    PreserveBoth,
    PreferLocal,
    PreferCloud,
    PreferDeterministicLatest
};

struct RunSaveCloudResolution final {
    RunSaveCloudRelation relation{RunSaveCloudRelation::BothMissing};
    RunSaveCloudAction action{RunSaveCloudAction::None};
    RunSaveCloudSide selected{RunSaveCloudSide::None};
    bool conflict{};
};

inline constexpr std::string_view RunSaveCloudRelationName(
    RunSaveCloudRelation value) noexcept {
    switch (value) {
    case RunSaveCloudRelation::BothMissing: return "cloud.both_missing";
    case RunSaveCloudRelation::LocalOnly: return "cloud.local_only";
    case RunSaveCloudRelation::CloudOnly: return "cloud.cloud_only";
    case RunSaveCloudRelation::IdenticalRevision:
        return "cloud.identical_revision";
    case RunSaveCloudRelation::EquivalentUntracked:
        return "cloud.equivalent_untracked";
    case RunSaveCloudRelation::LocalDescendant:
        return "cloud.local_descendant";
    case RunSaveCloudRelation::CloudDescendant:
        return "cloud.cloud_descendant";
    case RunSaveCloudRelation::Diverged: return "cloud.diverged";
    case RunSaveCloudRelation::AncestryUnavailable:
        return "cloud.ancestry_unavailable";
    case RunSaveCloudRelation::UnrelatedLineage:
        return "cloud.unrelated_lineage";
    case RunSaveCloudRelation::IncompatibleManifest:
        return "cloud.incompatible_manifest";
    case RunSaveCloudRelation::LocalInvalid: return "cloud.local_invalid";
    case RunSaveCloudRelation::CloudInvalid: return "cloud.cloud_invalid";
    case RunSaveCloudRelation::BothInvalid: return "cloud.both_invalid";
    }
    return "cloud.unknown";
}

inline constexpr std::string_view RunSaveCloudActionName(
    RunSaveCloudAction value) noexcept {
    switch (value) {
    case RunSaveCloudAction::None: return "none";
    case RunSaveCloudAction::UploadLocal: return "upload_local";
    case RunSaveCloudAction::DownloadCloud: return "download_cloud";
    case RunSaveCloudAction::PreserveBoth: return "preserve_both";
    case RunSaveCloudAction::UseLocal: return "use_local";
    case RunSaveCloudAction::UseCloud: return "use_cloud";
    case RunSaveCloudAction::Reject: return "reject";
    }
    return "unknown";
}

inline bool RunSaveCloudCandidateValid(
    const RunSaveEnvelopeDecodeResult* value) noexcept {
    return value && value->error == RunSaveEnvelopeError::None;
}

inline bool RunSaveCloudManifestCompatible(
    const RunSaveEnvelopeDecodeResult& a,
    const RunSaveEnvelopeDecodeResult& b) noexcept {
    return a.envelope.manifest.identity.productId ==
               b.envelope.manifest.identity.productId &&
           a.envelope.manifest.identity.contentManifestId ==
               b.envelope.manifest.identity.contentManifestId;
}

inline int CompareRunSaveCloudLatest(
    const RunSaveEnvelopeDecodeResult& local,
    const RunSaveEnvelopeDecodeResult& cloud) noexcept {
    const auto& localManifest = local.envelope.manifest;
    const auto& cloudManifest = cloud.envelope.manifest;
    const auto localTotal = RunSaveCloudClockTotal(
        localManifest.identity.cloud);
    const auto cloudTotal = RunSaveCloudClockTotal(
        cloudManifest.identity.cloud);
    if (localTotal != cloudTotal) return localTotal > cloudTotal ? 1 : -1;
    if (localManifest.saveTick != cloudManifest.saveTick) {
        return localManifest.saveTick > cloudManifest.saveTick ? 1 : -1;
    }
    if (localManifest.sequence != cloudManifest.sequence) {
        return localManifest.sequence > cloudManifest.sequence ? 1 : -1;
    }
    if (localManifest.identity.cloud.logicalClock !=
        cloudManifest.identity.cloud.logicalClock) {
        return localManifest.identity.cloud.logicalClock >
                       cloudManifest.identity.cloud.logicalClock
            ? 1
            : -1;
    }
    if (localManifest.identity.cloud.revisionId !=
        cloudManifest.identity.cloud.revisionId) {
        return localManifest.identity.cloud.revisionId >
                       cloudManifest.identity.cloud.revisionId
            ? 1
            : -1;
    }
    if (localManifest.identity.cloud.deviceId !=
        cloudManifest.identity.cloud.deviceId) {
        return localManifest.identity.cloud.deviceId >
                       cloudManifest.identity.cloud.deviceId
            ? 1
            : -1;
    }
    return 0;
}

inline RunSaveCloudResolution ResolveRunSaveCloudConflict(
    const RunSaveEnvelopeDecodeResult* local,
    const RunSaveEnvelopeDecodeResult* cloud,
    RunSaveCloudDivergencePolicy policy =
        RunSaveCloudDivergencePolicy::PreserveBoth) noexcept {
    RunSaveCloudResolution result;
    if (!local && !cloud) return result;
    if (!local) {
        result.relation = RunSaveCloudRelation::CloudOnly;
        result.action = RunSaveCloudAction::DownloadCloud;
        result.selected = RunSaveCloudSide::Cloud;
        return result;
    }
    if (!cloud) {
        result.relation = RunSaveCloudRelation::LocalOnly;
        result.action = RunSaveCloudAction::UploadLocal;
        result.selected = RunSaveCloudSide::Local;
        return result;
    }

    const bool localValid = RunSaveCloudCandidateValid(local);
    const bool cloudValid = RunSaveCloudCandidateValid(cloud);
    if (!localValid && !cloudValid) {
        result.relation = RunSaveCloudRelation::BothInvalid;
        result.action = RunSaveCloudAction::Reject;
        result.conflict = true;
        return result;
    }
    if (!localValid) {
        result.relation = RunSaveCloudRelation::LocalInvalid;
        result.action = RunSaveCloudAction::DownloadCloud;
        result.selected = RunSaveCloudSide::Cloud;
        return result;
    }
    if (!cloudValid) {
        result.relation = RunSaveCloudRelation::CloudInvalid;
        result.action = RunSaveCloudAction::UploadLocal;
        result.selected = RunSaveCloudSide::Local;
        return result;
    }
    if (!RunSaveCloudManifestCompatible(*local, *cloud)) {
        result.relation = RunSaveCloudRelation::IncompatibleManifest;
        result.action = RunSaveCloudAction::Reject;
        result.conflict = true;
        return result;
    }

    const auto& localRevision = local->envelope.manifest.identity.cloud;
    const auto& cloudRevision = cloud->envelope.manifest.identity.cloud;
    if (!localRevision.tracked() || !cloudRevision.tracked()) {
        if (local->envelope.payload == cloud->envelope.payload) {
            result.relation = RunSaveCloudRelation::EquivalentUntracked;
            result.action = RunSaveCloudAction::None;
        } else {
            result.relation = RunSaveCloudRelation::AncestryUnavailable;
            result.action = RunSaveCloudAction::PreserveBoth;
            result.conflict = true;
        }
        return result;
    }
    if (localRevision.lineageId != cloudRevision.lineageId) {
        result.relation = RunSaveCloudRelation::UnrelatedLineage;
        result.action = RunSaveCloudAction::PreserveBoth;
        result.conflict = true;
        return result;
    }
    if (localRevision.revisionId == cloudRevision.revisionId) {
        result.relation = RunSaveCloudRelation::IdenticalRevision;
        result.action = RunSaveCloudAction::None;
        return result;
    }
    if (RunSaveCloudClockDominates(localRevision, cloudRevision)) {
        result.relation = RunSaveCloudRelation::LocalDescendant;
        result.action = RunSaveCloudAction::UploadLocal;
        result.selected = RunSaveCloudSide::Local;
        return result;
    }
    if (RunSaveCloudClockDominates(cloudRevision, localRevision)) {
        result.relation = RunSaveCloudRelation::CloudDescendant;
        result.action = RunSaveCloudAction::DownloadCloud;
        result.selected = RunSaveCloudSide::Cloud;
        return result;
    }

    result.relation = RunSaveCloudRelation::Diverged;
    result.conflict = true;
    switch (policy) {
    case RunSaveCloudDivergencePolicy::PreserveBoth:
        result.action = RunSaveCloudAction::PreserveBoth;
        break;
    case RunSaveCloudDivergencePolicy::PreferLocal:
        result.action = RunSaveCloudAction::UseLocal;
        result.selected = RunSaveCloudSide::Local;
        break;
    case RunSaveCloudDivergencePolicy::PreferCloud:
        result.action = RunSaveCloudAction::UseCloud;
        result.selected = RunSaveCloudSide::Cloud;
        break;
    case RunSaveCloudDivergencePolicy::PreferDeterministicLatest:
        if (CompareRunSaveCloudLatest(*local, *cloud) >= 0) {
            result.action = RunSaveCloudAction::UseLocal;
            result.selected = RunSaveCloudSide::Local;
        } else {
            result.action = RunSaveCloudAction::UseCloud;
            result.selected = RunSaveCloudSide::Cloud;
        }
        break;
    }
    return result;
}

} // namespace rts::roguelite
