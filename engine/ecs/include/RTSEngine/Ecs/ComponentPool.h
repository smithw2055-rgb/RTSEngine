#pragma once

#include <RTSEngine/Ecs/Entity.h>

#include <cstddef>
#include <limits>
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
               entities_[sparse_[entity.index]] == entity;
    }

    template<class... Args>
    T& emplace(Entity entity, Args&&... args) {
        ensure(entity.index);
        if (contains(entity)) {
            data_[sparse_[entity.index]] = T(std::forward<Args>(args)...);
            return data_[sparse_[entity.index]];
        }

        sparse_[entity.index] = data_.size();
        entities_.push_back(entity);
        data_.emplace_back(std::forward<Args>(args)...);
        return data_.back();
    }

    bool remove(Entity entity) {
        if (!contains(entity)) {
            return false;
        }

        const auto position = sparse_[entity.index];
        const auto last = data_.size() - 1;
        if (position != last) {
            data_[position] = std::move(data_[last]);
            entities_[position] = entities_[last];
            sparse_[entities_[position].index] = position;
        }

        data_.pop_back();
        entities_.pop_back();
        sparse_[entity.index] = npos;
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

private:
    void ensure(std::uint32_t index) {
        if (sparse_.size() <= index) {
            sparse_.resize(static_cast<std::size_t>(index) + 1, npos);
        }
    }

    std::vector<std::size_t> sparse_;
    std::vector<Entity> entities_;
    std::vector<T> data_;
};

} // namespace rts::ecs
