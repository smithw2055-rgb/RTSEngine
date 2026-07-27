#pragma once

#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct ResourceLedger final {
    std::int32_t available{};
    std::int32_t reserved{};
    std::int32_t spent{};

    bool reserve(std::int32_t amount) noexcept {
        if (amount < 0 || available < amount ||
            reserved > std::numeric_limits<std::int32_t>::max() - amount) {
            return false;
        }
        available -= amount;
        reserved += amount;
        return true;
    }

    bool commit(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount ||
            spent > std::numeric_limits<std::int32_t>::max() - amount) {
            return false;
        }
        reserved -= amount;
        spent += amount;
        return true;
    }

    bool release(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount ||
            available > std::numeric_limits<std::int32_t>::max() - amount) {
            return false;
        }
        reserved -= amount;
        available += amount;
        return true;
    }

    std::int32_t addAvailable(std::int32_t amount) noexcept {
        if (amount <= 0) return 0;
        const auto headroom =
            std::numeric_limits<std::int32_t>::max() - available;
        const auto added = std::min(amount, headroom);
        available += added;
        return added;
    }

    bool valid() const noexcept {
        return available >= 0 && reserved >= 0 && spent >= 0;
    }
};

struct TeamEconomyAccount final {
    std::uint32_t teamId{};
    ResourceLedger resources{};
    std::uint32_t supplyUsed{};
    std::uint32_t supplyReserved{};
    std::uint32_t supplyCapacity{};
};

struct TeamEconomySnapshot final {
    std::uint32_t teamId{};
    ResourceLedger resources{};
    std::uint32_t supplyUsed{};
    std::uint32_t supplyReserved{};
    std::uint32_t supplyCapacity{};
};

class TeamEconomyRuntime final {
public:
    static constexpr std::uint32_t kMaximumTeams = 4096u;

    const std::vector<TeamEconomyAccount>& accounts() const noexcept {
        return accounts_;
    }

