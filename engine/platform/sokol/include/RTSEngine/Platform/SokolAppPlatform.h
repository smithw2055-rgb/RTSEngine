#pragma once

#include <RTSEngine/Platform/Platform.h>

#include <chrono>
#include <cstdint>
#include <vector>

struct sapp_event;

namespace rts::platform {

class SokolAppPlatform final : public Platform {
public:
    SokolAppPlatform() noexcept;

    bool createWindow(const WindowDescription& description,
                      WindowHandle& output) override;
    bool destroyWindow(WindowHandle window) override;
    bool windowState(WindowHandle window,
                     WindowState& output) const override;
    void pollEvents(std::vector<PlatformEvent>& output) override;
    double monotonicSeconds() const noexcept override;

    void handleEvent(const sapp_event& event);

private:
    static KeyCode mapKey(std::uint32_t key) noexcept;
    static PointerButton mapButton(std::uint32_t button) noexcept;
    static std::uint32_t mapModifiers(std::uint32_t modifiers) noexcept;
    void refreshState() noexcept;
    void pushWindowEvent(PlatformEventType type);

    WindowDescription description_{};
    WindowState state_{};
    std::vector<PlatformEvent> events_;
    std::chrono::steady_clock::time_point started_;
    std::uint32_t generation_{1};
    bool alive_{};
};

} // namespace rts::platform
