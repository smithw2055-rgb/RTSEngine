#include <RTSEngine/RtsDesktop/PlayableDesktopRuntime.h>

#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/CookedContent.h>
#include <RTSEngine/Roguelite/RunSimulationArchive.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace rts::rts_desktop {
namespace {

void writeCooked(assets::MemoryVfs& vfs,
                 const std::string& path,
                 assets::CookedAsset asset,
                 bool& ok) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    ok = ok && !bytes.empty() && vfs.write(path, bytes);
}

presentation::UiInput uiInput(const platform::InputState& input) {
    const auto& pointer = input.pointer();
    const auto index = static_cast<std::size_t>(platform::PointerButton::Left);
    return {pointer.x, pointer.y, pointer.down[index],
            pointer.pressed[index], pointer.released[index]};
}

} // namespace

PlayableDesktopRuntime::PlayableDesktopRuntime(
    platform::Platform& platform,
    render::RenderDevice& renderDevice,
    audio::AudioDevice& audioDevice,
    PlayableDesktopConfig config)
    : platform_(platform),
      renderDevice_(renderDevice),
      audioDevice_(audioDevice),
      config_(std::move(config)),
      assets_(vfs_, 4u * 1024u * 1024u),
      assetCache_(assets_, renderDevice_),
      renderer_(renderDevice_, assetCache_),
      ui_(renderDevice_),
      playback_(assets_, audioDevice_),
      presentation_({config_.playerTeam, true, false}),
      controller_({config_.playerTeam, 1, 1}),
      simulation_(config_.mapWidth, config_.mapHeight, config_.rootSeed),
      loop_(platform_,
            [this](const sim::TickContext& tick) { stepSimulation(tick); },
            {config_.window, 30.0, 4, 0.25}) {}

PlayableDesktopRuntime::~PlayableDesktopRuntime() { shutdown(); }

bool PlayableDesktopRuntime::initialize() {
    if (initialized_) return true;
    if (config_.mapWidth < 8 || config_.mapHeight < 6 ||
        config_.playerTeam == 0 || config_.enemyTeam == 0 ||
        config_.playerTeam == config_.enemyTeam ||
        !configureContent() || !configureSimulation() ||
        !configurePresentation() || !loop_.initialize() ||
        !renderer_.initialize() || !ui_.initialize()) {
        shutdown();
        return false;
    }
    controller_.setCamera(
        {config_.mapWidth * 0.5f, config_.mapHeight * 0.5f,
         static_cast<float>(config_.mapWidth),
         static_cast<float>(config_.mapHeight), true});
    initialized_ = true;
    statusMessage_ = "Ready";
    return true;
}

runtime::DesktopFrameResult PlayableDesktopRuntime::advanceFrame() {
    if (!initialized_ && !initialize()) return {};
    const auto result = loop_.advanceFrame(
        [this](const runtime::DesktopFrameContext& frame) {
            beforeSimulation(frame);
        },
        [this](const runtime::DesktopFrameContext& frame,
               const sim::FrameStepPlan& plan) {
            return renderFrame(frame, plan);
        });
    ++stats_.frames;
    stats_.renderer = renderer_.stats();
    return result;
}

void PlayableDesktopRuntime::shutdown() noexcept {
    if (!initialized_ && !loop_.initialized()) return;
    ui_.shutdown();
    renderer_.shutdown();
    playback_.clear();
    assetCache_.clear();
    for (const auto request : assetRequests_) {
        (void)assets_.releaseRequest(request);
    }
    assetRequests_.clear();
    loop_.shutdown();
    initialized_ = false;
}

bool PlayableDesktopRuntime::saveToMemory() {
    const auto bytes = roguelite::RunSimulationArchive::encode(simulation_);
    if (bytes.empty()) {
        statusMessage_ = "Save failed";
        return false;
    }
    memorySave_ = bytes;
    ++stats_.saveCount;
    statusMessage_ = "Saved in memory";
    return true;
}

