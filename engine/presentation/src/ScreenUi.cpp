#include <RTSEngine/Presentation/ScreenUi.h>

#include "BuiltinFontData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rts::presentation {
namespace {

UiColor choose(UiColor requested, UiColor fallback) noexcept {
    const bool defaultWhite = requested.red == 1.0f &&
                              requested.green == 1.0f &&
                              requested.blue == 1.0f &&
                              requested.alpha == 1.0f;
    return defaultWhite ? fallback : requested;
}

} // namespace

BuiltinFont::BuiltinFont(render::RenderDevice& device) noexcept
    : device_(device) {}

BuiltinFont::~BuiltinFont() { shutdown(); }

bool BuiltinFont::initialize() {
    if (texture_.valid() && deviceGeneration_ == device_.deviceGeneration()) {
        return true;
    }
    shutdown();
    deviceGeneration_ = device_.deviceGeneration();
    texture_ = device_.createTexture(
        {detail::kBuiltinFontWidth,
         detail::kBuiltinFontHeight,
         render::TextureFormat::Rgba8,
         false,
         render::FilterMode::Nearest,
         render::AddressMode::Clamp,
         render::AddressMode::Clamp});
    if (!texture_.valid()) return false;

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(detail::kBuiltinFontWidth) *
        detail::kBuiltinFontHeight * 4u, 255u);
    for (std::size_t pixel = 0;
         pixel < static_cast<std::size_t>(detail::kBuiltinFontWidth) *
                     detail::kBuiltinFontHeight;
         ++pixel) {
        const auto byte = detail::kBuiltinFontBits[pixel / 8u];
        const bool set = (byte & (1u << (7u - pixel % 8u))) != 0;
        pixels[pixel * 4u + 3u] = set ? 255u : 0u;
    }
    if (!device_.updateTexture(texture_, pixels.data(), pixels.size())) {
        shutdown();
        return false;
    }
    return true;
}

void BuiltinFont::shutdown() noexcept {
    if (texture_.valid() && deviceGeneration_ == device_.deviceGeneration()) {
        (void)device_.destroyTexture(texture_);
    }
    texture_ = {};
    deviceGeneration_ = 0;
}

bool BuiltinFont::initialized() const noexcept {
    return texture_.valid() && deviceGeneration_ == device_.deviceGeneration();
}

render::TextureHandle BuiltinFont::texture() const noexcept { return texture_; }

float BuiltinFont::measure(std::string_view utf8, float scale) const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < utf8.size();) {
        const auto value = static_cast<unsigned char>(utf8[index]);
        if ((value & 0x80u) == 0) index += 1;
        else if ((value & 0xE0u) == 0xC0u && index + 1 < utf8.size()) index += 2;
        else if ((value & 0xF0u) == 0xE0u && index + 2 < utf8.size()) index += 3;
        else if ((value & 0xF8u) == 0xF0u && index + 3 < utf8.size()) index += 4;
        else index += 1;
        ++count;
    }
    return static_cast<float>(count) * glyphWidth() * std::max(0.0f, scale);
}

void BuiltinFont::append(UiDrawList& output,
                         std::string_view utf8,
                         float x,
                         float y,
                         float scale,
                         UiColor color,
                         std::uint64_t sortBase) const {
    if (!initialized() || !output.valid() || !(scale > 0.0f)) return;
    const auto codepoints = decodeUtf8(utf8);
    const auto cellWidth = static_cast<float>(detail::kBuiltinFontCellWidth);
    const auto cellHeight = static_cast<float>(detail::kBuiltinFontCellHeight);
    float cursor = x;
    std::uint64_t ordinal = 0;
    for (auto codepoint : codepoints) {
        if (codepoint == '\n') {
            cursor = x;
            y += cellHeight * scale;
            continue;
        }
        if (codepoint < 32u || codepoint > 126u) codepoint = '?';
        const auto glyph = codepoint - 32u;
        const auto column = glyph % 16u;
        const auto row = glyph / 16u;
        UiQuad quad;
        quad.texture = texture_;
        quad.rect = {cursor, y, cellWidth * scale, cellHeight * scale};
        quad.u0 = static_cast<float>(column * detail::kBuiltinFontCellWidth) /
                  static_cast<float>(detail::kBuiltinFontWidth);
        quad.v0 = static_cast<float>(row * detail::kBuiltinFontCellHeight) /
                  static_cast<float>(detail::kBuiltinFontHeight);
        quad.u1 = static_cast<float>((column + 1u) * detail::kBuiltinFontCellWidth) /
                  static_cast<float>(detail::kBuiltinFontWidth);
        quad.v1 = static_cast<float>((row + 1u) * detail::kBuiltinFontCellHeight) /
                  static_cast<float>(detail::kBuiltinFontHeight);
        quad.color = color;
        quad.sortKey = sortBase + ordinal++;
        output.quads.push_back(quad);
        cursor += cellWidth * scale;
    }
}

bool BuiltinFont::ensureGeneration() { return initialize(); }

