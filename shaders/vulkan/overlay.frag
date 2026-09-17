#version 450

/* Overlay surfaces arrive premultiplied, so the fragment is emitted exactly
 * as sampled and the pipeline's blend state does the compositing:
 *
 *   srcColor = ONE, dstColor = ONE_MINUS_SRC_ALPHA
 *   srcAlpha = ONE, dstAlpha = ONE_MINUS_SRC_ALPHA
 *
 * Multiplying by alpha here would darken every layer a second time. */

layout(location = 0) in vec2 fragment_tex_coord;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D overlay_texture;

void main(void) {
    color = texture(overlay_texture, fragment_tex_coord);
}
