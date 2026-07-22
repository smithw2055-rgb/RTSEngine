#pragma once

#include <RTSEngine/Roguelite/RunSaveCloudConflict.h>
#include <RTSEngine/Roguelite/RunSaveCloudTransport.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rts::roguelite {

enum class RunSaveCloudSyncStatus : std::uint8_t {
    NoChange,
    Uploaded,
    Downloaded,
    ConflictPreserved,
    Rejected,
    InvalidLocal,
    TransportFailure,
    CompareExchangeConflict
};

struct RunSaveCloudSyncRequest final {
    std::string key;
    std::vector<std::uint8_t> localEnvelopeBytes;
    RunSaveCloudDivergencePolicy divergencePolicy{
        RunSaveCloudDivergencePolicy::PreserveBoth};
};

struct RunSaveCloudSyncResult final {
    RunSaveCloudSyncStatus status{RunSaveCloudSyncStatus::TransportFailure};
    RunSaveCloudTransportError transportError{
        RunSaveCloudTransportError::None};
    RunSaveCloudResolution resolution;
    RunSaveCloudResolution latestResolution;
    RunSaveCloudObject remoteBefore;
    RunSaveCloudObject remoteAfter;
    std::vector<std::uint8_t> selectedEnvelopeBytes;
    std::vector<std::uint8_t> preservedLocalBytes;
    std::vector<std::uint8_t> preservedCloudBytes;
};

inline RunSaveCloudSyncResult SynchronizeRunSaveCloud(
    IRunSaveCloudTransport& transport,
    RunSaveCloudSyncRequest request) {
    RunSaveCloudSyncResult result;

    RunSaveEnvelopeDecodeResult localDecoded;
    const RunSaveEnvelopeDecodeResult* local = nullptr;
    if (!request.localEnvelopeBytes.empty()) {
        localDecoded = RunSaveEnvelopeCodec::decode(
            request.localEnvelopeBytes);
        if (localDecoded.error != RunSaveEnvelopeError::None) {
            result.status = RunSaveCloudSyncStatus::InvalidLocal;
            return result;
        }
        local = &localDecoded;
    }

    const auto fetched = transport.fetch(request.key);
    const RunSaveEnvelopeDecodeResult* cloud = nullptr;
    if (fetched.error == RunSaveCloudTransportError::None) {
        result.remoteBefore = fetched.object;
        cloud = &result.remoteBefore.decoded;
    } else if (fetched.error != RunSaveCloudTransportError::NotFound) {
        result.status = RunSaveCloudSyncStatus::TransportFailure;
        result.transportError = fetched.error;
        return result;
    }

    result.resolution = ResolveRunSaveCloudConflict(
        local, cloud, request.divergencePolicy);

    switch (result.resolution.action) {
    case RunSaveCloudAction::None:
        result.status = RunSaveCloudSyncStatus::NoChange;
        if (local) result.selectedEnvelopeBytes = request.localEnvelopeBytes;
        else if (result.remoteBefore.present) {
            result.selectedEnvelopeBytes = result.remoteBefore.bytes;
        }
        return result;

    case RunSaveCloudAction::DownloadCloud:
    case RunSaveCloudAction::UseCloud:
        if (!result.remoteBefore.present) {
            result.status = RunSaveCloudSyncStatus::TransportFailure;
            result.transportError = RunSaveCloudTransportError::NotFound;
            return result;
        }
        result.status = RunSaveCloudSyncStatus::Downloaded;
        result.selectedEnvelopeBytes = result.remoteBefore.bytes;
        return result;

    case RunSaveCloudAction::PreserveBoth:
        result.status = RunSaveCloudSyncStatus::ConflictPreserved;
        result.preservedLocalBytes = request.localEnvelopeBytes;
        if (result.remoteBefore.present) {
            result.preservedCloudBytes = result.remoteBefore.bytes;
        }
        return result;

    case RunSaveCloudAction::Reject:
        result.status = RunSaveCloudSyncStatus::Rejected;
        return result;

    case RunSaveCloudAction::UploadLocal:
    case RunSaveCloudAction::UseLocal:
        break;
    }

    if (!local) {
        result.status = RunSaveCloudSyncStatus::InvalidLocal;
        return result;
    }

    const auto expectedRevision = result.remoteBefore.present
        ? result.remoteBefore.revisionId()
        : 0u;
    const auto exchanged = transport.compareExchange(
        request.key, expectedRevision, request.localEnvelopeBytes);
    result.transportError = exchanged.error;
    result.remoteAfter = exchanged.current;
    if (!exchanged.applied) {
        if (exchanged.error ==
            RunSaveCloudTransportError::RevisionMismatch) {
            result.status =
                RunSaveCloudSyncStatus::CompareExchangeConflict;
            const RunSaveEnvelopeDecodeResult* latestCloud =
                result.remoteAfter.present
                ? &result.remoteAfter.decoded
                : nullptr;
            result.latestResolution = ResolveRunSaveCloudConflict(
                local, latestCloud, request.divergencePolicy);
        } else {
            result.status = RunSaveCloudSyncStatus::TransportFailure;
        }
        return result;
    }

    result.status = RunSaveCloudSyncStatus::Uploaded;
    result.selectedEnvelopeBytes = request.localEnvelopeBytes;
    return result;
}

} // namespace rts::roguelite
