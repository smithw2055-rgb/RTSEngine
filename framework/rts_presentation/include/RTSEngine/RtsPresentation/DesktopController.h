#pragma once

#include <RTSEngine/Platform/InputState.h>
#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/Presentation/SpriteBatch.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <cstdint>
#include <vector>

namespace rts::rts_presentation {

enum class DesktopInteractionMode : std::uint8_t {
    Select,
    AttackMove,
    Build
};

struct DesktopControllerConfig final {
    std::uint32_t playerTeam{1};
    std::uint32_t issuer{1};
    std::uint32_t buildingDefinitionId{1};
    float clickRadiusWorld{0.8f};
    float dragThresholdPixels{5.0f};
    float panSpeedWorldPerSecond{12.0f};
    float minimumWorldWidth{8.0f};
    float maximumWorldWidth{96.0f};
};

struct DesktopControllerFrame final {
    std::uint64_t targetTick{};
    std::uint32_t framebufferWidth{};
    std::uint32_t framebufferHeight{};
    float deltaSeconds{};
    bool pointerCaptured{};
};

struct SelectionDrag final {
    bool active{};
    float startX{};
    float startY{};
    float currentX{};
    float currentY{};
};

struct DesktopControllerResult final {
    std::vector<gameplay::TickCommand> commands;
    bool selectionChanged{};
    bool cameraChanged{};
    bool modeChanged{};
};

class DesktopController final {
public:
    explicit DesktopController(DesktopControllerConfig config = {}) noexcept;

    DesktopControllerResult update(
        const platform::InputState& input,
        const gameplay::WorldSnapshot& snapshot,
        DesktopControllerFrame frame);

    void decorate(presentation::RenderPacket& packet) const;

    const presentation::Camera2D& camera() const noexcept;
    void setCamera(presentation::Camera2D camera) noexcept;
    DesktopInteractionMode mode() const noexcept;
    void setMode(DesktopInteractionMode mode) noexcept;
    const std::vector<ecs::Entity>& selection() const noexcept;
    const SelectionDrag& selectionDrag() const noexcept;
    gameplay::GridPoint pointerWorldCell() const noexcept;
    void clearSelection() noexcept;

private:
    struct WorldPoint final { float x{}; float y{}; };

    static bool entityEquals(ecs::Entity a, ecs::Entity b) noexcept;
    static bool isUnit(const gameplay::SnapshotEntity& entity) noexcept;
    static bool containsEntity(const std::vector<ecs::Entity>& values,
                               ecs::Entity entity) noexcept;
    static float square(float value) noexcept;

    WorldPoint screenToWorld(float x, float y,
                             std::uint32_t width,
                             std::uint32_t height) const noexcept;
    void updateCamera(const platform::InputState& input,
                      const gameplay::WorldSnapshot& snapshot,
                      DesktopControllerFrame frame,
                      DesktopControllerResult& result);
    void pruneSelection(const gameplay::WorldSnapshot& snapshot);
    const gameplay::SnapshotEntity* entityAt(
        const gameplay::WorldSnapshot& snapshot,
        WorldPoint point,
        bool enemyOnly) const noexcept;
    bool visibleToPlayer(const gameplay::WorldSnapshot& snapshot,
                         const gameplay::SnapshotEntity& entity) const noexcept;
    void finishSelection(const platform::InputState& input,
                         const gameplay::WorldSnapshot& snapshot,
                         DesktopControllerFrame frame,
                         DesktopControllerResult& result);
    void issueContextCommand(const platform::InputState& input,
                             const gameplay::WorldSnapshot& snapshot,
                             DesktopControllerFrame frame,
                             DesktopControllerResult& result);
    void issueSimple(gameplay::CommandType type,
                     std::uint64_t targetTick,
                     bool append,
                     DesktopControllerResult& result);
    gameplay::TickCommand makeCommand(gameplay::CommandType type,
                                      std::uint64_t tick) noexcept;
    void clampCamera(const gameplay::WorldSnapshot& snapshot) noexcept;

    DesktopControllerConfig config_{};
    presentation::Camera2D camera_{16.0f, 9.0f, 32.0f, 18.0f, true};
    DesktopInteractionMode mode_{DesktopInteractionMode::Select};
    std::vector<ecs::Entity> selection_;
    SelectionDrag drag_{};
    WorldPoint pointerWorld_{};
    std::uint32_t nextSequence_{1};
};

} // namespace rts::rts_presentation
