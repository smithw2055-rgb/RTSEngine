#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/GameplayModifiers.h>
#include <RTSEngine/Rts/TeamEconomy.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

using ResearchDefinitionId = std::uint32_t;
using ResearchQueueId = std::uint32_t;

struct ResourceCost final {
    ResourceTypeId resourceType{};
    ResourceAmount amount{};
};

struct PrerequisiteSet final {
    std::vector<ResearchDefinitionId> research;
    std::vector<std::uint32_t> buildings;
};

struct TeamModifierDelta final {
    std::int32_t unitHealth{};
    std::int32_t unitDamage{};
    std::int32_t unitArmorAdd{};
    std::int32_t unitMoveSpeed{};
    std::int32_t buildingHealth{};
    std::int32_t buildingDamage{};
    std::int32_t constructionSpeed{};
    std::int32_t productionSpeed{};
    std::int32_t bountyMultiplier{};
};

struct ResearchDefinition final {
    ResearchDefinitionId id{};
    std::vector<ResourceCost> costs;
    std::uint32_t researchTicks{1};
    PrerequisiteSet prerequisites;
    TeamModifierDelta modifiers;
};

struct DefinitionPrerequisiteEntry final {
    std::uint32_t definitionId{};
    PrerequisiteSet prerequisites;
};

struct ResearchItem final {
    ResearchQueueId id{};
    ResearchDefinitionId researchDefinitionId{};
    std::vector<ResourceCost> reservedCosts;
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{1};
    std::uint32_t baseRequiredTicks{1};
};

struct ResearchQueue final {
    std::vector<ResearchItem> items;
};

struct TeamTechState final {
    std::uint32_t teamId{};
    std::vector<ResearchDefinitionId> completed;
};

inline std::int32_t AddModifierValue(
    std::int32_t current,
    std::int32_t delta) noexcept {
    const auto value = static_cast<std::int64_t>(current) + delta;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

inline TeamModifierProfile ApplyTeamModifierDelta(
    TeamModifierProfile profile,
    const TeamModifierDelta& delta) noexcept {
    profile.unitHealth = AddModifierValue(profile.unitHealth, delta.unitHealth);
    profile.unitDamage = AddModifierValue(profile.unitDamage, delta.unitDamage);
    profile.unitArmorAdd = AddModifierValue(
        profile.unitArmorAdd, delta.unitArmorAdd);
    profile.unitMoveSpeed = AddModifierValue(
        profile.unitMoveSpeed, delta.unitMoveSpeed);
    profile.buildingHealth = AddModifierValue(
        profile.buildingHealth, delta.buildingHealth);
    profile.buildingDamage = AddModifierValue(
        profile.buildingDamage, delta.buildingDamage);
    profile.constructionSpeed = AddModifierValue(
        profile.constructionSpeed, delta.constructionSpeed);
    profile.productionSpeed = AddModifierValue(
        profile.productionSpeed, delta.productionSpeed);
    profile.bountyMultiplier = AddModifierValue(
        profile.bountyMultiplier, delta.bountyMultiplier);
    return SanitizeTeamModifierProfile(profile);
}

inline void NormalizePrerequisites(PrerequisiteSet& value) {
    std::sort(value.research.begin(), value.research.end());
    value.research.erase(
        std::unique(value.research.begin(), value.research.end()),
        value.research.end());
    std::sort(value.buildings.begin(), value.buildings.end());
    value.buildings.erase(
        std::unique(value.buildings.begin(), value.buildings.end()),
        value.buildings.end());
}

inline bool ValidatePrerequisites(const PrerequisiteSet& value) noexcept {
    return std::adjacent_find(
               value.research.begin(), value.research.end(),
               std::greater_equal<ResearchDefinitionId>()) ==
               value.research.end() &&
           std::all_of(
               value.research.begin(), value.research.end(),
               [](ResearchDefinitionId id) { return id != 0; }) &&
           std::adjacent_find(
               value.buildings.begin(), value.buildings.end(),
               std::greater_equal<std::uint32_t>()) ==
               value.buildings.end() &&
           std::all_of(
               value.buildings.begin(), value.buildings.end(),
               [](std::uint32_t id) { return id != 0; });
}

inline void NormalizeResourceCosts(std::vector<ResourceCost>& costs) {
    std::sort(
        costs.begin(), costs.end(),
        [](const ResourceCost& first, const ResourceCost& second) {
            return first.resourceType < second.resourceType;
        });
    std::vector<ResourceCost> normalized;
    normalized.reserve(costs.size());
    for (const auto& cost : costs) {
        if (!normalized.empty() &&
            normalized.back().resourceType == cost.resourceType) {
            const auto remaining =
                std::numeric_limits<ResourceAmount>::max() -
                normalized.back().amount;
            normalized.back().amount += std::min(cost.amount, remaining);
        } else {
            normalized.push_back(cost);
        }
    }
    costs = std::move(normalized);
}

inline bool ValidateResourceCosts(
    const std::vector<ResourceCost>& costs) noexcept {
    ResourceTypeId previous = 0;
    for (const auto& cost : costs) {
        if (cost.resourceType == 0 || cost.amount < 0 ||
            cost.resourceType <= previous) {
            return false;
        }
        previous = cost.resourceType;
    }
    return true;
}

class PrerequisiteCatalog final {
public:
    bool set(std::uint32_t definitionId, PrerequisiteSet prerequisites) {
        if (definitionId == 0) return false;
        NormalizePrerequisites(prerequisites);
        if (!ValidatePrerequisites(prerequisites)) return false;
        const auto found = lowerBound(definitionId);
        if (found != entries_.end() && found->definitionId == definitionId) {
            found->prerequisites = std::move(prerequisites);
        } else {
            entries_.insert(
                found,
                DefinitionPrerequisiteEntry{
                    definitionId, std::move(prerequisites)});
        }
        return true;
    }

    const PrerequisiteSet* find(std::uint32_t definitionId) const noexcept {
        const auto found = lowerBound(definitionId);
        return found != entries_.end() && found->definitionId == definitionId
            ? &found->prerequisites
            : nullptr;
    }

    bool restore(std::vector<DefinitionPrerequisiteEntry> entries) {
        std::uint32_t previous = 0;
        for (const auto& entry : entries) {
            if (entry.definitionId == 0 || entry.definitionId <= previous ||
                !ValidatePrerequisites(entry.prerequisites)) {
                return false;
            }
            previous = entry.definitionId;
        }
        entries_ = std::move(entries);
        return true;
    }

    const std::vector<DefinitionPrerequisiteEntry>& entries() const noexcept {
        return entries_;
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(entries_.size()));
        for (const auto& entry : entries_) {
            hash.WriteU32(entry.definitionId);
            appendPrerequisites(hash, entry.prerequisites);
        }
    }

    static void appendPrerequisites(
        foundation::CanonicalHash& hash,
        const PrerequisiteSet& value) noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(value.research.size()));
        for (const auto id : value.research) hash.WriteU32(id);
        hash.WriteU32(static_cast<std::uint32_t>(value.buildings.size()));
        for (const auto id : value.buildings) hash.WriteU32(id);
    }

