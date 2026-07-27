#pragma once

#include <RTSEngine/Ecs/Scheduler.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace rts::ecs {

class EntityCommandBuffer {
public:
    DeferredEntity create(const SystemContext& context) {
        DeferredEntity deferred{++nextDeferred_};
        push(context, [deferred](World& world, ResolvedEntities& resolved) {
            storeResolved(resolved, deferred.id, world.create());
        });
        return deferred;
    }

    void destroy(const SystemContext& context, Entity entity) {
        push(context, [entity](World& world, ResolvedEntities&) {
            world.destroy(entity);
        });
    }

    template<class T>
    void add(const SystemContext& context, Entity entity, T value) {
        push(context, [entity, value = std::move(value)](
                          World& world, ResolvedEntities&) mutable {
            if (world.alive(entity)) {
                world.emplace<T>(entity, std::move(value));
            }
        });
    }

    template<class T>
    void add(const SystemContext& context, DeferredEntity deferred, T value) {
        push(context, [deferred, value = std::move(value)](
                          World& world, ResolvedEntities& resolved) mutable {
            const auto entity = findResolved(resolved, deferred.id);
            if (entity.valid() && world.alive(entity)) {
                world.emplace<T>(entity, std::move(value));
            }
        });
    }

    template<class T>
    void remove(const SystemContext& context, Entity entity) {
        push(context, [entity](World& world, ResolvedEntities&) {
            world.remove<T>(entity);
        });
    }

    void commit_through(World& world, Stage stage) {
        std::stable_sort(commands_.begin(), commands_.end(), less);

        remaining_.clear();
        remaining_.reserve(commands_.size());
        for (auto& command : commands_) {
            if (command.stage <= stage) {
                command.apply(world, resolved_);
            } else {
                remaining_.push_back(std::move(command));
            }
        }
        commands_.swap(remaining_);

        // Deferred handles are intentionally scoped to one structural batch.
        // Keeping resolved IDs after the final barrier causes unbounded growth
        // during production, construction and wave spawning.
        if (commands_.empty()) {
            resolved_.clear();
            nextDeferred_ = 0;
            nextSequence_ = 0;
        }
    }

    void clear() noexcept {
        commands_.clear();
        remaining_.clear();
        resolved_.clear();
        nextDeferred_ = 0;
        nextSequence_ = 0;
    }

    bool empty() const noexcept { return commands_.empty(); }
    std::size_t pendingCount() const noexcept { return commands_.size(); }
    std::size_t resolvedCount() const noexcept { return resolved_.size(); }

private:
    struct ResolvedEntity final {
        std::uint32_t id{};
        Entity entity{};
    };
    using ResolvedEntities = std::vector<ResolvedEntity>;

    struct Command {
        Stage stage;
        std::uint32_t executionOrdinal;
        std::uint32_t sequence;
        std::function<void(World&, ResolvedEntities&)> apply;
    };

    static bool less(const Command& a, const Command& b) {
        if (a.stage != b.stage) {
            return a.stage < b.stage;
        }
        if (a.executionOrdinal != b.executionOrdinal) {
            return a.executionOrdinal < b.executionOrdinal;
        }
        return a.sequence < b.sequence;
    }

    static void storeResolved(ResolvedEntities& values,
                              std::uint32_t id,
                              Entity entity) {
        const auto found = std::lower_bound(
            values.begin(), values.end(), id,
            [](const ResolvedEntity& value, std::uint32_t key) {
                return value.id < key;
            });
        if (found != values.end() && found->id == id) {
            found->entity = entity;
        } else {
            values.insert(found, {id, entity});
        }
    }

    static Entity findResolved(const ResolvedEntities& values,
                               std::uint32_t id) noexcept {
        const auto found = std::lower_bound(
            values.begin(), values.end(), id,
            [](const ResolvedEntity& value, std::uint32_t key) {
                return value.id < key;
            });
        return found != values.end() && found->id == id
            ? found->entity
            : Entity{};
    }

    template<class Function>
    void push(const SystemContext& context, Function&& function) {
        commands_.push_back({
            context.stage,
            context.executionOrdinal,
            nextSequence_++,
            std::forward<Function>(function)
        });
    }

    std::vector<Command> commands_;
    std::vector<Command> remaining_;
    ResolvedEntities resolved_;
    std::uint32_t nextDeferred_{0};
    std::uint32_t nextSequence_{0};
};

} // namespace rts::ecs
