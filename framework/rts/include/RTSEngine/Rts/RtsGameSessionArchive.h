#pragma once

#include <RTSEngine/Rts/RtsGameSession.h>
#include <RTSEngine/Rts/SimulationArchive.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::gameplay {

class RtsGameSessionArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x31534752u; // "RGS1"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumNestedBytes =
        160u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumEntries = 4096u;
    static constexpr std::uint32_t kMaximumAllowedUnits = 65536u;

    static std::uint64_t authoritativeHash(
        const RtsGameSession& session) {
        foundation::CanonicalHash hash;
        hash.WriteU64(session.simulation_.snapshot().worldHash);
        appendRulesHash(
            hash,
            session.diplomacy_.entries(),
            session.ai_.teams(),
            session.producers_,
            session.supply_);
        return hash.Value();
    }

    static std::vector<std::uint8_t> encode(
        const RtsGameSession& session) {
        const auto simulationBytes =
            RtsSimulationArchive::encode(session.simulation_);
        if (simulationBytes.empty() ||
            simulationBytes.size() > kMaximumNestedBytes ||
            session.diplomacy_.entries().size() > kMaximumEntries ||
            session.ai_.teams().size() > kMaximumEntries ||
            session.producers_.size() > kMaximumEntries ||
            session.supply_.size() > kMaximumEntries) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(rulesHash(session));
        writer.writeU32(static_cast<std::uint32_t>(simulationBytes.size()));
        writer.writeBytes(simulationBytes);

        writer.writeU32(static_cast<std::uint32_t>(
            session.diplomacy_.entries().size()));
        for (const auto& entry : session.diplomacy_.entries()) {
            writer.writeU32(entry.firstTeam);
            writer.writeU32(entry.secondTeam);
            writer.writeU8(static_cast<std::uint8_t>(entry.relation));
        }

        writer.writeU32(static_cast<std::uint32_t>(
            session.ai_.teams().size()));
        for (const auto& team : session.ai_.teams()) {
            writer.writeU32(team.teamId);
            writer.writeU32(team.nextSequence);
            writer.writeU32(team.thinkIntervalTicks);
            writer.writeI32(team.fallbackObjective.x);
            writer.writeI32(team.fallbackObjective.y);
        }

        writer.writeU32(static_cast<std::uint32_t>(
            session.producers_.size()));
        for (const auto& policy : session.producers_) {
            if (policy.allowedUnitDefinitions.size() > kMaximumAllowedUnits) {
                return {};
            }
            writer.writeU32(policy.buildingDefinitionId);
            writer.writeU32(policy.queueCapacity);
            writer.writeU32(static_cast<std::uint32_t>(
                policy.allowedUnitDefinitions.size()));
            for (const auto definitionId : policy.allowedUnitDefinitions) {
                writer.writeU32(definitionId);
            }
        }

        writer.writeU32(static_cast<std::uint32_t>(session.supply_.size()));
        for (const auto& supply : session.supply_) {
            writer.writeU32(supply.teamId);
            writer.writeU32(supply.capacity);
        }
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        RtsGameSession& session) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedRulesHash = 0;
        std::uint32_t nestedByteCount = 0;
        std::vector<std::uint8_t> simulationBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedRulesHash) ||
            magic != kMagic || version != kVersion ||
            !reader.readU32(nestedByteCount) ||
            nestedByteCount == 0 || nestedByteCount > kMaximumNestedBytes ||
            !reader.readBytes(
                nestedByteCount,
                simulationBytes,
                kMaximumNestedBytes)) {
            return false;
        }

        std::vector<DiplomaticRelationEntry> relations;
        std::vector<AiTeamState> aiTeams;
        std::vector<ProducerPolicy> producers;
        std::vector<TeamSupplyLimit> supply;
        std::uint32_t count = 0;

        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        relations.resize(count);
        for (auto& entry : relations) {
            std::uint8_t rawRelation = 0;
            if (!reader.readU32(entry.firstTeam) ||
                !reader.readU32(entry.secondTeam) ||
                !reader.readU8(rawRelation) ||
                rawRelation > static_cast<std::uint8_t>(
                    DiplomaticRelation::Hostile)) {
                return false;
            }
            entry.relation = static_cast<DiplomaticRelation>(rawRelation);
        }

        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        aiTeams.resize(count);
        for (auto& team : aiTeams) {
            if (!reader.readU32(team.teamId) ||
                !reader.readU32(team.nextSequence) ||
                !reader.readU32(team.thinkIntervalTicks) ||
                !reader.readI32(team.fallbackObjective.x) ||
                !reader.readI32(team.fallbackObjective.y)) {
                return false;
            }
        }

        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        producers.resize(count);
        for (auto& policy : producers) {
            std::uint32_t allowedCount = 0;
            if (!reader.readU32(policy.buildingDefinitionId) ||
                !reader.readU32(policy.queueCapacity) ||
                !reader.readU32(allowedCount) ||
                allowedCount > kMaximumAllowedUnits) {
                return false;
            }
            policy.allowedUnitDefinitions.resize(allowedCount);
            for (auto& definitionId : policy.allowedUnitDefinitions) {
                if (!reader.readU32(definitionId)) return false;
            }
        }

        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        supply.resize(count);
        for (auto& entry : supply) {
            if (!reader.readU32(entry.teamId) ||
                !reader.readU32(entry.capacity)) {
                return false;
            }
        }
        if (!reader.atEnd()) return false;

        DiplomacyRuntime diplomacyCandidate;
        AiRuntime aiCandidate;
        if (!diplomacyCandidate.restore(relations) ||
            !aiCandidate.restore(aiTeams) ||
            !validateProducers(producers) ||
            !validateSupply(supply) ||
            rulesHash(relations, aiTeams, producers, supply) !=
                storedRulesHash) {
            return false;
        }

        if (!RtsSimulationArchive::decode(
                simulationBytes, session.simulation_)) {
            return false;
        }

        session.diplomacy_ = std::move(diplomacyCandidate);
        session.ai_ = std::move(aiCandidate);
        session.producers_ = std::move(producers);
        session.supply_ = std::move(supply);
        rebuildReservations(session);
        return true;
    }

