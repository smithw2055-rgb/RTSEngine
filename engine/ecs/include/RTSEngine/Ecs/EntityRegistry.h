#pragma once

#include <RTSEngine/Ecs/Entity.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rts::ecs {

struct EntityRegistryState final {
    std::vector<std::uint32_t> generations;
    std::vector<std::uint8_t> alive;
    std::vector<std::uint32_t> free;
};

class EntityRegistry {
public:
    static constexpr std::uint32_t kMaximumEntities = 1000000u;

    EntityRegistry()
        : generations_(1, 0), alive_(1, 0) {}

    Entity create() {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            alive_[index] = 1;
        } else {
            if (generations_.size() >= kMaximumEntities) return {};
            index = static_cast<std::uint32_t>(generations_.size());
            generations_.push_back(1);
            alive_.push_back(1);
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

        alive_[entity.index] = 0;
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
               alive_[entity.index] != 0 &&
               generations_[entity.index] == entity.generation;
    }

    std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(generations_.size());
    }

    std::uint64_t canonicalHash() const noexcept {
        foundation::CanonicalHash hash;
        hash.WriteString("ecs.entity-registry.v1");
        hash.WriteU32(static_cast<std::uint32_t>(generations_.size()));
        for (std::size_t index = 0; index < generations_.size(); ++index) {
            hash.WriteU32(generations_[index]);
            hash.WriteBool(alive_[index] != 0);
        }
        hash.WriteU32(static_cast<std::uint32_t>(free_.size()));
        for (const auto index : free_) hash.WriteU32(index);
        return hash.Value();
    }

    EntityRegistryState snapshot() const {
        return {generations_, alive_, free_};
    }

    bool restore(const EntityRegistryState& state) {
        if (!validate(state)) {
            return false;
        }
        generations_ = state.generations;
        alive_ = state.alive;
        free_ = state.free;
        return true;
    }

    bool restore(EntityRegistryState&& state) {
        if (!validate(state)) {
            return false;
        }
        generations_ = std::move(state.generations);
        alive_ = std::move(state.alive);
        free_ = std::move(state.free);
        return true;
    }

    bool writeState(foundation::BinaryWriter& writer) const {
        const auto state = snapshot();
        if (!validate(state)) {
            return false;
        }

        writer.writeU32(static_cast<std::uint32_t>(state.generations.size()));
        for (std::size_t index = 0; index < state.generations.size(); ++index) {
            writer.writeU32(state.generations[index]);
            writer.writeBool(state.alive[index] != 0);
        }
        writer.writeU32(static_cast<std::uint32_t>(state.free.size()));
        for (const auto index : state.free) {
            writer.writeU32(index);
        }
        return true;
    }

    static bool readState(foundation::BinaryReader& reader,
                          EntityRegistryState& state,
                          std::uint32_t maximumEntities = kMaximumEntities) {
        std::uint32_t capacity = 0;
        if (!reader.readU32(capacity) || capacity == 0 ||
            capacity > maximumEntities) {
            return false;
        }

        EntityRegistryState candidate;
        candidate.generations.resize(capacity);
        candidate.alive.resize(capacity);
        for (std::uint32_t index = 0; index < capacity; ++index) {
            bool alive = false;
            if (!reader.readU32(candidate.generations[index]) ||
                !reader.readBool(alive)) {
                return false;
            }
            candidate.alive[index] = alive ? 1u : 0u;
        }

        std::uint32_t freeCount = 0;
        if (!reader.readU32(freeCount) || freeCount >= capacity) {
            return false;
        }
        candidate.free.resize(freeCount);
        for (auto& index : candidate.free) {
            if (!reader.readU32(index)) {
                return false;
            }
        }

        if (!validate(candidate, maximumEntities)) {
            return false;
        }
        state = std::move(candidate);
        return true;
    }

    static bool validate(const EntityRegistryState& state,
                         std::uint32_t maximumEntities = kMaximumEntities) {
        const auto capacity = state.generations.size();
        if (capacity == 0 || capacity > maximumEntities ||
            state.alive.size() != capacity ||
            state.free.size() >= capacity ||
            state.generations[0] != 0 || state.alive[0] != 0) {
            return false;
        }

        std::vector<std::uint8_t> seen(capacity, 0);
        for (std::size_t index = 1; index < capacity; ++index) {
            if (state.generations[index] == 0 || state.alive[index] > 1) {
                return false;
            }
        }

        for (const auto index : state.free) {
            if (index == 0 || index >= capacity ||
                state.alive[index] != 0 || seen[index] != 0) {
                return false;
            }
            seen[index] = 1;
        }

        for (std::size_t index = 1; index < capacity; ++index) {
            const bool isAlive = state.alive[index] != 0;
            const bool isFree = seen[index] != 0;
            if (isAlive == isFree) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<std::uint32_t> generations_;
    std::vector<std::uint8_t> alive_;
    std::vector<std::uint32_t> free_;
};

} // namespace rts::ecs
