#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

enum class DiplomaticRelation : std::uint8_t {
    Allied,
    Neutral,
    Hostile
};

struct DiplomaticRelationEntry final {
    std::uint32_t firstTeam{};
    std::uint32_t secondTeam{};
    DiplomaticRelation relation{DiplomaticRelation::Hostile};
};

class DiplomacyRuntime final {
public:
    bool setRelation(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam,
        DiplomaticRelation relation) {
        if (firstTeam == 0 || secondTeam == 0 || firstTeam == secondTeam) {
            return false;
        }
        normalize(firstTeam, secondTeam);
        const auto found = lowerBound(firstTeam, secondTeam);
        if (found != entries_.end() &&
            found->firstTeam == firstTeam && found->secondTeam == secondTeam) {
            found->relation = relation;
            return true;
        }
        entries_.insert(
            found,
            DiplomaticRelationEntry{firstTeam, secondTeam, relation});
        return true;
    }

    DiplomaticRelation relation(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam) const noexcept {
        if (firstTeam != 0 && firstTeam == secondTeam) {
            return DiplomaticRelation::Allied;
        }
        if (firstTeam == 0 || secondTeam == 0) {
            return DiplomaticRelation::Neutral;
        }
        normalize(firstTeam, secondTeam);
        const auto found = lowerBound(firstTeam, secondTeam);
        return found != entries_.end() &&
               found->firstTeam == firstTeam && found->secondTeam == secondTeam
            ? found->relation
            : DiplomaticRelation::Hostile;
    }

    bool allied(std::uint32_t firstTeam, std::uint32_t secondTeam) const noexcept {
        return relation(firstTeam, secondTeam) == DiplomaticRelation::Allied;
    }

    bool hostile(std::uint32_t firstTeam, std::uint32_t secondTeam) const noexcept {
        return relation(firstTeam, secondTeam) == DiplomaticRelation::Hostile;
    }

    const std::vector<DiplomaticRelationEntry>& entries() const noexcept {
        return entries_;
    }

    bool restore(std::vector<DiplomaticRelationEntry> entries) {
        std::sort(entries.begin(), entries.end(), lessEntry);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            if (entry.firstTeam == 0 || entry.secondTeam == 0 ||
                entry.firstTeam >= entry.secondTeam ||
                (index != 0 && !lessEntry(entries[index - 1], entry))) {
                return false;
            }
        }
        entries_ = std::move(entries);
        return true;
    }

private:
    static void normalize(
        std::uint32_t& firstTeam,
        std::uint32_t& secondTeam) noexcept {
        if (secondTeam < firstTeam) std::swap(firstTeam, secondTeam);
    }

    static bool lessEntry(
        const DiplomaticRelationEntry& left,
        const DiplomaticRelationEntry& right) noexcept {
        return left.firstTeam < right.firstTeam ||
               (left.firstTeam == right.firstTeam &&
                left.secondTeam < right.secondTeam);
    }

    auto lowerBound(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam) noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(),
            DiplomaticRelationEntry{firstTeam, secondTeam, {}},
            lessEntry);
    }

    auto lowerBound(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam) const noexcept {
        return std::lower_bound(
            entries_.begin(), entries_.end(),
            DiplomaticRelationEntry{firstTeam, secondTeam, {}},
            lessEntry);
    }

    std::vector<DiplomaticRelationEntry> entries_;
};

} // namespace rts::gameplay
