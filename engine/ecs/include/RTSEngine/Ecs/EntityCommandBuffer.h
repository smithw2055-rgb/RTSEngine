#pragma once

#include <RTSEngine/Ecs/Scheduler.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace rts::ecs {

class EntityCommandBuffer {
public:
    static constexpr std::size_t kInlineOperationBytes = 128u;

    void reserve(std::size_t commandCount, std::size_t deferredCount = 0) {
        commands_.reserve(commandCount);
        remaining_.reserve(commandCount);
        resolved_.reserve(deferredCount);
    }

    DeferredEntity create(const SystemContext& context) {
        DeferredEntity deferred{++nextDeferred_};
        push(context, CreateOperation{deferred});
        return deferred;
    }

    void destroy(const SystemContext& context, Entity entity) {
        push(context, DestroyOperation{entity});
    }

    template<class T>
    void add(const SystemContext& context, Entity entity, T value) {
        push(
            context,
            AddEntityOperation<T>{entity, std::move(value)});
    }

    template<class T>
    void add(const SystemContext& context, DeferredEntity deferred, T value) {
        push(
            context,
            AddDeferredOperation<T>{deferred, std::move(value)});
    }

    template<class T>
    void remove(const SystemContext& context, Entity entity) {
        push(context, RemoveOperation<T>{entity});
    }

    void commit_through(World& world, Stage stage) {
        sortIfDirty();

        remaining_.clear();
        if (remaining_.capacity() < commands_.size()) {
            remaining_.reserve(commands_.size());
        }
        for (auto& command : commands_) {
            if (command.stage <= stage) {
                command.apply(world, resolved_);
            } else {
                remaining_.push_back(std::move(command));
            }
        }
        commands_.swap(remaining_);
        dirty_ = false;

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
        dirty_ = false;
    }

    bool empty() const noexcept { return commands_.empty(); }
    std::size_t pendingCount() const noexcept { return commands_.size(); }
    std::size_t resolvedCount() const noexcept { return resolved_.size(); }
    std::size_t commandCapacity() const noexcept { return commands_.capacity(); }

private:
    struct ResolvedEntity final {
        std::uint32_t id{};
        Entity entity{};
    };
    using ResolvedEntities = std::vector<ResolvedEntity>;

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

    struct CreateOperation final {
        DeferredEntity deferred{};

        void apply(World& world, ResolvedEntities& resolved) {
            storeResolved(resolved, deferred.id, world.create());
        }
    };

    struct DestroyOperation final {
        Entity entity{};

        void apply(World& world, ResolvedEntities&) {
            (void)world.destroy(entity);
        }
    };

    template<class T>
    struct AddEntityOperation final {
        Entity entity{};
        T value{};

        void apply(World& world, ResolvedEntities&) {
            if (world.alive(entity)) {
                world.emplace<T>(entity, std::move(value));
            }
        }
    };

    template<class T>
    struct AddDeferredOperation final {
        DeferredEntity deferred{};
        T value{};

        void apply(World& world, ResolvedEntities& resolved) {
            const auto entity = findResolved(resolved, deferred.id);
            if (entity.valid() && world.alive(entity)) {
                world.emplace<T>(entity, std::move(value));
            }
        }
    };

    template<class T>
    struct RemoveOperation final {
        Entity entity{};

        void apply(World& world, ResolvedEntities&) {
            (void)world.remove<T>(entity);
        }
    };

    class Command final {
    public:
        Command() = default;

        template<class Operation>
        Command(Stage valueStage,
                std::uint32_t valueExecutionOrdinal,
                std::uint32_t valueSequence,
                Operation operation)
            : stage(valueStage),
              executionOrdinal(valueExecutionOrdinal),
              sequence(valueSequence) {
            emplaceOperation(std::move(operation));
        }

        Command(const Command&) = delete;
        Command& operator=(const Command&) = delete;

        Command(Command&& other) {
            moveFrom(other);
        }

        Command& operator=(Command&& other) {
            if (this != &other) {
                reset();
                moveFrom(other);
            }
            return *this;
        }

        ~Command() { reset(); }

        void apply(World& world, ResolvedEntities& resolved) {
            if (apply_) apply_(storage_, world, resolved);
        }

        Stage stage{};
        std::uint32_t executionOrdinal{};
        std::uint32_t sequence{};

    private:
        using ApplyFunction = void(*)(
            void*, World&, ResolvedEntities&);
        using MoveFunction = void(*)(void*, void*);
        using DestroyFunction = void(*)(void*) noexcept;

        template<class Operation>
        static void applyOperation(
            void* storage,
            World& world,
            ResolvedEntities& resolved) {
            static_cast<Operation*>(storage)->apply(world, resolved);
        }

        template<class Operation>
        static void moveOperation(void* destination, void* source) {
            auto* value = static_cast<Operation*>(source);
            new (destination) Operation(std::move(*value));
            value->~Operation();
        }

        template<class Operation>
        static void destroyOperation(void* storage) noexcept {
            static_cast<Operation*>(storage)->~Operation();
        }

        template<class Operation>
        void emplaceOperation(Operation operation) {
            static_assert(
                sizeof(Operation) <= kInlineOperationBytes,
                "EntityCommandBuffer operation exceeds inline storage");
            static_assert(
                alignof(Operation) <= alignof(std::max_align_t),
                "EntityCommandBuffer operation requires over-aligned storage");
            new (storage_) Operation(std::move(operation));
            apply_ = &applyOperation<Operation>;
            move_ = &moveOperation<Operation>;
            destroy_ = &destroyOperation<Operation>;
        }

        void moveFrom(Command& other) {
            stage = other.stage;
            executionOrdinal = other.executionOrdinal;
            sequence = other.sequence;
            apply_ = other.apply_;
            move_ = other.move_;
            destroy_ = other.destroy_;
            if (move_) move_(storage_, other.storage_);
            other.apply_ = nullptr;
            other.move_ = nullptr;
            other.destroy_ = nullptr;
        }

        void reset() noexcept {
            if (destroy_) destroy_(storage_);
            apply_ = nullptr;
            move_ = nullptr;
            destroy_ = nullptr;
        }

        alignas(std::max_align_t) unsigned char storage_[
            kInlineOperationBytes]{};
        ApplyFunction apply_{};
        MoveFunction move_{};
        DestroyFunction destroy_{};
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

    void sortIfDirty() {
        if (!dirty_) return;
        std::stable_sort(commands_.begin(), commands_.end(), less);
        dirty_ = false;
    }

    template<class Operation>
    void push(const SystemContext& context, Operation operation) {
        commands_.emplace_back(
            context.stage,
            context.executionOrdinal,
            nextSequence_++,
            std::move(operation));
        dirty_ = true;
    }

    std::vector<Command> commands_;
    std::vector<Command> remaining_;
    ResolvedEntities resolved_;
    std::uint32_t nextDeferred_{0};
    std::uint32_t nextSequence_{0};
    bool dirty_{};
};

} // namespace rts::ecs
