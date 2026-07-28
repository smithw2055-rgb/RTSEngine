#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Harvesting.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/TechTree.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct AiEconomyPlan final {
    std::uint32_t teamId{};
    std::uint32_t thinkIntervalTicks{8};
    ResourceTypeId resourceType{kPrimaryResourceType};
    std::uint32_t workerDefinitionId{};
    std::uint32_t minimumWorkers{};
    std::uint32_t supplyBuildingDefinitionId{};
    std::uint32_t supplyBuffer{2};
    ResearchDefinitionId preferredResearchId{};
    GridPoint buildAnchor{};
    std::uint32_t nextBuildOrdinal{};
};

class AiEconomyPlanner final {
public:
    bool registerPlan(AiEconomyPlan plan) {
        if (!valid(plan)) return false;
        const auto found = lowerBound(plan.teamId);
        if (found != plans_.end() && found->teamId == plan.teamId) {
            return false;
        }
        plans_.insert(found, std::move(plan));
        return true;
    }

    bool restore(std::vector<AiEconomyPlan> plans) {
        std::sort(
            plans.begin(), plans.end(),
            [](const AiEconomyPlan& first, const AiEconomyPlan& second) {
                return first.teamId < second.teamId;
            });
        std::uint32_t previous = 0;
        for (const auto& plan : plans) {
            if (!valid(plan) || plan.teamId <= previous) return false;
            previous = plan.teamId;
        }
        plans_ = std::move(plans);
        return true;
    }

    const std::vector<AiEconomyPlan>& plans() const noexcept {
        return plans_;
    }

    template<class Sequence,
             class Submit,
             class AllowsUnit,
             class AllowsResearch,
             class UsedSupply,
             class SupplyCapacity>
    void emitCommands(
        const ecs::World& world,
        const NavigationGrid& navigation,
        const TechTreeRuntime& tech,
        std::uint64_t tick,
        Sequence&& sequence,
        Submit&& submit,
        AllowsUnit&& allowsUnit,
        AllowsResearch&& allowsResearch,
        UsedSupply&& usedSupply,
        SupplyCapacity&& supplyCapacity) {
        auto&& nextSequence = sequence;
        auto&& submitCommand = submit;
        auto&& unitAllowed = allowsUnit;
        auto&& researchAllowed = allowsResearch;
        auto&& used = usedSupply;
        auto&& capacity = supplyCapacity;

        for (auto& plan : plans_) {
            if (tick % plan.thinkIntervalTicks != 0) continue;
            assignIdleWorkers(
                world, tick, plan, nextSequence, submitCommand);
            requestSupply(
                world,
                navigation,
                tick,
                plan,
                nextSequence,
                submitCommand,
                used,
                capacity);
            requestWorker(
                world,
                tick,
                plan,
                nextSequence,
                submitCommand,
                unitAllowed);
            requestResearch(
                world,
                tech,
                tick,
                plan,
                nextSequence,
                submitCommand,
                researchAllowed);
        }
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(plans_.size()));
        for (const auto& plan : plans_) {
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

private:
    using Iterator = std::vector<AiEconomyPlan>::iterator;

    static bool valid(const AiEconomyPlan& plan) noexcept {
        return plan.teamId != 0 && plan.thinkIntervalTicks != 0 &&
               plan.resourceType != 0;
    }

    Iterator lowerBound(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            plans_.begin(), plans_.end(), teamId,
            [](const AiEconomyPlan& plan, std::uint32_t value) {
                return plan.teamId < value;
            });
    }

    static std::int32_t distance(
        Position first,
        Position second) noexcept {
        const auto dx = first.x > second.x
            ? first.x - second.x
            : second.x - first.x;
        const auto dy = first.y > second.y
            ? first.y - second.y
            : second.y - first.y;
        return dx + dy;
    }

