#pragma once

#include <RTSEngine/Ecs/Scheduler.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rts::ecs {

class EntityCommandBuffer {
public:
    DeferredEntity create(const SystemContext& context) {
        DeferredEntity deferred{++nextDeferred_};
        push(context, [deferred](World& world, ResolvedMap& resolved) {
            resolved[deferred.id] = world.create();
        });
        return deferred;
    }

    void destroy(const SystemContext& context, Entity entity) {
        push(context, [entity](World& world, ResolvedMap&) {
            world.destroy(entity);
        });
    }

    template<class T>
    void add(const SystemContext& context, Entity entity, T value) {
        push(context, [entity, value = std::move(value)](World& world, ResolvedMap&) mutable {
            if (world.alive(entity)) {
                world.emplace<T>(entity, std::move(value));
            }
        });
    }

    template<class T>
    void add(const SystemContext& context, DeferredEntity deferred, T value) {
        push(context, [deferred, value = std::move(value)](World& world, ResolvedMap& resolved) mutable {
            const auto iterator = resolved.find(deferred.id);
            if (iterator != resolved.end()) {
                world.emplace<T>(iterator->second, std::move(value));
            }
        });
    }

    template<class T>
    void remove(const SystemContext& context, Entity entity) {
        push(context, [entity](World& world, ResolvedMap&) {
            world.remove<T>(entity);
        });
    }

    void commit_through(World& world, Stage stage) {
        std::stable_sort(commands_.begin(), commands_.end(), less);

        std::vector<Command> remaining;
        remaining.reserve(commands_.size());
        for (auto& command : commands_) {
            if (command.stage <= stage) {
                command.apply(world, resolved_);
            } else {
                remaining.push_back(std::move(command));
            }
        }
        commands_ = std::move(remaining);
    }

    bool empty() const noexcept { return commands_.empty(); }

private:
    using ResolvedMap = std::unordered_map<std::uint32_t, Entity>;

    struct Command {
        Stage stage;
        std::uint32_t executionOrdinal;
        std::uint32_t sequence;
        std::function<void(World&, ResolvedMap&)> apply;
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
    ResolvedMap resolved_;
    std::uint32_t nextDeferred_{0};
    std::uint32_t nextSequence_{0};
};

} // namespace rts::ecs
