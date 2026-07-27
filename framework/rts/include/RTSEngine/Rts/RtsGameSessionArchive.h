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
    static constexpr std::uint32_t kMagic = 0x31534752u;
    static constexpr std::uint16_t kVersion = 2u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint32_t kMaximumNestedBytes =
        160u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumEntries = 4096u;
    static constexpr std::uint32_t kMaximumAllowedDefinitions = 65536u;
    static constexpr std::uint32_t kMaximumAllowedUnits =
        kMaximumAllowedDefinitions;

    static std::uint64_t authoritativeHash(
        const RtsGameSession& session) {
        foundation::CanonicalHash hash;
        hash.WriteU64(
            RtsSimulationArchive::authoritativeHash(session.simulation_));
        appendRulesHash(
            hash,
            session.diplomacy_.entries(),
            session.ai_.teams(),
            session.producers_,
            session.supply_,
            session.researchPolicies_,
            session.economyAi_.plans(),
            kVersion);
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
            session.supply_.size() > kMaximumEntries ||
            session.researchPolicies_.size() > kMaximumEntries ||
            session.economyAi_.plans().size() > kMaximumEntries) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(rulesHash(session, kVersion));
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
            if (policy.allowedUnitDefinitions.size() >
                kMaximumAllowedDefinitions) {
                return {};
            }
            writer.writeU32(policy.buildingDefinitionId);
            writer.writeU32(policy.queueCapacity);
            writer.writeU32(static_cast<std::uint32_t>(
                policy.allowedUnitDefinitions.size()));
            for (const auto id : policy.allowedUnitDefinitions) {
                writer.writeU32(id);
            }
        }

        writer.writeU32(static_cast<std::uint32_t>(session.supply_.size()));
        for (const auto& supply : session.supply_) {
            writer.writeU32(supply.teamId);
            writer.writeU32(supply.capacity);
        }

        writer.writeU32(static_cast<std::uint32_t>(
            session.researchPolicies_.size()));
        for (const auto& policy : session.researchPolicies_) {
            if (policy.allowedResearchDefinitions.size() >
                kMaximumAllowedDefinitions) {
                return {};
            }
            writer.writeU32(policy.buildingDefinitionId);
            writer.writeU32(policy.queueCapacity);
            writer.writeU32(static_cast<std::uint32_t>(
                policy.allowedResearchDefinitions.size()));
            for (const auto id : policy.allowedResearchDefinitions) {
                writer.writeU32(id);
            }
        }

        writer.writeU32(static_cast<std::uint32_t>(
            session.economyAi_.plans().size()));
        for (const auto& plan : session.economyAi_.plans()) {
            writeEconomyPlan(writer, plan);
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
            !reader.readU64(storedRulesHash) || magic != kMagic ||
            version < kMinimumVersion || version > kVersion ||
            !reader.readU32(nestedByteCount) || nestedByteCount == 0 ||
            nestedByteCount > kMaximumNestedBytes ||
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
        std::vector<ResearchPolicy> researchPolicies;
        std::vector<AiEconomyPlan> economyPlans;
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

        if (!readProducerPolicies(reader, producers) ||
            !reader.readU32(count) || count > kMaximumEntries) {
            return false;
        }
        supply.resize(count);
        for (auto& entry : supply) {
            if (!reader.readU32(entry.teamId) ||
                !reader.readU32(entry.capacity)) {
                return false;
            }
        }

        if (version >= 2u) {
            if (!readResearchPolicies(reader, researchPolicies) ||
                !reader.readU32(count) || count > kMaximumEntries) {
                return false;
            }
            economyPlans.resize(count);
            for (auto& plan : economyPlans) {
                if (!readEconomyPlan(reader, plan)) return false;
            }
        }
        if (!reader.atEnd()) return false;

        DiplomacyRuntime diplomacyCandidate;
        AiRuntime aiCandidate;
        AiEconomyPlanner economyAiCandidate;
        if (!diplomacyCandidate.restore(relations) ||
            !aiCandidate.restore(aiTeams) ||
            !validateProducers(producers) ||
            !validateSupply(supply) ||
            !validateResearchPolicies(researchPolicies) ||
            !validateEconomyPlans(economyPlans, aiTeams) ||
            !economyAiCandidate.restore(economyPlans) ||
            rulesHash(
                diplomacyCandidate.entries(),
                aiCandidate.teams(),
                producers,
                supply,
                researchPolicies,
                economyAiCandidate.plans(),
                version) != storedRulesHash) {
            return false;
        }

        if (!RtsSimulationArchive::decode(
                simulationBytes, session.simulation_)) {
            return false;
        }

        session.diplomacy_ = std::move(diplomacyCandidate);
        session.ai_ = std::move(aiCandidate);
        session.economyAi_ = std::move(economyAiCandidate);
        session.producers_ = std::move(producers);
        session.supply_ = std::move(supply);
        session.researchPolicies_ = std::move(researchPolicies);
        rebuildReservations(session);
        return true;
    }

