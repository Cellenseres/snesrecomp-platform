#version 450

/*
 * Semantic Mode 7 compositor, ported from the OpenGL backend's fragment
 * shader. The behaviour is the OpenGL one; only the binding syntax is Vulkan.
 *
 * gl_FragCoord has its origin at the upper left here and at the lower left in
 * OpenGL, but the two backends also disagree about which end of the image the
 * quad puts at the top of the screen, and the two conventions cancel: row
 * zero of the offscreen target is SNES scanline zero and is drawn at the top
 * in both. The arithmetic below is therefore unchanged.
 *
 * One rename: the local flag the OpenGL source calls `half` is `half_math`
 * here, because `half` is a reserved word and glslang enforces that.
 */

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D map_texture;
layout(set = 0, binding = 1) uniform sampler2D char_texture;
layout(set = 0, binding = 2) uniform sampler2D palette_texture;
layout(set = 0, binding = 3) uniform sampler2D line_texture;
layout(set = 0, binding = 4) uniform sampler2D flags_texture;
layout(set = 0, binding = 5) uniform sampler2D obj_texture;
layout(set = 0, binding = 6) uniform sampler2D window_texture;
layout(set = 0, binding = 7) uniform sampler2D math_texture;

layout(push_constant) uniform Mode7Push {
    vec2 map_fixed_wrap;
    float hd_scale;
    float canvas_extra;
    float native_height;
    float bg_filter;
    float line_lerp;
    float reserved;
} pc;

float byte_value(float v) { return floor(v * 255.0 + 0.5); }

vec3 palette_rgb5(float index) {
    return floor(texelFetch(palette_texture,
        ivec2(int(index), 0), 0).rgb * 255.0 + 0.5);
}

vec2 wrap_texel(vec2 texel, vec2 limit) {
    return texel - limit * floor(texel / limit);
}

float map_index(vec2 texel) {
    ivec2 pixel = ivec2(floor(texel));
    ivec2 tile_xy = pixel / 8;
    int tile = int(byte_value(texelFetch(map_texture, tile_xy, 0).r));
    ivec2 in_tile = pixel - tile_xy * 8;
    int address = tile * 64 + in_tile.y * 8 + in_tile.x;
    return byte_value(texelFetch(char_texture,
        ivec2(address & 127, address >> 7), 0).r);
}

/* Nearest by default. Interpolating takes the plane between two native
   scanlines instead of repeating one, but only where they describe the same
   surface: near the horizon consecutive rows can sit further apart than the
   shortest wrap, and blending those would invent geometry. */
vec4 affine_row(float linef) {
    int l0 = clamp(int(floor(linef)), 0, int(pc.native_height) - 1);
    vec4 a0 = texelFetch(line_texture, ivec2(0, l0), 0);
    if (pc.line_lerp < 0.5) return a0;
    int l1 = clamp(l0 + 1, 0, int(pc.native_height) - 1);
    vec4 a1 = texelFetch(line_texture, ivec2(0, l1), 0);
    vec2 d = a1.xy - a0.xy;
    d -= pc.map_fixed_wrap * floor(d / pc.map_fixed_wrap + 0.5);
    if (any(greaterThan(abs(d), vec2(16384.0)))) return a0;
    float t = clamp(linef - float(l0), 0.0, 1.0);
    return vec4(a0.xy + d * t, mix(a0.zw, a1.zw, t));
}

vec3 brightness_rgb(vec3 c5, float brightness, bool half_math) {
    if (half_math) c5 = floor(c5 * 0.5);
    c5 = min(c5, vec3(31.0));
    vec3 c8 = c5 * 8.0 + floor(c5 * 0.25);
    return floor(c8 * brightness / 15.0) / 255.0;
}

