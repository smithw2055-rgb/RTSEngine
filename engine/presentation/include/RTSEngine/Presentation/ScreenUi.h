#pragma once

#include <RTSEngine/Render/RenderDevice.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rts::presentation {

struct UiRect final {
    float x{};
    float y{};
    float width{};
    float height{};

    bool contains(float px, float py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

struct UiColor final {
    float red{1.0f};
    float green{1.0f};
    float blue{1.0f};
    float alpha{1.0f};
};

struct UiQuad final {
    render::TextureHandle texture{};
    UiRect rect{};
    float u0{};
    float v0{};
    float u1{1.0f};
    float v1{1.0f};
    UiColor color{};
    render::BlendMode blend{render::BlendMode::Alpha};
    std::uint64_t sortKey{};
};

struct UiDrawList final {
    std::uint32_t framebufferWidth{};
    std::uint32_t framebufferHeight{};
    std::vector<UiQuad> quads;

    bool valid() const noexcept {
        return framebufferWidth != 0 && framebufferHeight != 0;
    }

    void clear(std::uint32_t width, std::uint32_t height) {
        framebufferWidth = width;
        framebufferHeight = height;
        quads.clear();
    }
};

struct UiInput final {
    float pointerX{};
    float pointerY{};
    bool leftDown{};
    bool leftPressed{};
    bool leftReleased{};
};

class BuiltinFont final {
public:
    explicit BuiltinFont(render::RenderDevice& device) noexcept;
    ~BuiltinFont();

    BuiltinFont(const BuiltinFont&) = delete;
    BuiltinFont& operator=(const BuiltinFont&) = delete;

    bool initialize();
    void shutdown() noexcept;
    bool initialized() const noexcept;
    render::TextureHandle texture() const noexcept;

    float measure(std::string_view utf8, float scale = 1.0f) const noexcept;
    void append(UiDrawList& output,
                std::string_view utf8,
                float x,
                float y,
                float scale,
                UiColor color,
                std::uint64_t sortBase = 0) const;

    static constexpr float glyphWidth() noexcept { return 8.0f; }
    static constexpr float glyphHeight() noexcept { return 12.0f; }

private:
    bool ensureGeneration();
    static std::vector<std::uint32_t> decodeUtf8(std::string_view text);

    render::RenderDevice& device_;
    render::TextureHandle texture_{};
    std::uint32_t deviceGeneration_{};
};

struct MinimalUiStyle final {
    UiColor panel{0.06f, 0.08f, 0.12f, 0.92f};
    UiColor panelBorder{0.25f, 0.35f, 0.46f, 1.0f};
    UiColor text{0.92f, 0.96f, 1.0f, 1.0f};
    UiColor mutedText{0.62f, 0.70f, 0.78f, 1.0f};
    UiColor button{0.13f, 0.22f, 0.32f, 1.0f};
    UiColor buttonHover{0.20f, 0.34f, 0.48f, 1.0f};
    UiColor buttonPressed{0.08f, 0.16f, 0.24f, 1.0f};
    UiColor progressBack{0.03f, 0.04f, 0.06f, 0.9f};
    UiColor progressFill{0.20f, 0.78f, 0.38f, 1.0f};
    float padding{8.0f};
};

class MinimalUi final {
public:
    explicit MinimalUi(render::RenderDevice& device) noexcept;

    bool initialize();
    void shutdown() noexcept;
    void begin(std::uint32_t framebufferWidth,
               std::uint32_t framebufferHeight,
               render::TextureHandle whiteTexture,
               UiInput input);

    void panel(UiRect rect, UiColor color = {});
    void label(std::string_view text,
               float x,
               float y,
               float scale = 1.0f,
               UiColor color = {});
    bool button(std::uint64_t id,
                UiRect rect,
                std::string_view text,
                bool enabled = true);
    void progressBar(UiRect rect,
                     float value,
                     UiColor fill = {});
    void outline(UiRect rect,
                 UiColor color,
                 float thickness = 1.0f);

    const UiDrawList& drawList() const noexcept;
    const MinimalUiStyle& style() const noexcept;
    MinimalUiStyle& style() noexcept;

private:
    void solid(UiRect rect, UiColor color, std::uint64_t sortKey);
    void border(UiRect rect, UiColor color, float thickness,
                std::uint64_t sortKey);

    BuiltinFont font_;
    render::TextureHandle whiteTexture_{};
    UiInput input_{};
    UiDrawList drawList_{};
    MinimalUiStyle style_{};
    std::uint64_t hotId_{};
    std::uint64_t activeId_{};
    std::uint64_t sortKey_{};
};

} // namespace rts::presentation