private:
    using Iterator = std::vector<DefinitionPrerequisiteEntry>::iterator;
    using ConstIterator =
        std::vector<DefinitionPrerequisiteEntry>::const_iterator;

    Iterator lowerBound(std::uint32_t id) noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(), id,
            [](const DefinitionPrerequisiteEntry& entry, std::uint32_t value) {
                return entry.definitionId < value;
            });
    }

    ConstIterator lowerBound(std::uint32_t id) const noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(), id,
            [](const DefinitionPrerequisiteEntry& entry, std::uint32_t value) {
                return entry.definitionId < value;
            });
    }

    std::vector<DefinitionPrerequisiteEntry> entries_;
};

class TechTreeRuntime final {
public:
    bool completed(
        std::uint32_t teamId,
        ResearchDefinitionId researchId) const noexcept {
        const auto* state = find(teamId);
        return state && std::binary_search(
            state->completed.begin(), state->completed.end(), researchId);
    }

    bool unlock(
        std::uint32_t teamId,
        ResearchDefinitionId researchId) {
        if (teamId == 0 || researchId == 0) return false;
        auto& state = ensure(teamId);
        const auto found = std::lower_bound(
            state.completed.begin(), state.completed.end(), researchId);
        if (found != state.completed.end() && *found == researchId) {
            return false;
        }
        state.completed.insert(found, researchId);
        return true;
    }

    bool meets(
        const ecs::World& world,
        std::uint32_t teamId,
        const PrerequisiteSet& prerequisites) const {
        if (teamId == 0) return false;
        for (const auto researchId : prerequisites.research) {
            if (!completed(teamId, researchId)) return false;
        }
        for (const auto buildingId : prerequisites.buildings) {
            bool found = false;
            world.eachRef<Team, Building>(
                [&](ecs::Entity,
                    const Team& team,
                    const Building& building) {
                    found = found ||
                        (team.id == teamId &&
                         building.definitionId == buildingId);
                });
            if (!found) return false;
        }
        return true;
    }

    bool restore(std::vector<TeamTechState> states) {
        std::uint32_t previousTeam = 0;
        for (const auto& state : states) {
            if (state.teamId == 0 || state.teamId <= previousTeam ||
                std::adjacent_find(
                    state.completed.begin(), state.completed.end(),
                    std::greater_equal<ResearchDefinitionId>()) !=
                    state.completed.end() ||
                std::any_of(
                    state.completed.begin(), state.completed.end(),
                    [](ResearchDefinitionId id) { return id == 0; })) {
                return false;
            }
            previousTeam = state.teamId;
        }
        states_ = std::move(states);
        return true;
    }

    const std::vector<TeamTechState>& states() const noexcept {
        return states_;
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(states_.size()));
        for (const auto& state : states_) {
            hash.WriteU32(state.teamId);
            hash.WriteU32(static_cast<std::uint32_t>(
                state.completed.size()));
            for (const auto id : state.completed) hash.WriteU32(id);
        }
    }

private:
    using Iterator = std::vector<TeamTechState>::iterator;
    using ConstIterator = std::vector<TeamTechState>::const_iterator;

    Iterator lowerBound(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            states_.begin(), states_.end(), teamId,
            [](const TeamTechState& state, std::uint32_t value) {
                return state.teamId < value;
            });
    }

    ConstIterator lowerBound(std::uint32_t teamId) const noexcept {
        return std::lower_bound(
            states_.begin(), states_.end(), teamId,
            [](const TeamTechState& state, std::uint32_t value) {
                return state.teamId < value;
            });
    }

    const TeamTechState* find(std::uint32_t teamId) const noexcept {
        const auto found = lowerBound(teamId);
        return found != states_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    TeamTechState& ensure(std::uint32_t teamId) {
        const auto found = lowerBound(teamId);
        if (found != states_.end() && found->teamId == teamId) {
            return *found;
        }
        return *states_.insert(found, TeamTechState{teamId, {}});
    }

    std::vector<TeamTechState> states_;
};

} // namespace rts::gameplay
