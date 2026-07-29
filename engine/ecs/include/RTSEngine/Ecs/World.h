#pragma once

#include <RTSEngine/Ecs/ComponentPool.h>
#include <RTSEngine/Ecs/EntityRegistry.h>

#include <cassert>
#include <memory>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rts::ecs {

class WorldArchive;

class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;

    Entity create() { return entities_.create(); }

    bool destroy(Entity entity) {
        if (!entities_.destroy(entity)) return false;
        for (auto& entry : pools_) {
            entry.second->remove(entity);
        }
        return true;
    }

    bool alive(Entity entity) const noexcept { return entities_.alive(entity); }

    std::uint32_t capacity() const noexcept { return entities_.capacity(); }

    std::uint64_t entityRegistryHash() const noexcept {
        return entities_.canonicalHash();
    }

    template<class T, class... Args>
    T& emplace(Entity entity, Args&&... args) {
        assert(alive(entity));
        return pool<T>().pool.emplace(entity, std::forward<Args>(args)...);
    }

    template<class T>
    bool remove(Entity entity) {
        auto* value = find_pool<T>();
        return value ? value->pool.remove(entity) : false;
    }

    template<class T>
    T* try_get(Entity entity) {
        auto* value = find_pool<T>();
        return value ? value->pool.try_get(entity) : nullptr;
    }

    template<class T>
    const T* try_get(Entity entity) const {
        auto* value = find_pool<T>();
        return value ? value->pool.try_get(entity) : nullptr;
    }

    template<class T>
    void reserve(std::size_t count) {
        pool<T>().pool.reserve(count);
    }

    // Allocation-free deterministic entity query. The smallest component pool
    // is used as the driving range. This compatibility form only exposes the
    // Entity and is retained for callers that do not need component references.
    template<class... Components, class Function>
    void each(Function&& function) {
        const auto* driving = smallestPool<Components...>();
        if (!driving) return;
        auto&& callback = function;
        for (const auto entity : driving->entities()) {
            if (alive(entity) && (has<Components>(entity) && ...)) {
                callback(entity);
            }
        }
    }

    template<class... Components, class Function>
    void each(Function&& function) const {
        const auto* driving = smallestPool<Components...>();
        if (!driving) return;
        auto&& callback = function;
        for (const auto entity : driving->entities()) {
            if (alive(entity) && (has<Components>(entity) && ...)) {
                callback(entity);
            }
        }
    }

    // Direct component-reference query for warmed Tick paths. Component pool
    // pointers are resolved once at query construction, not once per entity.
    // The callback signature is:
    //   void(Entity, Components&...)
    template<class... Components, class Function>
    void eachRef(Function&& function) {
        auto pools = std::make_tuple(find_pool<Components>()...);
        constexpr auto indices = std::index_sequence_for<Components...>{};
        if (!allPoolsPresent(pools, indices)) return;
        const auto* driving = smallestPool<Components...>();
        if (!driving) return;

        auto&& callback = function;
        for (const auto entity : driving->entities()) {
            if (!alive(entity)) continue;
            invokeMutableReferences(callback, entity, pools, indices);
        }
    }

    // Const overload exposes const component references:
    //   void(Entity, const Components&...)
    template<class... Components, class Function>
    void eachRef(Function&& function) const {
        auto pools = std::make_tuple(find_pool<Components>()...);
        constexpr auto indices = std::index_sequence_for<Components...>{};
        if (!allPoolsPresent(pools, indices)) return;
        const auto* driving = smallestPool<Components...>();
        if (!driving) return;

        auto&& callback = function;
        for (const auto entity : driving->entities()) {
            if (!alive(entity)) continue;
            invokeConstReferences(callback, entity, pools, indices);
        }
    }

    template<class... Components>
    void viewInto(std::vector<Entity>& result) const {
        result.clear();
        const auto* driving = smallestPool<Components...>();
        if (!driving) return;
        if (result.capacity() < driving->size()) {
            result.reserve(driving->size());
        }
        for (const auto entity : driving->entities()) {
            if (alive(entity) && (has<Components>(entity) && ...)) {
                result.push_back(entity);
            }
        }
    }

    template<class First, class... Rest>
    std::vector<Entity> view() const {
        std::vector<Entity> result;
        viewInto<First, Rest...>(result);
        return result;
    }

private:
    friend class WorldArchive;

    template<class T>
    bool has(Entity entity) const {
        const auto* value = find_pool<T>();
        return value && value->pool.contains(entity);
    }

    template<class Tuple, std::size_t... Indices>
    static bool allPoolsPresent(
        const Tuple& pools,
        std::index_sequence<Indices...>) noexcept {
        return ((std::get<Indices>(pools) != nullptr) && ...);
    }

    template<class Function, class Tuple, std::size_t... Indices>
    static void invokeMutableReferences(
        Function& callback,
        Entity entity,
        Tuple& pools,
        std::index_sequence<Indices...>) {
        auto values = std::make_tuple(
            std::get<Indices>(pools)->pool.try_get(entity)...);
        if (((std::get<Indices>(values) != nullptr) && ...)) {
            callback(entity, *std::get<Indices>(values)...);
        }
    }

    template<class Function, class Tuple, std::size_t... Indices>
    static void invokeConstReferences(
        Function& callback,
        Entity entity,
        const Tuple& pools,
        std::index_sequence<Indices...>) {
        auto values = std::make_tuple(
            std::get<Indices>(pools)->pool.try_get(entity)...);
        if (((std::get<Indices>(values) != nullptr) && ...)) {
            callback(entity, *std::get<Indices>(values)...);
        }
    }

    template<class... Components>
    const IComponentPool* smallestPool() const noexcept {
        const IComponentPool* candidates[] = {
            static_cast<const IComponentPool*>(find_pool<Components>())...
        };
        const IComponentPool* result = nullptr;
        for (const auto* candidate : candidates) {
            if (!candidate) return nullptr;
            if (!result || candidate->size() < result->size()) {
                result = candidate;
            }
        }
        return result;
    }

    template<class T>
    ComponentPoolModel<T>& pool() {
        const auto key = std::type_index(typeid(T));
        auto iterator = pools_.find(key);
        if (iterator == pools_.end()) {
            iterator = pools_.emplace(
                key, std::make_unique<ComponentPoolModel<T>>()).first;
        }
        return *static_cast<ComponentPoolModel<T>*>(iterator->second.get());
    }

    template<class T>
    ComponentPoolModel<T>* find_pool() {
        auto iterator = pools_.find(std::type_index(typeid(T)));
        return iterator == pools_.end()
            ? nullptr
            : static_cast<ComponentPoolModel<T>*>(iterator->second.get());
    }

    template<class T>
    const ComponentPoolModel<T>* find_pool() const {
        auto iterator = pools_.find(std::type_index(typeid(T)));
        return iterator == pools_.end()
            ? nullptr
            : static_cast<const ComponentPoolModel<T>*>(iterator->second.get());
    }

    EntityRegistry entities_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools_;
};

} // namespace rts::ecs
