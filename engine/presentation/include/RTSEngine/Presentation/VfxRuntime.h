#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::presentation {

using VfxDefinitionId = std::uint64_t;
using VfxEventId = std::uint64_t;
using VfxSpriteId = std::uint64_t;

struct VfxDefinition final {
    VfxDefinitionId id{};
    VfxSpriteId spriteId{};
    std::uint32_t durationMilliseconds{500};
    std::uint32_t particleLifetimeMilliseconds{500};
    std::uint16_t maximumParticles{32};
    std::uint16_t burstParticles{8};
    float speed{1.0f};
    float spreadRadians{6.28318530718f};
    float gravity{};
    float startSize{1.0f};
    float endSize{};
    bool additive{};
};

struct VfxCue final {
    VfxEventId eventId{};
    VfxDefinitionId definitionId{};
    float x{};
    float y{};
    float directionRadians{};
};

struct VfxParticleView final {
    VfxEventId eventId{};
    VfxSpriteId spriteId{};
    float x{};
    float y{};
    float size{1.0f};
    float opacity{1.0f};
    bool additive{};
};

class VfxRuntime final {
public:
    bool registerDefinition(VfxDefinition definition) {
        if (!valid(definition)) return false;
        const auto iterator = std::lower_bound(
            definitions_.begin(), definitions_.end(), definition.id,
            [](const VfxDefinition& value, VfxDefinitionId id) {
                return value.id < id;
            });
        if (iterator != definitions_.end() && iterator->id == definition.id) {
            *iterator = definition;
        } else {
            definitions_.insert(iterator, definition);
        }
        return true;
    }

    bool spawn(const VfxCue& cue, std::uint64_t nowMilliseconds) {
        if (cue.eventId == 0 || cue.definitionId == 0 ||
            findInstance(cue.eventId) != instances_.end()) {
            return false;
        }
        const auto* definition = findDefinition(cue.definitionId);
        if (!definition) return false;
        Instance instance;
        instance.eventId = cue.eventId;
        instance.definitionId = cue.definitionId;
        instance.startedMilliseconds = nowMilliseconds;
        instance.x = cue.x;
        instance.y = cue.y;
        instance.randomState = mix(cue.eventId ^ cue.definitionId);
        instance.particles.reserve(definition->maximumParticles);
        emit(instance, *definition, definition->burstParticles, cue.directionRadians);
        const auto iterator = std::lower_bound(
            instances_.begin(), instances_.end(), cue.eventId,
            [](const Instance& value, VfxEventId id) {
                return value.eventId < id;
            });
        instances_.insert(iterator, std::move(instance));
        return true;
    }

    void update(
        std::uint64_t nowMilliseconds,
        std::uint32_t deltaMilliseconds) {
        auto iterator = instances_.begin();
        while (iterator != instances_.end()) {
            const auto* definition = findDefinition(iterator->definitionId);
            if (!definition) {
                iterator = instances_.erase(iterator);
                continue;
            }
            const auto elapsed = nowMilliseconds >= iterator->startedMilliseconds
                ? nowMilliseconds - iterator->startedMilliseconds
                : 0;
            if (elapsed >= definition->durationMilliseconds) {
                iterator = instances_.erase(iterator);
                continue;
            }
            const auto deltaSeconds =
                static_cast<float>(deltaMilliseconds) / 1000.0f;
            for (auto& particle : iterator->particles) {
                particle.ageMilliseconds = std::min<std::uint32_t>(
                    particle.lifetimeMilliseconds,
                    particle.ageMilliseconds + deltaMilliseconds);
                particle.velocityY += definition->gravity * deltaSeconds;
                particle.x += particle.velocityX * deltaSeconds;
                particle.y += particle.velocityY * deltaSeconds;
            }
            iterator->particles.erase(
                std::remove_if(
                    iterator->particles.begin(), iterator->particles.end(),
                    [](const Particle& particle) {
                        return particle.ageMilliseconds >=
                               particle.lifetimeMilliseconds;
                    }),
                iterator->particles.end());
            ++iterator;
        }
    }

