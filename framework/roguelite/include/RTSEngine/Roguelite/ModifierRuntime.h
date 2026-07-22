#pragma once

#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::roguelite {

using ModifierId = std::uint32_t;
using StatId = std::uint64_t;
using TagId = std::uint64_t;

inline StatId MakeStatId(std::string_view name) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString(name);
    return hash.Value();
}

inline TagId MakeTagId(std::string_view name) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString(name);
    return hash.Value();
}

enum class ModifierOperation : std::uint8_t {
    Add,
    Multiply,
    Override
};

struct ModifierEffect {
    StatId stat{};
    ModifierOperation operation{ModifierOperation::Add};
    std::int32_t value{};
};

struct ModifierDefinition {
    ModifierId id{};
    std::uint32_t weight{1};
    std::uint32_t maxStacks{1};
    std::vector<TagId> tags;
    std::vector<ModifierId> requiredModifiers;
    std::vector<TagId> requiredTags;
    std::vector<ModifierId> excludedModifiers;
    std::vector<TagId> excludedTags;
    std::vector<ModifierEffect> effects;
};

struct ModifierStack {
    ModifierId id{};
    std::uint32_t stacks{};

    friend bool operator==(ModifierStack a,
                           ModifierStack b) noexcept {
        return a.id == b.id && a.stacks == b.stacks;
    }
};

enum class ApplyFailure : std::uint8_t {
    None,
    UnknownModifier,
    MaximumStacks,
    MissingPrerequisite,
    Excluded
};

struct ApplyResult {
    bool accepted{};
    ApplyFailure failure{ApplyFailure::None};
    ModifierId modifierId{};
    std::uint32_t stacks{};
};

class ModifierRuntime {
public:
    static constexpr std::int32_t kMultiplierScale = 1000;

    bool registerDefinition(ModifierDefinition definition) {
        if (!valid(definition)) return false;
        normalize(definition.tags);
        normalize(definition.requiredModifiers);
        normalize(definition.requiredTags);
        normalize(definition.excludedModifiers);
        normalize(definition.excludedTags);

        const auto iterator = std::lower_bound(
            definitions_.begin(), definitions_.end(), definition.id,
            [](const ModifierDefinition& current, ModifierId id) {
                return current.id < id;
            });
        if (iterator != definitions_.end() &&
            iterator->id == definition.id) {
            *iterator = std::move(definition);
        } else {
            definitions_.insert(iterator, std::move(definition));
        }
        return true;
    }

    const ModifierDefinition* definition(ModifierId id) const noexcept {
        const auto iterator = std::lower_bound(
            definitions_.begin(), definitions_.end(), id,
            [](const ModifierDefinition& current, ModifierId key) {
                return current.id < key;
            });
        return iterator != definitions_.end() && iterator->id == id
            ? &*iterator
            : nullptr;
    }

    const std::vector<ModifierDefinition>& definitions() const noexcept {
        return definitions_;
    }

    const std::vector<ModifierStack>& stacks() const noexcept {
        return stacks_;
    }

    std::vector<ModifierId> definitionIds() const {
        std::vector<ModifierId> result;
        result.reserve(definitions_.size());
        for (const auto& value : definitions_) result.push_back(value.id);
        return result;
    }

    std::uint32_t stackCount(ModifierId id) const noexcept {
        const auto iterator = stackIterator(id);
        return iterator == stacks_.end() ? 0u : iterator->stacks;
    }

    bool hasModifier(ModifierId id) const noexcept {
        return stackCount(id) > 0;
    }

    bool hasTag(TagId tag) const noexcept {
        if (tag == 0) return false;
        for (const auto& stack : stacks_) {
            if (stack.stacks == 0) continue;
            const auto* value = definition(stack.id);
            if (value && contains(value->tags, tag)) return true;
        }
        return false;
    }

    ApplyFailure canApply(ModifierId id) const noexcept {
        const auto* candidate = definition(id);
        if (!candidate) return ApplyFailure::UnknownModifier;
        if (stackCount(id) >= candidate->maxStacks) {
            return ApplyFailure::MaximumStacks;
        }

        for (const auto required : candidate->requiredModifiers) {
            if (!hasModifier(required)) {
                return ApplyFailure::MissingPrerequisite;
            }
        }
        for (const auto required : candidate->requiredTags) {
            if (!hasTag(required)) {
                return ApplyFailure::MissingPrerequisite;
            }
        }
        for (const auto excluded : candidate->excludedModifiers) {
            if (hasModifier(excluded)) return ApplyFailure::Excluded;
        }
        for (const auto excluded : candidate->excludedTags) {
            if (hasTag(excluded)) return ApplyFailure::Excluded;
        }

        for (const auto& activeStack : stacks_) {
            if (activeStack.stacks == 0) continue;
            const auto* active = definition(activeStack.id);
            if (!active) continue;
            if (contains(active->excludedModifiers, candidate->id)) {
                return ApplyFailure::Excluded;
            }
            for (const auto candidateTag : candidate->tags) {
                if (contains(active->excludedTags, candidateTag)) {
                    return ApplyFailure::Excluded;
                }
            }
        }
        return ApplyFailure::None;
    }

