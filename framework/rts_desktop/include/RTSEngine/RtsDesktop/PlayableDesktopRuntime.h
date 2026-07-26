#pragma once

#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/Audio/AudioDevice.h>
#include <RTSEngine/Presentation/Fixed2DRenderer.h>
#include <RTSEngine/Presentation/PresentationAssetCache.h>
#include <RTSEngine/Presentation/PresentationPlayback.h>
#include <RTSEngine/Presentation/ScreenUi.h>
#include <RTSEngine/Render/RenderDevice.h>
#include <RTSEngine/Roguelite/RunSimulation.h>
#include <RTSEngine/RtsPresentation/DesktopController.h>
#include <RTSEngine/RtsPresentation/RtsPresentationRuntime.h>
#include <RTSEngine/Runtime/DesktopFrameLoop.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rts::rts_desktop {

struct PlayableDesktopConfig final {
    platform::WindowDescription window{"RTSEngine Playable Desktop", 1280, 720,
                                       true, true};
    std::int32_t mapWidth{32};
    std::int32_t mapHeight{18};
    std::uint64_t rootSeed{20260726};
    std::uint32_t playerTeam{1};
    std::uint32_t enemyTeam{2};
};

struct PlayableDesktopStats final {
    std::uint64_t frames{};
    std::uint64_t simulationTicks{};
    std::uint64_t playerCommands{};
    std::uint64_t saveCount{};
    std::uint64_t restoreCount{};
    std::uint64_t rejectedCommands{};
    presentation::Fixed2DRendererStats renderer{};
};

class PlayableDesktopRuntime final {
public:
    PlayableDesktopRuntime(platform::Platform& platform,
                           render::RenderDevice& renderDevice,
                           audio::AudioDevice& audioDevice,
                           PlayableDesktopConfig config = {});
    ~PlayableDesktopRuntime();

    PlayableDesktopRuntime(const PlayableDesktopRuntime&) = delete;
    PlayableDesktopRuntime& operator=(const PlayableDesktopRuntime&) = delete;

    bool initialize();
    runtime::DesktopFrameResult advanceFrame();
    void shutdown() noexcept;

    bool saveToMemory();
    bool restoreFromMemory();
    bool hasMemorySave() const noexcept;

    bool initialized() const noexcept;
    bool quitRequested() const noexcept;
    const roguelite::RunSimulation& simulation() const noexcept;
    const rts_presentation::DesktopController& controller() const noexcept;
    const PlayableDesktopStats& stats() const noexcept;
    const std::string& statusMessage() const noexcept;

private:
    static constexpr std::uint64_t kTextureId = 100;
    static constexpr std::uint64_t kPlayerSpriteId = 101;
    static constexpr std::uint64_t kEnemySpriteId = 102;
    static constexpr std::uint64_t kTowerSpriteId = 103;
    static constexpr std::uint64_t kConstructionSpriteId = 104;

    bool configureContent();
    bool configureSimulation();
    bool configurePresentation();
    void beforeSimulation(const runtime::DesktopFrameContext& frame);
    void stepSimulation(const sim::TickContext& tick);
    bool renderFrame(const runtime::DesktopFrameContext& frame,
                     const sim::FrameStepPlan& plan);
    void buildHud(const runtime::DesktopFrameContext& frame);
    void submitRunCommand(roguelite::CommandType type,
                          std::uint32_t objectId,
                          std::uint64_t targetTick);
    bool pointerOverHud(const runtime::DesktopFrameContext& frame) const noexcept;
    static const char* phaseName(roguelite::RunPhase phase) noexcept;

    platform::Platform& platform_;
    render::RenderDevice& renderDevice_;
    audio::AudioDevice& audioDevice_;
    PlayableDesktopConfig config_{};
    assets::MemoryVfs vfs_;
    assets::AssetManager assets_;
    presentation::PresentationAssetCache assetCache_;
    presentation::Fixed2DRenderer renderer_;
    presentation::MinimalUi ui_;
    presentation::PresentationPlaybackRuntime playback_;
    rts_presentation::RtsPresentationRuntime presentation_;
    rts_presentation::DesktopController controller_;
    roguelite::RunSimulation simulation_;
    runtime::DesktopFrameLoop loop_;
    std::vector<assets::AssetRequestHandle> assetRequests_;
    std::vector<std::uint8_t> memorySave_;
    PlayableDesktopStats stats_{};
    std::string statusMessage_;
    std::uint32_t nextRunSequence_{1};
    std::uint64_t presentationMilliseconds_{};
    bool initialized_{};
    bool publishedSnapshot_{};
};

} // namespace rts::rts_desktop
