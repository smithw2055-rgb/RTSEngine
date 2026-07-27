#pragma once

#include <RTSEngine/Ecs/Entity.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <typeindex>
#include <utility>
#include <vector>

namespace rts::ecs {

template<class T>
class ComponentPool {
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

public:
    bool contains(Entity entity) const noexcept {
        return entity.index < sparse_.size() &&
               sparse_[entity.index] != npos &&
               sparse_[entity.index] < entities_.size() &&
               entities_[sparse_[entity.index]] == entity;
    }

    template<class... Args>
    T& emplace(Entity entity, Args&&... args) {
        ensure(entity.index);
        if (contains(entity)) {
            data_[sparse_[entity.index]] = T(std::forward<Args>(args)...);
            return data_[sparse_[entity.index]];
        }

        // Keep dense entities in canonical Entity order. Structural writes are
        // less frequent than queries in RTS workloads, so paying the insertion
        // shift once removes per-query sorting and makes allocation-free ranges
        // deterministic by construction.
        const auto found = std::lower_bound(
            entities_.begin(), entities_.end(), entity);
        const auto position = static_cast<std::size_t>(
            found - entities_.begin());
        entities_.insert(found, entity);
        data_.insert(
            data_.begin() + static_cast<std::ptrdiff_t>(position),
            T(std::forward<Args>(args)...));
        rebuildSparseFrom(position);
        return data_[position];
    }

    bool remove(Entity entity) {
        if (!contains(entity)) return false;

        const auto position = sparse_[entity.index];
        sparse_[entity.index] = npos;
        entities_.erase(
            entities_.begin() + static_cast<std::ptrdiff_t>(position));
        data_.erase(data_.begin() + static_cast<std::ptrdiff_t>(position));
        rebuildSparseFrom(position);
        return true;
    }

    T* try_get(Entity entity) noexcept {
        return contains(entity) ? &data_[sparse_[entity.index]] : nullptr;
    }

    const T* try_get(Entity entity) const noexcept {
        return contains(entity) ? &data_[sparse_[entity.index]] : nullptr;
    }

    const std::vector<Entity>& entities() const noexcept {
        return entities_;
    }

    std::size_t size() const noexcept {
        return data_.size();
    }

    void reserve(std::size_t count) {
        entities_.reserve(count);
        data_.reserve(count);
    }

private:
    void ensure(std::uint32_t index) {
        if (sparse_.size() <= index) {
            sparse_.resize(static_cast<std::size_t>(index) + 1, npos);
        }
    }

    void rebuildSparseFrom(std::size_t first) noexcept {
        for (std::size_t position = first;
             position < entities_.size(); ++position) {
            sparse_[entities_[position].index] = position;
        }
    }

    std::vector<std::size_t> sparse_;
    std::vector<Entity> entities_;
    std::vector<T> data_;
};

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual std::type_index cppType() const noexcept = 0;
    virtual void remove(Entity entity) = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual std::vector<Entity> orderedEntities() const = 0;
    virtual const void* value(Entity entity) const noexcept = 0;
};

template<class T>
class ComponentPoolModel final : public IComponentPool {
public:
    std::type_index cppType() const noexcept override {
        return std::type_index(typeid(T));
    }

    void remove(Entity entity) override {
        pool.remove(entity);
    }

    std::size_t size() const noexcept override {
        return pool.size();
    }

    std::vector<Entity> orderedEntities() const override {
        return pool.entities();
    }

    const void* value(Entity entity) const noexcept override {
        return pool.try_get(entity);
    }

    ComponentPool<T> pool;
};

} // namespace rts::ecs
