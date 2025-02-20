// Generated code, do not modify this file!
// Generated on 2023-04-04T01:24:40.539Z by mwilsnd using shaders/generate_shader_code.js

#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::FillProgram, gfx::Backend::Type::OpenGL> {
    static constexpr const char* vertex = R"(attribute vec2 a_pos;

uniform mat4 u_matrix;

#ifndef HAS_UNIFORM_u_color
uniform lowp float u_color_t;
attribute highp vec4 a_color;
varying highp vec4 color;
#else
uniform highp vec4 u_color;
#endif
#ifndef HAS_UNIFORM_u_opacity
uniform lowp float u_opacity_t;
attribute lowp vec2 a_opacity;
varying lowp float opacity;
#else
uniform lowp float u_opacity;
#endif

void main() {
    #ifndef HAS_UNIFORM_u_color
color = unpack_mix_color(a_color, u_color_t);
#else
highp vec4 color = u_color;
#endif
    #ifndef HAS_UNIFORM_u_opacity
opacity = unpack_mix_vec2(a_opacity, u_opacity_t);
#else
lowp float opacity = u_opacity;
#endif

    gl_Position = u_matrix * vec4(a_pos, 0, 1);
}
)";
    static constexpr const char* fragment = R"(#ifndef HAS_UNIFORM_u_color
varying highp vec4 color;
#else
uniform highp vec4 u_color;
#endif
#ifndef HAS_UNIFORM_u_opacity
varying lowp float opacity;
#else
uniform lowp float u_opacity;
#endif

void main() {
    #ifdef HAS_UNIFORM_u_color
highp vec4 color = u_color;
#endif
    #ifdef HAS_UNIFORM_u_opacity
lowp float opacity = u_opacity;
#endif

    gl_FragColor = color * opacity;

#ifdef OVERDRAW_INSPECTOR
    gl_FragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mbgl
