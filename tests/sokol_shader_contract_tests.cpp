#include <RTSEngine/Presentation/Fixed2DRenderer.h>
#include <RTSEngine/Presentation/SokolFixed2DShader.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace {

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

} // namespace

int main() {
    const auto shader =
        rts::presentation::MakeSokolFixed2DShaderDescription(
            rts::presentation::Fixed2DRenderer::kSpriteShaderKey);
    check(shader.vertex_func.source != nullptr);
    check(shader.fragment_func.source != nullptr);
    check(std::strstr(shader.vertex_func.source, "gl_Position") != nullptr);
    check(std::strstr(shader.fragment_func.source, "texture") != nullptr);
    check(shader.attrs[0].glsl_name != nullptr);
    check(shader.attrs[1].glsl_name != nullptr);
    check(shader.attrs[2].glsl_name != nullptr);
    check(shader.views[0].texture.stage == SG_SHADERSTAGE_FRAGMENT);
    check(shader.samplers[0].stage == SG_SHADERSTAGE_FRAGMENT);
    check(shader.texture_sampler_pairs[0].view_slot == 0);
    check(shader.texture_sampler_pairs[0].sampler_slot == 0);

    const auto unknown =
        rts::presentation::MakeSokolFixed2DShaderDescription(0);
    check(unknown.vertex_func.source == nullptr);
    check(unknown.fragment_func.source == nullptr);
    return 0;
}
