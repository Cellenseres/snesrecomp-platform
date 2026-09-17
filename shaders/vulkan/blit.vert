#version 450

/* The quad is a triangle strip in clip space. Vulkan's clip space has +Y
 * pointing down, so the vertex buffer pairs uv(0,0) with NDC(-1,-1): the
 * first row of an image is drawn at the top of the viewport, which is the
 * same thing the OpenGL backend's quad does with its own Y convention. */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 tex_coord;

layout(location = 0) out vec2 fragment_tex_coord;

void main(void) {
    gl_Position = vec4(position, 0.0, 1.0);
    fragment_tex_coord = tex_coord;
}
