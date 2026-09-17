#version 450

/* Straight resolve of the submitted frame. Nearest or linear is the sampler's
 * business, not the shader's, so one pipeline serves both filters. */

layout(location = 0) in vec2 fragment_tex_coord;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source_texture;

void main(void) {
    color = texture(source_texture, fragment_tex_coord);
}
