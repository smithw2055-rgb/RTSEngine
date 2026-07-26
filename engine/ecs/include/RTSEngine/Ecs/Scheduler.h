#pragma once

#include <RTSEngine/Ecs/World.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace rts::ecs {

enum class Stage : std::uint8_t {
    Command,
    Navigation,
    Simulation,
    Combat,
    Cleanup,
    Snapshot
};

struct SystemContext {
    std::uint64_t tick{};
    std::uint32_t executionOrdinal{};
    Stage stage{};
};

class Scheduler {
public:
    using Function = std::function<void(World&, const SystemContext&)>;

    void add(Stage stage, std::int32_t order, std::uint32_t systemId, Function function) {
        systems_.push_back({stage, order, systemId, std::move(function)});
        dirty_ = true;
    }

    void run(World& world, std::uint64_t tick) {
        if (dirty_) {
            sort();
        }

        std::uint32_t executionOrdinal = 0;
        for (auto& system : systems_) {
            system.function(world, {tick, executionOrdinal++, system.stage});
        }
    }

    void run_stage(World& world, std::uint64_t tick, Stage stage) {
        if (dirty_) {
            sort();
        }

        std::uint32_t executionOrdinal = 0;
        for (auto& system : systems_) {
            if (system.stage == stage) {
                system.function(world, {tick, executionOrdinal, system.stage});
            }
            ++executionOrdinal;
        }
    }

private:
    struct Entry {
        Stage stage;
        std::int32_t order;
        std::uint32_t systemId;
        Function function;
    };

    void sort() {
        std::stable_sort(systems_.begin(), systems_.end(), [](const Entry& a, const Entry& b) {
            if (a.stage != b.stage) {
                return a.stage < b.stage;
            }
            if (a.order != b.order) {
                return a.order < b.order;
            }
            return a.systemId < b.systemId;
        });
        dirty_ = false;
    }

    std::vector<Entry> systems_;
    bool dirty_{true};
};

} // namespace rts::ecs