private:
    static bool validateProducers(
        const std::vector<ProducerPolicy>& producers) {
        std::uint32_t previousBuilding = 0;
        bool hasPreviousBuilding = false;
        for (const auto& policy : producers) {
            if (policy.buildingDefinitionId == 0 ||
                policy.queueCapacity == 0 ||
                (hasPreviousBuilding &&
                 policy.buildingDefinitionId <= previousBuilding)) {
                return false;
            }
            std::uint32_t previousUnit = 0;
            bool hasPreviousUnit = false;
            for (const auto definitionId : policy.allowedUnitDefinitions) {
                if (definitionId == 0 ||
                    (hasPreviousUnit && definitionId <= previousUnit)) {
                    return false;
                }
                previousUnit = definitionId;
                hasPreviousUnit = true;
            }
            previousBuilding = policy.buildingDefinitionId;
            hasPreviousBuilding = true;
        }
        return true;
    }

    static bool validateSupply(
        const std::vector<TeamSupplyLimit>& supply) {
        std::uint32_t previousTeam = 0;
        bool hasPrevious = false;
        for (const auto& entry : supply) {
            if (entry.teamId == 0 ||
                (hasPrevious && entry.teamId <= previousTeam)) {
                return false;
            }
            previousTeam = entry.teamId;
            hasPrevious = true;
        }
        return true;
    }

    static void rebuildReservations(RtsGameSession& session) {
        session.reservations_.clear();
        const auto commandState = session.simulation_.commandStreamState();
        session.reservations_.reserve(commandState.pending.size());
        for (const auto& command : commandState.pending) {
            if (command.type != CommandType::Train) continue;
            session.reservations_.push_back(
                {command.targetTick,
                 command.issuer,
                 command.sequence,
                 command.subject,
                 command.definitionId});
        }
        std::sort(
            session.reservations_.begin(),
            session.reservations_.end(),
            RtsGameSession::reservationLess);
    }

    static std::uint64_t rulesHash(const RtsGameSession& session) {
        return rulesHash(
            session.diplomacy_.entries(),
            session.ai_.teams(),
            session.producers_,
            session.supply_);
    }

    static std::uint64_t rulesHash(
        const std::vector<DiplomaticRelationEntry>& relations,
        const std::vector<AiTeamState>& aiTeams,
        const std::vector<ProducerPolicy>& producers,
        const std::vector<TeamSupplyLimit>& supply) {
        foundation::CanonicalHash hash;
        appendRulesHash(hash, relations, aiTeams, producers, supply);
        return hash.Value();
    }

    static void appendRulesHash(
        foundation::CanonicalHash& hash,
        const std::vector<DiplomaticRelationEntry>& relations,
        const std::vector<AiTeamState>& aiTeams,
        const std::vector<ProducerPolicy>& producers,
        const std::vector<TeamSupplyLimit>& supply) {
        hash.WriteU32(static_cast<std::uint32_t>(relations.size()));
        for (const auto& relation : relations) {
            hash.WriteU32(relation.firstTeam);
            hash.WriteU32(relation.secondTeam);
            hash.WriteU8(static_cast<std::uint8_t>(relation.relation));
        }

        hash.WriteU32(static_cast<std::uint32_t>(aiTeams.size()));
        for (const auto& team : aiTeams) {
            hash.WriteU32(team.teamId);
            hash.WriteU32(team.nextSequence);
            hash.WriteU32(team.thinkIntervalTicks);
            hash.WriteI32(team.fallbackObjective.x);
            hash.WriteI32(team.fallbackObjective.y);
        }

        hash.WriteU32(static_cast<std::uint32_t>(producers.size()));
        for (const auto& policy : producers) {
            hash.WriteU32(policy.buildingDefinitionId);
            hash.WriteU32(policy.queueCapacity);
            hash.WriteU32(static_cast<std::uint32_t>(
                policy.allowedUnitDefinitions.size()));
            for (const auto definitionId : policy.allowedUnitDefinitions) {
                hash.WriteU32(definitionId);
            }
        }

        hash.WriteU32(static_cast<std::uint32_t>(supply.size()));
        for (const auto& entry : supply) {
            hash.WriteU32(entry.teamId);
            hash.WriteU32(entry.capacity);
        }
    }
};

} // namespace rts::gameplay