private:
    static bool readDefinitionIds(
        foundation::BinaryReader& reader,
        std::vector<std::uint32_t>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > kMaximumAllowedDefinitions) {
            return false;
        }
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU32(value)) return false;
        }
        return true;
    }

    static bool readProducerPolicies(
        foundation::BinaryReader& reader,
        std::vector<ProducerPolicy>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& policy : values) {
            if (!reader.readU32(policy.buildingDefinitionId) ||
                !reader.readU32(policy.queueCapacity) ||
                !readDefinitionIds(
                    reader, policy.allowedUnitDefinitions)) {
                return false;
            }
        }
        return true;
    }

    static bool readResearchPolicies(
        foundation::BinaryReader& reader,
        std::vector<ResearchPolicy>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& policy : values) {
            if (!reader.readU32(policy.buildingDefinitionId) ||
                !reader.readU32(policy.queueCapacity) ||
                !readDefinitionIds(
                    reader, policy.allowedResearchDefinitions)) {
                return false;
            }
        }
        return true;
    }

    static void writeEconomyPlan(
        foundation::BinaryWriter& writer,
        const AiEconomyPlan& plan) {
        writer.writeU32(plan.teamId);
        writer.writeU32(plan.thinkIntervalTicks);
        writer.writeU32(plan.resourceType);
        writer.writeU32(plan.workerDefinitionId);
        writer.writeU32(plan.minimumWorkers);
        writer.writeU32(plan.supplyBuildingDefinitionId);
        writer.writeU32(plan.supplyBuffer);
        writer.writeU32(plan.preferredResearchId);
        writer.writeI32(plan.buildAnchor.x);
        writer.writeI32(plan.buildAnchor.y);
        writer.writeU32(plan.nextBuildOrdinal);
    }

    static bool readEconomyPlan(
        foundation::BinaryReader& reader,
        AiEconomyPlan& plan) {
        return reader.readU32(plan.teamId) &&
               reader.readU32(plan.thinkIntervalTicks) &&
               reader.readU32(plan.resourceType) &&
               reader.readU32(plan.workerDefinitionId) &&
               reader.readU32(plan.minimumWorkers) &&
               reader.readU32(plan.supplyBuildingDefinitionId) &&
               reader.readU32(plan.supplyBuffer) &&
               reader.readU32(plan.preferredResearchId) &&
               reader.readI32(plan.buildAnchor.x) &&
               reader.readI32(plan.buildAnchor.y) &&
               reader.readU32(plan.nextBuildOrdinal);
    }

    template<class Policy, class Ids>
    static bool validatePolicyList(
        const std::vector<Policy>& policies,
        Ids ids) {
        std::uint32_t previousBuilding = 0;
        bool hasPreviousBuilding = false;
        for (const auto& policy : policies) {
            const auto& values = ids(policy);
            if (policy.buildingDefinitionId == 0 ||
                policy.queueCapacity == 0 ||
                (hasPreviousBuilding &&
                 policy.buildingDefinitionId <= previousBuilding)) {
                return false;
            }
            std::uint32_t previous = 0;
            bool hasPrevious = false;
            for (const auto id : values) {
                if (id == 0 || (hasPrevious && id <= previous)) {
                    return false;
                }
                previous = id;
                hasPrevious = true;
            }
            previousBuilding = policy.buildingDefinitionId;
            hasPreviousBuilding = true;
        }
        return true;
    }

    static bool validateProducers(
        const std::vector<ProducerPolicy>& producers) {
        return validatePolicyList(
            producers,
            [](const ProducerPolicy& policy) -> const auto& {
                return policy.allowedUnitDefinitions;
            });
    }

    static bool validateResearchPolicies(
        const std::vector<ResearchPolicy>& policies) {
        return validatePolicyList(
            policies,
            [](const ResearchPolicy& policy) -> const auto& {
                return policy.allowedResearchDefinitions;
            });
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

    static bool validateEconomyPlans(
        const std::vector<AiEconomyPlan>& plans,
        const std::vector<AiTeamState>& aiTeams) {
        std::uint32_t previousTeam = 0;
        for (const auto& plan : plans) {
            const auto team = std::lower_bound(
                aiTeams.begin(), aiTeams.end(), plan.teamId,
                [](const AiTeamState& value, std::uint32_t id) {
                    return value.teamId < id;
                });
            if (plan.teamId == 0 || plan.teamId <= previousTeam ||
                plan.thinkIntervalTicks == 0 || plan.resourceType == 0 ||
                team == aiTeams.end() || team->teamId != plan.teamId) {
                return false;
            }
            previousTeam = plan.teamId;
        }
        return true;
    }

    static void rebuildReservations(RtsGameSession& session) {
        session.trainReservations_.clear();
        session.researchReservations_.clear();
        const auto commandState = session.simulation_.commandStreamState();
        session.trainReservations_.reserve(commandState.pending.size());
        session.researchReservations_.reserve(commandState.pending.size());
        for (const auto& command : commandState.pending) {
            if (command.type == CommandType::Train) {
                session.trainReservations_.push_back(
                    {command.targetTick,
                     command.issuer,
                     command.sequence,
                     command.subject,
                     command.definitionId});
            } else if (command.type == CommandType::Research) {
                session.researchReservations_.push_back(
                    {command.targetTick,
                     command.issuer,
                     command.sequence,
                     command.subject,
                     command.definitionId});
            }
        }
        std::sort(
            session.trainReservations_.begin(),
            session.trainReservations_.end(),
            RtsGameSession::trainReservationLess);
        std::sort(
            session.researchReservations_.begin(),
            session.researchReservations_.end(),
            RtsGameSession::researchReservationLess);
    }

    static std::uint64_t rulesHash(
        const RtsGameSession& session,
        std::uint16_t version) {
        return rulesHash(
            session.diplomacy_.entries(),
            session.ai_.teams(),
            session.producers_,
            session.supply_,
            session.researchPolicies_,
            session.economyAi_.plans(),
            version);
    }

    static std::uint64_t rulesHash(
        const std::vector<DiplomaticRelationEntry>& relations,
        const std::vector<AiTeamState>& aiTeams,
        const std::vector<ProducerPolicy>& producers,
        const std::vector<TeamSupplyLimit>& supply,
        const std::vector<ResearchPolicy>& researchPolicies,
        const std::vector<AiEconomyPlan>& economyPlans,
        std::uint16_t version) {
        foundation::CanonicalHash hash;
        appendRulesHash(
            hash,
            relations,
            aiTeams,
            producers,
            supply,
            researchPolicies,
            economyPlans,
            version);
        return hash.Value();
    }

    static void appendRulesHash(
        foundation::CanonicalHash& hash,
        const std::vector<DiplomaticRelationEntry>& relations,
        const std::vector<AiTeamState>& aiTeams,
        const std::vector<ProducerPolicy>& producers,
        const std::vector<TeamSupplyLimit>& supply,
        const std::vector<ResearchPolicy>& researchPolicies,
        const std::vector<AiEconomyPlan>& economyPlans,
        std::uint16_t version) {
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
            for (const auto id : policy.allowedUnitDefinitions) {
                hash.WriteU32(id);
            }
        }

        hash.WriteU32(static_cast<std::uint32_t>(supply.size()));
        for (const auto& entry : supply) {
            hash.WriteU32(entry.teamId);
            hash.WriteU32(entry.capacity);
        }

        if (version >= 2u) {
            hash.WriteU32(static_cast<std::uint32_t>(
                researchPolicies.size()));
            for (const auto& policy : researchPolicies) {
                hash.WriteU32(policy.buildingDefinitionId);
                hash.WriteU32(policy.queueCapacity);
                hash.WriteU32(static_cast<std::uint32_t>(
                    policy.allowedResearchDefinitions.size()));
                for (const auto id : policy.allowedResearchDefinitions) {
                    hash.WriteU32(id);
                }
            }
            hash.WriteU32(static_cast<std::uint32_t>(
                economyPlans.size()));
            for (const auto& plan : economyPlans) {
                hash.WriteU32(plan.teamId);
                hash.WriteU32(plan.thinkIntervalTicks);
                hash.WriteU32(plan.resourceType);
                hash.WriteU32(plan.workerDefinitionId);
                hash.WriteU32(plan.minimumWorkers);
                hash.WriteU32(plan.supplyBuildingDefinitionId);
                hash.WriteU32(plan.supplyBuffer);
                hash.WriteU32(plan.preferredResearchId);
                hash.WriteI32(plan.buildAnchor.x);
                hash.WriteI32(plan.buildAnchor.y);
                hash.WriteU32(plan.nextBuildOrdinal);
            }
        }
    }
};

} // namespace rts::gameplay
