#version 450

/* Sharp-bilinear pass one. Point-sampled into a target exactly twice the
 * source size, which replicates each source pixel into a 2x2 block and
 * establishes the integer texel grid pass two blends across. It changes no
 * colour; without it the blend would follow the source grid instead. */

layout(location = 0) in vec2 fragment_tex_coord;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source_texture;

void main(void) {
    color = texture(source_texture, fragment_tex_coord);
}