    template<class Sequence, class Submit>
    static void assignIdleWorkers(
        const ecs::World& world,
        std::uint64_t tick,
        const AiEconomyPlan& plan,
        Sequence& sequence,
        Submit& submit) {
        std::vector<std::pair<ecs::Entity, Position>> nodes;
        world.eachRef<Position, ResourceNode>(
            [&](ecs::Entity entity,
                const Position& position,
                const ResourceNode& node) {
                if (node.resourceType == plan.resourceType &&
                    node.remaining > 0) {
                    nodes.push_back({entity, position});
                }
            });
        std::sort(
            nodes.begin(), nodes.end(),
            [](const auto& first, const auto& second) {
                return first.first < second.first;
            });
        if (nodes.empty()) return;

        world.eachRef<Position, Team, WorkerHarvester>(
            [&](ecs::Entity entity,
                const Position& position,
                const Team& team,
                const WorkerHarvester& worker) {
                if (team.id != plan.teamId ||
                    worker.state != HarvestState::Idle ||
                    worker.targetNode.valid()) {
                    return;
                }
                ecs::Entity best{};
                auto bestDistance =
                    std::numeric_limits<std::int32_t>::max();
                for (const auto& node : nodes) {
                    const auto candidate = distance(position, node.second);
                    if (candidate < bestDistance ||
                        (candidate == bestDistance &&
                         (!best.valid() || node.first < best))) {
                        best = node.first;
                        bestDistance = candidate;
                    }
                }
                const auto id = sequence(plan.teamId);
                if (!best.valid() || id == 0) return;
                TickCommand command;
                command.targetTick = tick;
                command.issuer = plan.teamId;
                command.sequence = id;
                command.type = CommandType::Gather;
                command.subject = entity;
                command.targetEntity = best;
                submit(std::move(command));
            });
    }

    template<class Sequence, class Submit, class AllowsUnit>
    static void requestWorker(
        const ecs::World& world,
        std::uint64_t tick,
        const AiEconomyPlan& plan,
        Sequence& sequence,
        Submit& submit,
        AllowsUnit& allowsUnit) {
        if (plan.workerDefinitionId == 0 || plan.minimumWorkers == 0) return;
        std::uint32_t workers = 0;
        world.eachRef<Team, UnitArchetype>(
            [&](ecs::Entity,
                const Team& team,
                const UnitArchetype& archetype) {
                if (team.id == plan.teamId &&
                    archetype.definitionId == plan.workerDefinitionId &&
                    workers != std::numeric_limits<std::uint32_t>::max()) {
                    ++workers;
                }
            });
        world.eachRef<Team, ProductionQueue>(
            [&](ecs::Entity,
                const Team& team,
                const ProductionQueue& queue) {
                if (team.id != plan.teamId) return;
                for (const auto& item : queue.items) {
                    if (item.unitDefinitionId == plan.workerDefinitionId &&
                        workers != std::numeric_limits<std::uint32_t>::max()) {
                        ++workers;
                    }
                }
            });
        if (workers >= plan.minimumWorkers) return;

        ecs::Entity producer{};
        world.eachRef<Team, Building, ProductionQueue>(
            [&](ecs::Entity entity,
                const Team& team,
                const Building& building,
                const ProductionQueue&) {
                if (team.id == plan.teamId && building.producer &&
                    allowsUnit(
                        building.definitionId,
                        plan.workerDefinitionId) &&
                    (!producer.valid() || entity < producer)) {
                    producer = entity;
                }
            });
        const auto id = sequence(plan.teamId);
        if (!producer.valid() || id == 0) return;
        TickCommand command;
        command.targetTick = tick;
        command.issuer = plan.teamId;
        command.sequence = id;
        command.type = CommandType::Train;
        command.subject = producer;
        command.definitionId = plan.workerDefinitionId;
        submit(std::move(command));
    }