bool PlayableDesktopRuntime::restoreFromMemory() {
    if (memorySave_.empty() ||
        !roguelite::RunSimulationArchive::decode(memorySave_, simulation_)) {
        statusMessage_ = "Restore failed";
        return false;
    }
    loop_.resetSimulationTick(simulation_.lastTick());
    presentation_.reset();
    playback_.clear();
    controller_.clearSelection();
    publishedSnapshot_ = presentation_.publishSnapshot(
        simulation_.tower().rts().snapshot());
    ++stats_.restoreCount;
    statusMessage_ = "Restored memory save";
    return true;
}

bool PlayableDesktopRuntime::hasMemorySave() const noexcept {
    return !memorySave_.empty();
}
bool PlayableDesktopRuntime::initialized() const noexcept { return initialized_; }
bool PlayableDesktopRuntime::quitRequested() const noexcept {
    return loop_.quitRequested();
}
const roguelite::RunSimulation& PlayableDesktopRuntime::simulation() const noexcept {
    return simulation_;
}
const rts_presentation::DesktopController&
PlayableDesktopRuntime::controller() const noexcept { return controller_; }
const PlayableDesktopStats& PlayableDesktopRuntime::stats() const noexcept {
    return stats_;
}
const std::string& PlayableDesktopRuntime::statusMessage() const noexcept {
    return statusMessage_;
}

bool PlayableDesktopRuntime::configureContent() {
    bool ok = true;
    assets::Texture2DContent texture;
    texture.width = 4;
    texture.height = 1;
    texture.format = assets::PixelFormat::Rgba8;
    texture.pixels = {
        55, 175, 255, 255,
        240, 75, 72, 255,
        244, 198, 72, 255,
        92, 225, 134, 210
    };
    writeCooked(vfs_, "desktop/texture.rta",
                assets::CookedContentCodec::textureAsset(kTextureId, texture), ok);

    const std::uint64_t spriteIds[] = {
        kPlayerSpriteId, kEnemySpriteId,
        kTowerSpriteId, kConstructionSpriteId};
    for (std::uint32_t index = 0; index < 4; ++index) {
        assets::SpriteContent sprite;
        sprite.texture = {assets::AssetType::Texture2D, kTextureId};
        sprite.x = index;
        sprite.width = 1;
        sprite.height = 1;
        sprite.worldWidthMilli = index >= 2 ? 1800 : 900;
        sprite.worldHeightMilli = index >= 2 ? 1800 : 900;
        writeCooked(vfs_, "desktop/sprite" + std::to_string(index) + ".rta",
                    assets::CookedContentCodec::spriteAsset(spriteIds[index], sprite), ok);
    }
    ok = ok && assets_.registerAsset(
        {{assets::AssetType::Texture2D, kTextureId},
         "desktop/texture.rta", assets::CookedContentCodec::kTextureSchema});
    for (std::uint32_t index = 0; index < 4; ++index) {
        ok = ok && assets_.registerAsset(
            {{assets::AssetType::Sprite, spriteIds[index]},
             "desktop/sprite" + std::to_string(index) + ".rta",
             assets::CookedContentCodec::kSpriteSchema});
        const auto request = assets_.request(
            {assets::AssetType::Sprite, spriteIds[index]});
        ok = ok && request.valid();
        if (request.valid()) assetRequests_.push_back(request);
    }
    if (ok) assets_.process();
    return ok && std::all_of(
        std::begin(spriteIds), std::end(spriteIds),
        [this](std::uint64_t id) {
            return assets_.state({assets::AssetType::Sprite, id}) ==
                   assets::AssetState::Ready;
        });
}

