#include <RTSEngine/Presentation/SokolFixed2DShader.h>

#include <RTSEngine/Presentation/Fixed2DRenderer.h>

namespace rts::presentation {

sg_shader_desc MakeSokolFixed2DShaderDescription(
    render::ShaderKey shaderKey) noexcept {
    sg_shader_desc description{};
    if (shaderKey != Fixed2DRenderer::kSpriteShaderKey) {
        return description;
    }

    description.vertex_func.source = R"(
        #version 410
        layout(location=0) in vec2 position;
        layout(location=1) in vec2 texcoord0;
        layout(location=2) in vec4 color0;
        out vec2 uv;
        out vec4 color;
        void main() {
            gl_Position = vec4(position, 0.0, 1.0);
            uv = texcoord0;
            color = color0;
        }
    )";
    description.fragment_func.source = R"(
        #version 410
        uniform sampler2D tex_smp;
        in vec2 uv;
        in vec4 color;
        out vec4 frag_color;
        void main() {
            frag_color = texture(tex_smp, uv) * color;
        }
    )";
    description.attrs[0].glsl_name = "position";
    description.attrs[1].glsl_name = "texcoord0";
    description.attrs[2].glsl_name = "color0";
    description.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    description.views[0].texture.image_type = SG_IMAGETYPE_2D;
    description.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    description.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    description.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    description.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    description.texture_sampler_pairs[0].view_slot = 0;
    description.texture_sampler_pairs[0].sampler_slot = 0;
    description.texture_sampler_pairs[0].glsl_name = "tex_smp";
    description.label = "rts-fixed-2d-sprite";
    return description;
}

} // namespace rts::presentation
