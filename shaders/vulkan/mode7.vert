#version 450

/* The semantic Mode 7 pass is addressed entirely from gl_FragCoord, so the
 * vertex stage only has to cover the offscreen target. */

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 tex_coord;

void main(void) {
    gl_Position = vec4(position, 0.0, 1.0);
}
