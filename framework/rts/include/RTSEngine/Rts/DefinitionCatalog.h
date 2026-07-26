#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::gameplay {

template<class Definition>
class DefinitionCatalog final {
public:
    void replace(Definition value) {
        const auto iterator = std::lower_bound(
            values_.begin(), values_.end(), value.id,
            [](const Definition& current, std::uint32_t id) {
                return current.id < id;
            });
        if (iterator != values_.end() && iterator->id == value.id) {
            *iterator = std::move(value);
        } else {
            values_.insert(iterator, std::move(value));
        }
    }

    const Definition* find(std::uint32_t id) const noexcept {
        const auto iterator = std::lower_bound(
            values_.begin(), values_.end(), id,
            [](const Definition& current, std::uint32_t key) {
                return current.id < key;
            });
        return iterator != values_.end() && iterator->id == id
            ? &*iterator
            : nullptr;
    }

    const std::vector<Definition>& values() const noexcept {
        return values_;
    }

private:
    std::vector<Definition> values_;
};

} // namespace rts::gameplay