bool PlayableDesktopRuntime::configureSimulation() {
    simulation_.setPlayerTeam(config_.playerTeam);
    simulation_.setResources(500);
    simulation_.setRequiredRoute(
        {config_.mapWidth - 1, config_.mapHeight / 2},
        {1, config_.mapHeight / 2});

    gameplay::BuildingDefinition tower;
    tower.id = 1;
    tower.cost = 60;
    tower.buildTicks = 6;
    tower.width = 2;
    tower.height = 2;
    tower.combat = {80, 1, 6, 6, 2, 0};
    tower.visionRange = 8;
    simulation_.registerBuilding(tower);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {16, 0, 2, 1, 3, 4};
    enemy.visionRange = 5;
    simulation_.registerUnit(enemy);

    gameplay::UnitDefinition soldier;
    soldier.id = 2;
    soldier.cost = 40;
    soldier.trainTicks = 5;
    soldier.cellsPerTick = 1;
    soldier.combat = {35, 1, 6, 4, 2, 0};
    soldier.visionRange = 8;
    simulation_.registerUnit(soldier);

    if (!simulation_.registerLane(
        {1, {config_.mapWidth - 2, config_.mapHeight / 2},
         {1, config_.mapHeight / 2}, 1})) return false;

    roguelite::ModifierDefinition salvage;
    salvage.id = 1;
    salvage.effects = {
        {roguelite::WaveCompletionResourceStat(),
         roguelite::ModifierOperation::Add, 25}
    };
    if (!simulation_.registerModifier(salvage)) return false;

    roguelite::ModifierDefinition compound;
    compound.id = 2;
    compound.effects = {
        {roguelite::WaveCompletionResourceStat(),
         roguelite::ModifierOperation::Multiply, 1250}
    };
    if (!simulation_.registerModifier(compound)) return false;

    tower_defense::WaveDefinition wave;
    wave.id = 1;
    wave.budget = 5;
    wave.enemyTeamId = config_.enemyTeam;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 1}};
    wave.rewardPool = {1, 2};
    wave.rewardChoices = 2;
    if (!simulation_.registerWave(wave)) return false;
    wave.id = 2;
    wave.budget = 8;
    if (!simulation_.registerWave(wave)) return false;
    wave.id = 3;
    wave.budget = 12;
    if (!simulation_.registerWave(wave)) return false;
    roguelite::RunDefinition run;
    run.id = 1;
    run.waves = {1, 2, 3};
    if (!simulation_.registerRun(std::move(run))) return false;

    gameplay::CombatStats core{180, 3, 0, 0, 1, 0};
    simulation_.createBaseCore(
        {1, config_.mapHeight / 2}, config_.playerTeam, core);
    gameplay::CombatStats defender{45, 1, 8, 7, 2, 0};
    simulation_.createDefender(
        {7, config_.mapHeight / 2}, {1}, config_.playerTeam, defender);
    simulation_.createDefender(
        {9, config_.mapHeight / 2 + 2}, {1}, config_.playerTeam, defender);
    submitRunCommand(roguelite::CommandType::StartRun, 1, 1);
    return true;
}

bool PlayableDesktopRuntime::configurePresentation() {
    bool ok = true;
    ok = ok && presentation_.registerFallbackVisual(
        presentation::SceneEntityKind::Unit, kPlayerSpriteId);
    ok = ok && presentation_.registerVisual(
        {{presentation::SceneEntityKind::Unit, 1}, kEnemySpriteId, 0,
         presentation::RenderLayer::WorldEntity, 0});
    ok = ok && presentation_.registerVisual(
        {{presentation::SceneEntityKind::Unit, 2}, kPlayerSpriteId, 0,
         presentation::RenderLayer::WorldEntity, 0});
    ok = ok && presentation_.registerVisual(
        {{presentation::SceneEntityKind::Building, 1}, kTowerSpriteId, 0,
         presentation::RenderLayer::WorldEntity, -2});
    ok = ok && presentation_.registerVisual(
        {{presentation::SceneEntityKind::Construction, 1},
         kConstructionSpriteId, 0,
         presentation::RenderLayer::WorldEntity, -1});
    return ok;
}

