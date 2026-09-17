#version 450

/*
 * Sharp-bilinear pass two. Most output pixels sample the exact centre of one
 * prescaled texel; a narrow, symmetric transition at a texel boundary absorbs
 * fractional-scale wobble without making the whole image look
 * bilinear-filtered. No colour transform and no sharpening is applied.
 *
 * input_size is the size of this pass input, texture_size the allocated size
 * of that input, and output_size the viewport being resolved into.
 */

layout(location = 0) in vec2 fragment_tex_coord;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(push_constant) uniform SharpBilinearPush {
    vec2 input_size;
    vec2 texture_size;
    vec2 output_size;
} pc;

void main(void) {
    vec2 texel = fragment_tex_coord * pc.texture_size;
    vec2 output_scale = max(pc.output_size / pc.input_size, vec2(1.0));

    /* Keep the blend to 0.65 output pixels across each boundary. Starting the
     * transition at the texel's left edge instead would soften and
     * asymmetrically shift far more of the image. */
    const vec2 blend_pixels = vec2(0.65);
    vec2 half_blend = 0.5 * blend_pixels / output_scale;
    vec2 centre_distance = fract(texel) - 0.5;
    vec2 hold_region = max(vec2(0.0), vec2(0.5) - half_blend);
    vec2 transition =
        (centre_distance -
         clamp(centre_distance, -hold_region, hold_region)) /
        (2.0 * half_blend) + 0.5;

    /* At exact integer scale there is no uneven-pixel problem to hide. Snap
     * completely to texel centres for a byte-stable nearest presentation. */
    vec2 integer_distance =
        abs(output_scale - floor(output_scale + vec2(0.5)));
    vec2 integer_axis = vec2(1.0) - step(vec2(0.0001), integer_distance);
    transition = mix(transition, vec2(0.5), integer_axis);

    vec2 sample_uv =
        (floor(texel) + clamp(transition, 0.0, 1.0)) / pc.texture_size;
    color = texture(source_texture, sample_uv);
}
