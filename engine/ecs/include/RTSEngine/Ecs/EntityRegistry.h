#pragma once

#include <RTSEngine/Ecs/Entity.h>

#include <cstdint>
#include <vector>

namespace rts::ecs {

class EntityRegistry {
public:
    EntityRegistry()
        : generations_(1, 0), alive_(1, false) {}

    Entity create() {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            alive_[index] = true;
        } else {
            index = static_cast<std::uint32_t>(generations_.size());
            generations_.push_back(1);
            alive_.push_back(true);
        }

        if (generations_[index] == 0) {
            generations_[index] = 1;
        }
        return {index, generations_[index]};
    }

    bool destroy(Entity entity) {
        if (!alive(entity)) {
            return false;
        }

        alive_[entity.index] = false;
        auto& generation = generations_[entity.index];
        ++generation;
        if (generation == 0) {
            ++generation;
        }
        free_.push_back(entity.index);
        return true;
    }

    bool alive(Entity entity) const noexcept {
        return entity.valid() &&
               entity.index < generations_.size() &&
               alive_[entity.index] &&
               generations_[entity.index] == entity.generation;
    }

    std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(generations_.size());
    }

private:
    std::vector<std::uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<std::uint32_t> free_;
};

} // namespace rts::ecs