void PlayableDesktopRuntime::beforeSimulation(
    const runtime::DesktopFrameContext& frame) {
    presentationMilliseconds_ += static_cast<std::uint64_t>(
        std::max(0.0, frame.frameSeconds) * 1000.0);
    buildHud(frame);

    const auto& input = *frame.input;
    if (input.keyPressed(platform::KeyCode::F5)) (void)saveToMemory();
    if (input.keyPressed(platform::KeyCode::F9)) {
        (void)restoreFromMemory();
        return;
    }
    if (simulation_.state().phase == roguelite::RunPhase::Idle &&
        input.keyPressed(platform::KeyCode::Space)) {
        submitRunCommand(roguelite::CommandType::StartRun, 1, frame.targetTick);
    }
    if (simulation_.state().phase == roguelite::RunPhase::RewardPending) {
        const auto& choices = simulation_.tower().snapshot().rewardChoices;
        const platform::KeyCode keys[] = {
            platform::KeyCode::Digit1,
            platform::KeyCode::Digit2,
            platform::KeyCode::Digit3};
        for (std::size_t index = 0;
             index < choices.size() && index < 3; ++index) {
            if (input.keyPressed(keys[index])) {
                submitRunCommand(roguelite::CommandType::ChooseModifier,
                                 choices[index], frame.targetTick);
            }
        }
    }

    const auto controllerResult = controller_.update(
        input,
        simulation_.tower().rts().snapshot(),
        {frame.targetTick,
         static_cast<std::uint32_t>(
             std::max(0, frame.windowState.framebufferWidth)),
         static_cast<std::uint32_t>(
             std::max(0, frame.windowState.framebufferHeight)),
         static_cast<float>(frame.frameSeconds),
         pointerOverHud(frame)});
    for (auto command : controllerResult.commands) {
        if (simulation_.submitRts(std::move(command))) ++stats_.playerCommands;
        else ++stats_.rejectedCommands;
    }

    const auto& drag = controller_.selectionDrag();
    if (drag.active) {
        ui_.outline(
            {std::min(drag.startX, drag.currentX),
             std::min(drag.startY, drag.currentY),
             std::abs(drag.currentX - drag.startX),
             std::abs(drag.currentY - drag.startY)},
            {0.20f, 0.85f, 1.0f, 0.9f});
    }
}

void PlayableDesktopRuntime::stepSimulation(const sim::TickContext& tick) {
    if (!simulation_.step(tick.tick)) {
        statusMessage_ = "Simulation rejected tick";
        return;
    }
    ++stats_.simulationTicks;
    const auto& rts = simulation_.tower().rts();
    if (presentation_.publishSnapshot(rts.snapshot(), rts.events())) {
        publishedSnapshot_ = true;
        const auto cues = presentation_.consumeCues();
        playback_.consume(cues, presentationMilliseconds_);
    }
}

bool PlayableDesktopRuntime::renderFrame(
    const runtime::DesktopFrameContext& frame,
    const sim::FrameStepPlan& plan) {
    if (!publishedSnapshot_) {
        publishedSnapshot_ = presentation_.publishSnapshot(
            simulation_.tower().rts().snapshot());
    }
    if (!publishedSnapshot_) return false;
    auto packet = presentation_.buildRenderPacket(
        static_cast<float>(plan.alpha));
    playback_.apply(packet, presentationMilliseconds_);
    controller_.decorate(packet);
    return renderer_.render(
        {frame.window,
         static_cast<std::uint32_t>(frame.windowState.framebufferWidth),
         static_cast<std::uint32_t>(frame.windowState.framebufferHeight),
         0.025f, 0.035f, 0.055f, 1.0f},
        packet, controller_.camera(), &ui_.drawList());
}

