#pragma once

#include <RTSEngine/Ecs/ComponentPool.h>
#include <RTSEngine/Ecs/EntityRegistry.h>

#include <algorithm>
#include <cassert>
#include <memory>
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
        if (!entities_.destroy(entity)) {
            return false;
        }
        for (auto& entry : pools_) {
            entry.second->remove(entity);
        }
        return true;
    }

    bool alive(Entity entity) const noexcept { return entities_.alive(entity); }

    std::uint32_t capacity() const noexcept { return entities_.capacity(); }

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

    template<class First, class... Rest>
    std::vector<Entity> view() const {
        std::vector<Entity> result;
        const auto* first = find_pool<First>();
        if (!first) {
            return result;
        }

        for (Entity entity : first->pool.entities()) {
            if (alive(entity) && (has<Rest>(entity) && ...)) {
                result.push_back(entity);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    friend class WorldArchive;

    template<class T>
    bool has(Entity entity) const {
        const auto* value = find_pool<T>();
        return value && value->pool.contains(entity);
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