    void buildViews(std::vector<VfxParticleView>& output) const {
        output.clear();
        for (const auto& instance : instances_) {
            const auto* definition = findDefinition(instance.definitionId);
            if (!definition) continue;
            for (const auto& particle : instance.particles) {
                const auto normalized = particle.lifetimeMilliseconds == 0
                    ? 1.0f
                    : static_cast<float>(particle.ageMilliseconds) /
                      static_cast<float>(particle.lifetimeMilliseconds);
                output.push_back({
                    instance.eventId,
                    definition->spriteId,
                    particle.x,
                    particle.y,
                    definition->startSize +
                        (definition->endSize - definition->startSize) * normalized,
                    std::clamp(1.0f - normalized, 0.0f, 1.0f),
                    definition->additive});
            }
        }
    }

    void clear() noexcept { instances_.clear(); }
    std::size_t activeEffectCount() const noexcept { return instances_.size(); }

private:
    struct Particle final {
        float x{};
        float y{};
        float velocityX{};
        float velocityY{};
        std::uint32_t ageMilliseconds{};
        std::uint32_t lifetimeMilliseconds{};
    };

    struct Instance final {
        VfxEventId eventId{};
        VfxDefinitionId definitionId{};
        std::uint64_t startedMilliseconds{};
        float x{};
        float y{};
        std::uint64_t randomState{};
        std::vector<Particle> particles;
    };

    static bool valid(const VfxDefinition& definition) noexcept {
        return definition.id != 0 && definition.spriteId != 0 &&
               definition.durationMilliseconds != 0 &&
               definition.particleLifetimeMilliseconds != 0 &&
               definition.maximumParticles != 0 &&
               definition.burstParticles <= definition.maximumParticles &&
               std::isfinite(definition.speed) &&
               std::isfinite(definition.spreadRadians) &&
               std::isfinite(definition.gravity) &&
               std::isfinite(definition.startSize) &&
               std::isfinite(definition.endSize);
    }

    const VfxDefinition* findDefinition(VfxDefinitionId id) const noexcept {
        const auto iterator = std::lower_bound(
            definitions_.begin(), definitions_.end(), id,
            [](const VfxDefinition& value, VfxDefinitionId lookup) {
                return value.id < lookup;
            });
        return iterator != definitions_.end() && iterator->id == id
            ? &*iterator : nullptr;
    }

    std::vector<Instance>::iterator findInstance(VfxEventId id) {
        return std::lower_bound(
            instances_.begin(), instances_.end(), id,
            [](const Instance& value, VfxEventId lookup) {
                return value.eventId < lookup;
            });
    }

    static std::uint64_t mix(std::uint64_t value) noexcept {
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        value ^= value >> 31u;
        return value == 0 ? 1 : value;
    }

    static std::uint32_t nextRandom(Instance& instance) noexcept {
        auto value = instance.randomState;
        value ^= value << 13u;
        value ^= value >> 7u;
        value ^= value << 17u;
        instance.randomState = value == 0 ? 1 : value;
        return static_cast<std::uint32_t>(instance.randomState >> 32u);
    }

    static float unitRandom(Instance& instance) noexcept {
        return static_cast<float>(nextRandom(instance)) /
               static_cast<float>(std::numeric_limits<std::uint32_t>::max());
    }

    static void emit(
        Instance& instance,
        const VfxDefinition& definition,
        std::uint16_t count,
        float directionRadians) {
        const auto available = definition.maximumParticles >
                instance.particles.size()
            ? definition.maximumParticles - instance.particles.size()
            : 0;
        count = static_cast<std::uint16_t>(std::min<std::size_t>(count, available));
        for (std::uint16_t index = 0; index < count; ++index) {
            const auto offset = (unitRandom(instance) - 0.5f) *
                                definition.spreadRadians;
            const auto angle = directionRadians + offset;
            instance.particles.push_back({
                instance.x,
                instance.y,
                std::cos(angle) * definition.speed,
                std::sin(angle) * definition.speed,
                0,
                definition.particleLifetimeMilliseconds});
        }
    }

    std::vector<VfxDefinition> definitions_;
    std::vector<Instance> instances_;
};

} // namespace rts::presentation