void PlayableDesktopRuntime::buildHud(
    const runtime::DesktopFrameContext& frame) {
    const auto width = static_cast<std::uint32_t>(
        std::max(0, frame.windowState.framebufferWidth));
    const auto height = static_cast<std::uint32_t>(
        std::max(0, frame.windowState.framebufferHeight));
    ui_.begin(width, height, renderer_.whiteTexture(), uiInput(*frame.input));
    if (width == 0 || height == 0) return;

    ui_.panel({8, 8, 470, 78});
    std::ostringstream line;
    line << "Phase: " << phaseName(simulation_.state().phase)
         << "  Wave: " << simulation_.state().currentWave
         << "  Resources: " << simulation_.snapshot().availableResources;
    ui_.label(line.str(), 18, 18);
    line.str({});
    line.clear();
    line << "Selected: " << controller_.selection().size()
         << "  Mode: "
         << (controller_.mode() == rts_presentation::DesktopInteractionMode::Build
                 ? "BUILD"
                 : controller_.mode() ==
                       rts_presentation::DesktopInteractionMode::AttackMove
                       ? "ATTACK-MOVE" : "SELECT")
         << "  F5 Save / F9 Load";
    ui_.label(line.str(), 18, 38, 1.0f, ui_.style().mutedText);
    ui_.label(statusMessage_, 18, 58, 1.0f, ui_.style().mutedText);

    if (ui_.button(1, {490, 8, 110, 34}, "Build [B]")) {
        controller_.setMode(rts_presentation::DesktopInteractionMode::Build);
    }
    if (ui_.button(2, {608, 8, 130, 34}, "Attack [A]")) {
        controller_.setMode(
            rts_presentation::DesktopInteractionMode::AttackMove);
    }
    if (ui_.button(3, {746, 8, 86, 34}, "Save")) {
        (void)saveToMemory();
    }
    if (ui_.button(4, {840, 8, 86, 34}, "Load", hasMemorySave())) {
        (void)restoreFromMemory();
    }

    if (simulation_.state().phase == roguelite::RunPhase::RewardPending) {
        const auto& choices = simulation_.tower().snapshot().rewardChoices;
        ui_.panel({static_cast<float>(width) - 250.0f, 52.0f, 242.0f,
                   42.0f + choices.size() * 38.0f});
        ui_.label("Choose reward", static_cast<float>(width) - 238.0f, 62.0f);
        for (std::size_t index = 0; index < choices.size(); ++index) {
            const auto id = 100u + static_cast<std::uint64_t>(index);
            const auto text = std::string("[") + std::to_string(index + 1) +
                              "] Modifier " + std::to_string(choices[index]);
            if (ui_.button(id,
                           {static_cast<float>(width) - 238.0f,
                            84.0f + static_cast<float>(index) * 38.0f,
                            218.0f, 30.0f}, text)) {
                submitRunCommand(roguelite::CommandType::ChooseModifier,
                                 choices[index], frame.targetTick);
            }
        }
    }
}

void PlayableDesktopRuntime::submitRunCommand(
    roguelite::CommandType type,
    std::uint32_t objectId,
    std::uint64_t targetTick) {
    roguelite::TickCommand command;
    command.targetTick = targetTick;
    command.issuer = 1;
    command.sequence = nextRunSequence_++;
    if (nextRunSequence_ == 0) nextRunSequence_ = 1;
    command.type = type;
    command.objectId = objectId;
    if (!simulation_.submit(command)) ++stats_.rejectedCommands;
}

bool PlayableDesktopRuntime::pointerOverHud(
    const runtime::DesktopFrameContext& frame) const noexcept {
    if (!frame.input) return false;
    const auto& pointer = frame.input->pointer();
    const auto width = static_cast<float>(frame.windowState.framebufferWidth);
    return pointer.y < 98.0f || pointer.x > width - 260.0f;
}

const char* PlayableDesktopRuntime::phaseName(
    roguelite::RunPhase phase) noexcept {
    switch (phase) {
    case roguelite::RunPhase::Idle: return "Idle";
    case roguelite::RunPhase::BetweenWaves: return "Preparing";
    case roguelite::RunPhase::WaveActive: return "Wave";
    case roguelite::RunPhase::RewardPending: return "Reward";
    case roguelite::RunPhase::Complete: return "Complete";
    case roguelite::RunPhase::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace rts::rts_desktop