std::vector<std::uint32_t> BuiltinFont::decodeUtf8(std::string_view text) {
    std::vector<std::uint32_t> output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::uint32_t codepoint = '?';
        std::size_t length = 1;
        if ((first & 0x80u) == 0) {
            codepoint = first;
        } else if ((first & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            codepoint = ((first & 0x1Fu) << 6u) |
                        (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            length = 2;
        } else if ((first & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            codepoint = ((first & 0x0Fu) << 12u) |
                        ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6u) |
                        (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            length = 3;
        } else if ((first & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            codepoint = ((first & 0x07u) << 18u) |
                        ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12u) |
                        ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6u) |
                        (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            length = 4;
        }
        output.push_back(codepoint);
        i += length;
    }
    return output;
}

MinimalUi::MinimalUi(render::RenderDevice& device) noexcept : font_(device) {}

bool MinimalUi::initialize() { return font_.initialize(); }
void MinimalUi::shutdown() noexcept { font_.shutdown(); drawList_ = {}; }

void MinimalUi::begin(std::uint32_t framebufferWidth,
                      std::uint32_t framebufferHeight,
                      render::TextureHandle whiteTexture,
                      UiInput input) {
    (void)font_.initialize();
    drawList_.clear(framebufferWidth, framebufferHeight);
    whiteTexture_ = whiteTexture;
    input_ = input;
    hotId_ = 0;
    sortKey_ = 1;
    if (!input_.leftDown && !input_.leftPressed && !input_.leftReleased) {
        activeId_ = 0;
    }
}

void MinimalUi::panel(UiRect rect, UiColor color) {
    color = choose(color, style_.panel);
    solid(rect, color, sortKey_++);
    border(rect, style_.panelBorder, 1.0f, sortKey_++);
}

void MinimalUi::label(std::string_view text,
                      float x,
                      float y,
                      float scale,
                      UiColor color) {
    color = choose(color, style_.text);
    font_.append(drawList_, text, x, y, scale, color, sortKey_);
    sortKey_ += static_cast<std::uint64_t>(text.size() + 1u);
}

bool MinimalUi::button(std::uint64_t id,
                       UiRect rect,
                       std::string_view text,
                       bool enabled) {
    const bool hovered = enabled && rect.contains(input_.pointerX, input_.pointerY);
    if (hovered) hotId_ = id;
    if (hovered && input_.leftPressed) activeId_ = id;
    const bool active = enabled && activeId_ == id && input_.leftDown;
    const bool clicked = enabled && activeId_ == id && hovered && input_.leftReleased;
    if (input_.leftReleased && activeId_ == id) activeId_ = 0;

    UiColor color = enabled ? style_.button : UiColor{0.10f, 0.11f, 0.13f, 0.8f};
    if (active) color = style_.buttonPressed;
    else if (hovered) color = style_.buttonHover;
    solid(rect, color, sortKey_++);
    border(rect, style_.panelBorder, 1.0f, sortKey_++);
    const auto textWidth = font_.measure(text, 1.0f);
    label(text,
          rect.x + std::max(0.0f, (rect.width - textWidth) * 0.5f),
          rect.y + std::max(0.0f, (rect.height - BuiltinFont::glyphHeight()) * 0.5f),
          1.0f,
          enabled ? style_.text : style_.mutedText);
    return clicked;
}

void MinimalUi::progressBar(UiRect rect, float value, UiColor fill) {
    fill = choose(fill, style_.progressFill);
    solid(rect, style_.progressBack, sortKey_++);
    const auto clamped = std::clamp(value, 0.0f, 1.0f);
    solid({rect.x, rect.y, rect.width * clamped, rect.height},
          fill, sortKey_++);
    border(rect, style_.panelBorder, 1.0f, sortKey_++);
}

void MinimalUi::outline(UiRect rect, UiColor color, float thickness) {
    border(rect, color, thickness, sortKey_++);
}

const UiDrawList& MinimalUi::drawList() const noexcept { return drawList_; }
const MinimalUiStyle& MinimalUi::style() const noexcept { return style_; }
MinimalUiStyle& MinimalUi::style() noexcept { return style_; }

void MinimalUi::solid(UiRect rect, UiColor color, std::uint64_t sortKey) {
    if (!whiteTexture_.valid() || rect.width <= 0.0f || rect.height <= 0.0f) return;
    drawList_.quads.push_back(
        {whiteTexture_, rect, 0.0f, 0.0f, 1.0f, 1.0f,
         color, render::BlendMode::Alpha, sortKey});
}

void MinimalUi::border(UiRect rect,
                       UiColor color,
                       float thickness,
                       std::uint64_t sortKey) {
    if (!(thickness > 0.0f)) return;
    solid({rect.x, rect.y, rect.width, thickness}, color, sortKey);
    solid({rect.x, rect.y + rect.height - thickness, rect.width, thickness},
          color, sortKey + 1u);
    solid({rect.x, rect.y, thickness, rect.height}, color, sortKey + 2u);
    solid({rect.x + rect.width - thickness, rect.y, thickness, rect.height},
          color, sortKey + 3u);
    sortKey_ = std::max(sortKey_, sortKey + 4u);
}

} // namespace rts::presentation