    template<class Sequence,
             class Submit,
             class UsedSupply,
             class SupplyCapacity>
    static void requestSupply(
        const ecs::World& world,
        const NavigationGrid& navigation,
        std::uint64_t tick,
        AiEconomyPlan& plan,
        Sequence& sequence,
        Submit& submit,
        UsedSupply& usedSupply,
        SupplyCapacity& supplyCapacity) {
        if (plan.supplyBuildingDefinitionId == 0) return;
        const auto used = usedSupply(plan.teamId);
        const auto capacity = supplyCapacity(plan.teamId);
        if (capacity == std::numeric_limits<std::uint32_t>::max() ||
            capacity > used + plan.supplyBuffer) {
            return;
        }
        bool pending = false;
        world.eachRef<ConstructionSite>(
            [&](ecs::Entity, const ConstructionSite& site) {
                pending = pending ||
                    (site.ownerTeam == plan.teamId &&
                     site.definitionId == plan.supplyBuildingDefinitionId);
            });
        if (pending) return;

        const auto width = navigation.width();
        const auto height = navigation.height();
        const auto cellCount = static_cast<std::uint64_t>(width) *
                               static_cast<std::uint64_t>(height);
        if (cellCount == 0) return;
        const auto anchorX = std::clamp(plan.buildAnchor.x, 0, width - 1);
        const auto anchorY = std::clamp(plan.buildAnchor.y, 0, height - 1);
        const auto anchor = static_cast<std::uint64_t>(anchorY) * width +
                            static_cast<std::uint64_t>(anchorX);

        GridPoint point{};
        bool found = false;
        for (std::uint64_t attempt = 0; attempt < cellCount; ++attempt) {
            const auto ordinal =
                (anchor + plan.nextBuildOrdinal + attempt) % cellCount;
            const GridPoint candidate{
                static_cast<std::int32_t>(ordinal % width),
                static_cast<std::int32_t>(ordinal / width)};
            if (navigation.blocked(candidate)) continue;
            bool resourceNode = false;
            world.eachRef<Position, ResourceNode>(
                [&](ecs::Entity,
                    const Position& position,
                    const ResourceNode&) {
                    resourceNode = resourceNode ||
                        (position.x == candidate.x &&
                         position.y == candidate.y);
                });
            if (!resourceNode) {
                point = candidate;
                plan.nextBuildOrdinal = static_cast<std::uint32_t>(
                    (ordinal + 1u) % cellCount);
                found = true;
                break;
            }
        }
        const auto id = sequence(plan.teamId);
        if (!found || id == 0) return;
        TickCommand command;
        command.targetTick = tick;
        command.issuer = plan.teamId;
        command.sequence = id;
        command.type = CommandType::Build;
        command.definitionId = plan.supplyBuildingDefinitionId;
        command.targetX = point.x;
        command.targetY = point.y;
        submit(std::move(command));
    }

    template<class Sequence, class Submit, class AllowsResearch>
    static void requestResearch(
        const ecs::World& world,
        const TechTreeRuntime& tech,
        std::uint64_t tick,
        const AiEconomyPlan& plan,
        Sequence& sequence,
        Submit& submit,
        AllowsResearch& allowsResearch) {
        if (plan.preferredResearchId == 0 ||
            tech.completed(plan.teamId, plan.preferredResearchId)) {
            return;
        }
        bool queued = false;
        world.eachRef<Team, ResearchQueue>(
            [&](ecs::Entity,
                const Team& team,
                const ResearchQueue& queue) {
                if (team.id != plan.teamId) return;
                queued = queued || std::any_of(
                    queue.items.begin(), queue.items.end(),
                    [&](const ResearchItem& item) {
                        return item.researchDefinitionId ==
                               plan.preferredResearchId;
                    });
            });
        if (queued) return;

        ecs::Entity facility{};
        world.eachRef<Team, Building>(
            [&](ecs::Entity entity,
                const Team& team,
                const Building& building) {
                if (team.id == plan.teamId &&
                    allowsResearch(
                        building.definitionId,
                        plan.preferredResearchId) &&
                    (!facility.valid() || entity < facility)) {
                    facility = entity;
                }
            });
        const auto id = sequence(plan.teamId);
        if (!facility.valid() || id == 0) return;
        TickCommand command;
        command.targetTick = tick;
        command.issuer = plan.teamId;
        command.sequence = id;
        command.type = CommandType::Research;
        command.subject = facility;
        command.definitionId = plan.preferredResearchId;
        submit(std::move(command));
    }

    std::vector<AiEconomyPlan> plans_;
};

} // namespace rts::gameplay
