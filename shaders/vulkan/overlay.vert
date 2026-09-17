#version 450

/* Overlay layers use the same clip-space quad as the frame blit; the layer's
 * destination rectangle is applied as a viewport rather than in the vertex
 * data, so one static vertex buffer serves every layer. */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 tex_coord;

layout(location = 0) out vec2 fragment_tex_coord;

void main(void) {
    gl_Position = vec4(position, 0.0, 1.0);
    fragment_tex_coord = tex_coord;
}