    const TeamEconomyAccount* find(std::uint32_t teamId) const noexcept {
        const auto found = lowerBound(teamId);
        return found != accounts_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    TeamEconomyAccount* find(std::uint32_t teamId) noexcept {
        const auto found = lowerBound(teamId);
        return found != accounts_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    TeamEconomyAccount* ensure(std::uint32_t teamId) {
        if (teamId == 0 || accounts_.size() >= kMaximumTeams) return nullptr;
        auto found = lowerBound(teamId);
        if (found != accounts_.end() && found->teamId == teamId) {
            return &*found;
        }
        found = accounts_.insert(found, TeamEconomyAccount{teamId});
        return &*found;
    }

    bool setResources(std::uint32_t teamId, std::int32_t available) {
        auto* account = ensure(teamId);
        if (!account) return false;
        account->resources.available = std::max<std::int32_t>(0, available);
        return true;
    }

    const ResourceLedger& resources(std::uint32_t teamId) const noexcept {
        static const ResourceLedger empty;
        const auto* account = find(teamId);
        return account ? account->resources : empty;
    }

    bool reserveResources(std::uint32_t teamId, std::int32_t amount) {
        auto* account = ensure(teamId);
        return account && account->resources.reserve(amount);
    }

    bool commitResources(std::uint32_t teamId, std::int32_t amount) {
        auto* account = find(teamId);
        return account && account->resources.commit(amount);
    }

    bool releaseResources(std::uint32_t teamId, std::int32_t amount) {
        auto* account = find(teamId);
        return account && account->resources.release(amount);
    }

    std::int32_t addResources(std::uint32_t teamId, std::int32_t amount) {
        auto* account = ensure(teamId);
        return account ? account->resources.addAvailable(amount) : 0;
    }

    void beginSupplyRebuild() noexcept {
        for (auto& account : accounts_) {
            account.supplyUsed = 0;
            account.supplyReserved = 0;
            account.supplyCapacity = 0;
        }
    }

    bool addSupplyCapacity(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = ensure(teamId);
        if (!account) return false;
        account->supplyCapacity = saturatingAdd(account->supplyCapacity, amount);
        return true;
    }

    bool addSupplyUsed(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = ensure(teamId);
        if (!account) return false;
        account->supplyUsed = saturatingAdd(account->supplyUsed, amount);
        return true;
    }

    bool addSupplyReserved(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = ensure(teamId);
        if (!account) return false;
        account->supplyReserved = saturatingAdd(
            account->supplyReserved, amount);
        return true;
    }

    bool reserveSupply(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = ensure(teamId);
        if (!account) return false;
        const auto committed = static_cast<std::uint64_t>(account->supplyUsed) +
                               account->supplyReserved;
        if (committed + amount > account->supplyCapacity) return false;
        account->supplyReserved += amount;
        return true;
    }

    bool commitSupply(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = find(teamId);
        if (!account || account->supplyReserved < amount ||
            account->supplyUsed >
                std::numeric_limits<std::uint32_t>::max() - amount) {
            return false;
        }
        account->supplyReserved -= amount;
        account->supplyUsed += amount;
        return true;
    }

    bool releaseReservedSupply(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = find(teamId);
        if (!account || account->supplyReserved < amount) return false;
        account->supplyReserved -= amount;
        return true;
    }

    bool releaseUsedSupply(std::uint32_t teamId, std::uint32_t amount) {
        auto* account = find(teamId);
        if (!account || account->supplyUsed < amount) return false;
        account->supplyUsed -= amount;
        return true;
    }

    void buildSnapshot(std::vector<TeamEconomySnapshot>& output) const {
        output.clear();
        output.reserve(accounts_.size());
        for (const auto& account : accounts_) {
            output.push_back(
                {account.teamId,
                 account.resources,
                 account.supplyUsed,
                 account.supplyReserved,
                 account.supplyCapacity});
        }
    }

    void appendHash(foundation::CanonicalHash& hash) const {
        hash.WriteU32(static_cast<std::uint32_t>(accounts_.size()));
        for (const auto& account : accounts_) {
            hash.WriteU32(account.teamId);
            hash.WriteI32(account.resources.available);
            hash.WriteI32(account.resources.reserved);
            hash.WriteI32(account.resources.spent);
            hash.WriteU32(account.supplyUsed);
            hash.WriteU32(account.supplyReserved);
            hash.WriteU32(account.supplyCapacity);
        }
    }

    bool restoreAccounts(std::vector<TeamEconomyAccount> accounts) {
        if (accounts.size() > kMaximumTeams) return false;
        std::uint32_t previous = 0;
        for (auto& account : accounts) {
            if (account.teamId == 0 || account.teamId <= previous ||
                !account.resources.valid()) {
                return false;
            }
            account.supplyUsed = 0;
            account.supplyReserved = 0;
            account.supplyCapacity = 0;
            previous = account.teamId;
        }
        accounts_ = std::move(accounts);
        return true;
    }

private:
    using Iterator = std::vector<TeamEconomyAccount>::iterator;
    using ConstIterator = std::vector<TeamEconomyAccount>::const_iterator;

    Iterator lowerBound(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            accounts_.begin(), accounts_.end(), teamId,
            [](const TeamEconomyAccount& account, std::uint32_t value) {
                return account.teamId < value;
            });
    }

    ConstIterator lowerBound(std::uint32_t teamId) const noexcept {
        return std::lower_bound(
            accounts_.begin(), accounts_.end(), teamId,
            [](const TeamEconomyAccount& account, std::uint32_t value) {
                return account.teamId < value;
            });
    }

    static std::uint32_t saturatingAdd(
        std::uint32_t value,
        std::uint32_t amount) noexcept {
        return amount > std::numeric_limits<std::uint32_t>::max() - value
            ? std::numeric_limits<std::uint32_t>::max()
            : value + amount;
    }

    std::vector<TeamEconomyAccount> accounts_;
};

} // namespace rts::gameplay
