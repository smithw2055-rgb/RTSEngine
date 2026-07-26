#pragma once

#include <RTSEngine/Render/RenderDevice.h>

#include <sokol_gfx.h>

namespace rts::presentation {

sg_shader_desc MakeSokolFixed2DShaderDescription(
    render::ShaderKey shaderKey) noexcept;

} // namespace rts::presentation