void main(void) {
    int line = int(floor(gl_FragCoord.y / pc.hd_scale));
    line = clamp(line, 0, int(pc.native_height) - 1);
    vec4 flags = texelFetch(flags_texture, ivec2(0, line), 0);
    float brightness = byte_value(flags.r);
    float margin_left = byte_value(flags.g);
    int enables = int(byte_value(flags.b));
    float margin_right = byte_value(flags.a);
    if (brightness < 0.5) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    float local_hd_x = gl_FragCoord.x - pc.canvas_extra * pc.hd_scale;
    bool in_bg = local_hd_x >= -margin_left * pc.hd_scale &&
                 local_hd_x < (256.0 + margin_right) * pc.hd_scale;
    int native_xi = int(floor(gl_FragCoord.x / pc.hd_scale));
    int window_bits = int(byte_value(texelFetch(window_texture,
        ivec2(native_xi, line), 0).r));
    bool bg_window_visible = (window_bits & 1) != 0;
    bool obj_window_visible = (window_bits & 2) != 0;
    bool obj_sub_visible = (window_bits & 4) != 0;
    bool main_rgb_visible = (window_bits & 64) != 0;
    bool math_visible = (window_bits & 128) != 0;
    float bg_index = 0.0;
    vec3 bg_rgb5 = vec3(0.0);
    if (in_bg && bg_window_visible && (enables & 1) != 0) {
        float linef = pc.line_lerp > 0.5
            ? (gl_FragCoord.y + 0.5) / pc.hd_scale - 0.5 : float(line);
        vec4 affine = affine_row(linef);
        float native_x = (local_hd_x + 0.5) / pc.hd_scale - 0.5;
        vec2 limit = pc.map_fixed_wrap / 256.0;
        vec2 texel = (affine.xy + affine.zw * native_x) / 256.0;
        bg_index = map_index(wrap_texel(texel, limit));
        bg_rgb5 = palette_rgb5(bg_index);
        /* Tile numbers and palette indices cannot be blended, so every tap is
           resolved to colour first. Index zero is transparent and is left out
           rather than bleeding backdrop into the edge, and the nearest tap
           still decides priority and colour math -- only the visible colour
           changes. */
        if (pc.bg_filter > 0.5) {
            vec2 base = floor(texel - 0.5);
            vec2 f = texel - 0.5 - base;
            vec3 sum = vec3(0.0);
            float weight = 0.0;
            for (int j = 0; j < 4; j++) {
                vec2 off = vec2(float(j & 1), float(j >> 1));
                float w = (off.x > 0.5 ? f.x : 1.0 - f.x) *
                          (off.y > 0.5 ? f.y : 1.0 - f.y);
                float idx = map_index(wrap_texel(base + off, limit));
                if (idx > 0.5) { sum += palette_rgb5(idx) * w;
                                 weight += w; }
            }
            if (weight > 0.0) bg_rgb5 = sum / weight;
        }
    }
    vec2 obj = texelFetch(obj_texture, ivec2(native_xi, line), 0).rg;
    float obj_index = byte_value(obj.r);
    float obj_priority = byte_value(obj.g);
    bool obj_wins = obj_window_visible && (enables & 16) != 0 &&
                    obj_index > 0.5 &&
                    (bg_index < 0.5 || obj_priority > 0.5);
    float palette_index = obj_wins ? obj_index : bg_index;
    int source = obj_wins ? (obj_index >= 192.0 ? 4 : 6)
                          : (bg_index > 0.5 ? 0 : 5);
    vec3 result5 = main_rgb_visible
        ? (obj_wins ? palette_rgb5(palette_index) : bg_rgb5)
        : vec3(0.0);
    vec4 math0 = texelFetch(math_texture, ivec2(0, line), 0);
    vec4 math1 = texelFetch(math_texture, ivec2(1, line), 0);
    int cgadsub = int(byte_value(math0.r));
    int cgwsel = int(byte_value(math0.g));
    vec3 fixed5 = vec3(byte_value(math0.b),
                       byte_value(math0.a),
                       byte_value(math1.r));
    bool half_math = false;
    bool source_math = source < 6 && (cgadsub & (1 << source)) != 0;
    if (math_visible && source_math) {
        bool add_subscreen = (cgwsel & 2) != 0;
        bool sub_has_obj = add_subscreen && obj_sub_visible &&
                           obj_index > 0.5;
        vec3 second5 = sub_has_obj ? palette_rgb5(obj_index) : fixed5;
        half_math = (cgadsub & 64) != 0 &&
                    (!add_subscreen || sub_has_obj);
        if ((cgadsub & 128) != 0)
            result5 = max(result5 - second5, vec3(0.0));
        else
            result5 += second5;
    }
    color = vec4(brightness_rgb(result5, brightness, half_math), 1.0);
}