    ApplyResult apply(ModifierId id) {
        const auto failure = canApply(id);
        if (failure != ApplyFailure::None) {
            return {false, failure, id, stackCount(id)};
        }

        auto iterator = std::lower_bound(
            stacks_.begin(), stacks_.end(), id,
            [](const ModifierStack& current, ModifierId key) {
                return current.id < key;
            });
        if (iterator != stacks_.end() && iterator->id == id) {
            ++iterator->stacks;
        } else {
            iterator = stacks_.insert(iterator, ModifierStack{id, 1});
        }
        return {true, ApplyFailure::None, id, iterator->stacks};
    }

    std::vector<ModifierId> eligible(
        std::vector<ModifierId> candidates) const {
        normalize(candidates);
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                           [this](ModifierId id) {
                               return canApply(id) != ApplyFailure::None;
                           }),
            candidates.end());
        return candidates;
    }

    std::int32_t resolve(StatId stat,
                         std::int32_t baseValue) const noexcept {
        std::int64_t value = baseValue;
        for (const auto& stack : stacks_) {
            const auto* modifier = definition(stack.id);
            if (!modifier) continue;
            for (std::uint32_t layer = 0;
                 layer < stack.stacks; ++layer) {
                for (const auto& effect : modifier->effects) {
                    if (effect.stat != stat) continue;
                    switch (effect.operation) {
                    case ModifierOperation::Add:
                        value = clamp(value + effect.value);
                        break;
                    case ModifierOperation::Multiply:
                        value = clamp(
                            (value * effect.value) /
                            kMultiplierScale);
                        break;
                    case ModifierOperation::Override:
                        value = effect.value;
                        break;
                    }
                }
            }
        }
        return static_cast<std::int32_t>(clamp(value));
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(definitions_.size()));
        for (const auto& definitionValue : definitions_) {
            hash.WriteU32(definitionValue.id);
            hash.WriteU32(definitionValue.weight);
            hash.WriteU32(definitionValue.maxStacks);
            hashVector(hash, definitionValue.tags);
            hashVector(hash, definitionValue.requiredModifiers);
            hashVector(hash, definitionValue.requiredTags);
            hashVector(hash, definitionValue.excludedModifiers);
            hashVector(hash, definitionValue.excludedTags);
            hash.WriteU32(
                static_cast<std::uint32_t>(definitionValue.effects.size()));
            for (const auto& effect : definitionValue.effects) {
                hash.WriteU64(effect.stat);
                hash.WriteU8(
                    static_cast<std::uint8_t>(effect.operation));
                hash.WriteI32(effect.value);
            }
        }

        hash.WriteU32(static_cast<std::uint32_t>(stacks_.size()));
        for (const auto& stack : stacks_) {
            hash.WriteU32(stack.id);
            hash.WriteU32(stack.stacks);
        }
    }

private:
    template<class T>
    static void normalize(std::vector<T>& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()),
                     values.end());
    }

    template<class T>
    static bool contains(const std::vector<T>& values,
                         const T& value) noexcept {
        return std::binary_search(values.begin(), values.end(), value);
    }

    static bool valid(const ModifierDefinition& definitionValue) noexcept {
        if (definitionValue.id == 0 || definitionValue.weight == 0 ||
            definitionValue.maxStacks == 0) {
            return false;
        }
        if (std::find(definitionValue.requiredModifiers.begin(),
                      definitionValue.requiredModifiers.end(),
                      definitionValue.id) !=
            definitionValue.requiredModifiers.end()) {
            return false;
        }
        if (std::find(definitionValue.excludedModifiers.begin(),
                      definitionValue.excludedModifiers.end(),
                      definitionValue.id) !=
            definitionValue.excludedModifiers.end()) {
            return false;
        }
        for (const auto& effect : definitionValue.effects) {
            if (effect.stat == 0) return false;
            if (effect.operation == ModifierOperation::Multiply &&
                effect.value < 0) {
                return false;
            }
        }
        return true;
    }

    static std::int64_t clamp(std::int64_t value) noexcept {
        return std::max<std::int64_t>(
            std::numeric_limits<std::int32_t>::min(),
            std::min<std::int64_t>(
                std::numeric_limits<std::int32_t>::max(), value));
    }

    std::vector<ModifierStack>::const_iterator stackIterator(
        ModifierId id) const noexcept {
        return std::lower_bound(
            stacks_.begin(), stacks_.end(), id,
            [](const ModifierStack& current, ModifierId key) {
                return current.id < key;
            });
    }

    template<class T>
    static void hashVector(foundation::CanonicalHash& hash,
                           const std::vector<T>& values) noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(values.size()));
        for (const auto value : values) {
            if constexpr (sizeof(T) <= sizeof(std::uint32_t)) {
                hash.WriteU32(static_cast<std::uint32_t>(value));
            } else {
                hash.WriteU64(static_cast<std::uint64_t>(value));
            }
        }
    }

    std::vector<ModifierDefinition> definitions_;
    std::vector<ModifierStack> stacks_;
};

inline StatId WaveCompletionResourceStat() noexcept {
    static const StatId value =
        MakeStatId("run.wave-completion-resource");
    return value;
}

} // namespace rts::roguelite
