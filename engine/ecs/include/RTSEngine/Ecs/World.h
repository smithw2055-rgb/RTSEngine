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

class World {
    struct IPool {
        virtual ~IPool() = default;
        virtual void remove(Entity entity) = 0;
    };

    template<class T>
    struct PoolModel final : IPool {
        ComponentPool<T> pool;
        void remove(Entity entity) override { pool.remove(entity); }
    };

public:
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
    template<class T>
    bool has(Entity entity) const {
        const auto* value = find_pool<T>();
        return value && value->pool.contains(entity);
    }

    template<class T>
    PoolModel<T>& pool() {
        const auto key = std::type_index(typeid(T));
        auto iterator = pools_.find(key);
        if (iterator == pools_.end()) {
            iterator = pools_.emplace(key, std::make_unique<PoolModel<T>>()).first;
        }
        return *static_cast<PoolModel<T>*>(iterator->second.get());
    }

    template<class T>
    PoolModel<T>* find_pool() {
        auto iterator = pools_.find(std::type_index(typeid(T)));
        return iterator == pools_.end()
            ? nullptr
            : static_cast<PoolModel<T>*>(iterator->second.get());
    }

    template<class T>
    const PoolModel<T>* find_pool() const {
        auto iterator = pools_.find(std::type_index(typeid(T)));
        return iterator == pools_.end()
            ? nullptr
            : static_cast<const PoolModel<T>*>(iterator->second.get());
    }

    EntityRegistry entities_;
    std::unordered_map<std::type_index, std::unique_ptr<IPool>> pools_;
};

} // namespace rts::ecs
