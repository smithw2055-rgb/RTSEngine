#pragma once

#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

using ResourceTypeId = std::uint32_t;
using ResourceAmount = std::int64_t;

inline constexpr ResourceTypeId kPrimaryResourceType = 1u;

struct ResourceLedger final {
    std::int32_t available{};
    std::int32_t reserved{};
    std::int32_t spent{};

    bool reserve(std::int32_t amount) noexcept {
        if (amount < 0 || available < amount) return false;
        available -= amount;
        reserved += amount;
        return true;
    }

    bool commit(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount) return false;
        reserved -= amount;
        spent += amount;
        return true;
    }

    bool release(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount) return false;
        reserved -= amount;
        const auto next = std::min<std::int64_t>(
            std::numeric_limits<std::int32_t>::max(),
            static_cast<std::int64_t>(available) + amount);
        available = static_cast<std::int32_t>(next);
        return true;
    }
};

struct ResourceDropOff final {
    ResourceTypeId resourceType{};
    std::int32_t accessX{};
    std::int32_t accessY{};
};

struct SupplyProvider final {
    std::uint32_t capacity{};
};

struct TeamResourceAccount final {
    std::uint32_t teamId{};
    ResourceTypeId resourceType{};
    ResourceAmount available{};
    ResourceAmount reserved{};
    ResourceAmount spent{};
};

class TeamEconomyRuntime final {
public:
    bool setAvailable(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        if (!validKey(teamId, resourceType) || amount < 0) return false;
        auto& value = ensure(teamId, resourceType);
        value.available = amount;
        return true;
    }

    bool restore(std::vector<TeamResourceAccount> entries) {
        if (!validate(entries)) return false;
        entries_ = std::move(entries);
        return true;
    }

    bool importLegacy(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        const ResourceLedger& ledger) {
        if (!validKey(teamId, resourceType) || ledger.available < 0 ||
            ledger.reserved < 0 || ledger.spent < 0) {
            return false;
        }
        auto& value = ensure(teamId, resourceType);
        value.available = ledger.available;
        value.reserved = ledger.reserved;
        value.spent = ledger.spent;
        return true;
    }

    bool credit(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        if (!validKey(teamId, resourceType) || amount < 0) return false;
        auto& value = ensure(teamId, resourceType);
        value.available = saturatedAdd(value.available, amount);
        return true;
    }

    bool reserve(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        if (!validKey(teamId, resourceType) || amount < 0) return false;
        auto& value = ensure(teamId, resourceType);
        if (value.available < amount ||
            value.reserved > std::numeric_limits<ResourceAmount>::max() - amount) {
            return false;
        }
        value.available -= amount;
        value.reserved += amount;
        return true;
    }

    bool commit(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        if (!validKey(teamId, resourceType) || amount < 0) return false;
        auto& value = ensure(teamId, resourceType);
        if (value.reserved < amount ||
            value.spent > std::numeric_limits<ResourceAmount>::max() - amount) {
            return false;
        }
        value.reserved -= amount;
        value.spent += amount;
        return true;
    }

    bool release(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        if (!validKey(teamId, resourceType) || amount < 0) return false;
        auto& value = ensure(teamId, resourceType);
        if (value.reserved < amount) return false;
        value.reserved -= amount;
        value.available = saturatedAdd(value.available, amount);
        return true;
    }

    const TeamResourceAccount* find(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        if (!validKey(teamId, resourceType)) return nullptr;
        const auto found = lowerBound(teamId, resourceType);
        return found != entries_.end() && found->teamId == teamId &&
               found->resourceType == resourceType
            ? &*found
            : nullptr;
    }

    ResourceAmount available(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        const auto* value = find(teamId, resourceType);
        return value ? value->available : 0;
    }

    ResourceAmount reserved(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        const auto* value = find(teamId, resourceType);
        return value ? value->reserved : 0;
    }

    ResourceAmount spent(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        const auto* value = find(teamId, resourceType);
        return value ? value->spent : 0;
    }

    ResourceLedger legacyLedger(
        std::uint32_t teamId,
        ResourceTypeId resourceType = kPrimaryResourceType) const noexcept {
        const auto* value = find(teamId, resourceType);
        if (!value) return {};
        return {
            clampLegacy(value->available),
            clampLegacy(value->reserved),
            clampLegacy(value->spent)};
    }

    const std::vector<TeamResourceAccount>& entries() const noexcept {
        return entries_;
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(entries_.size()));
        for (const auto& value : entries_) {
            hash.WriteU32(value.teamId);
            hash.WriteU32(value.resourceType);
            hash.WriteU64(static_cast<std::uint64_t>(value.available));
            hash.WriteU64(static_cast<std::uint64_t>(value.reserved));
            hash.WriteU64(static_cast<std::uint64_t>(value.spent));
        }
    }

    static bool validate(
        const std::vector<TeamResourceAccount>& entries) noexcept {
        std::uint32_t previousTeam = 0;
        ResourceTypeId previousType = 0;
        bool hasPrevious = false;
        for (const auto& value : entries) {
            if (!validKey(value.teamId, value.resourceType) ||
                value.available < 0 || value.reserved < 0 || value.spent < 0 ||
                (hasPrevious &&
                 (value.teamId < previousTeam ||
                  (value.teamId == previousTeam &&
                   value.resourceType <= previousType)))) {
                return false;
            }
            previousTeam = value.teamId;
            previousType = value.resourceType;
            hasPrevious = true;
        }
        return true;
    }

private:
    using Iterator = std::vector<TeamResourceAccount>::iterator;
    using ConstIterator = std::vector<TeamResourceAccount>::const_iterator;

    static bool validKey(
        std::uint32_t teamId,
        ResourceTypeId resourceType) noexcept {
        return teamId != 0 && resourceType != 0;
    }

    static ResourceAmount saturatedAdd(
        ResourceAmount first,
        ResourceAmount second) noexcept {
        return first > std::numeric_limits<ResourceAmount>::max() - second
            ? std::numeric_limits<ResourceAmount>::max()
            : first + second;
    }

    static std::int32_t clampLegacy(ResourceAmount value) noexcept {
        return static_cast<std::int32_t>(std::min<ResourceAmount>(
            std::numeric_limits<std::int32_t>::max(),
            std::max<ResourceAmount>(0, value)));
    }

    static bool less(
        const TeamResourceAccount& first,
        const TeamResourceAccount& second) noexcept {
        return first.teamId < second.teamId ||
               (first.teamId == second.teamId &&
                first.resourceType < second.resourceType);
    }

    Iterator lowerBound(
        std::uint32_t teamId,
        ResourceTypeId resourceType) noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(),
            TeamResourceAccount{teamId, resourceType}, less);
    }

    ConstIterator lowerBound(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(),
            TeamResourceAccount{teamId, resourceType}, less);
    }

    TeamResourceAccount& ensure(
        std::uint32_t teamId,
        ResourceTypeId resourceType) {
        const auto found = lowerBound(teamId, resourceType);
        if (found != entries_.end() && found->teamId == teamId &&
            found->resourceType == resourceType) {
            return *found;
        }
        return *entries_.insert(
            found, TeamResourceAccount{teamId, resourceType});
    }

    std::vector<TeamResourceAccount> entries_;
};

} // namespace rts::gameplay
