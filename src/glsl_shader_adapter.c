#include "snesrecomp_platform/glsl_shader_adapter.h"

#include <stdio.h>

#include "glsl_shader.h"

static void *preset_create(
    const char *path,
    char *error,
    size_t error_size) {
    GlslShader *shader = GlslShader_CreateFromFile(path);
    if (!shader && error && error_size > 0) {
        snprintf(
            error,
            error_size,
            "could not load GLSL preset: %s",
            path ? path : "(null)");
    }
    return shader;
}

static void preset_destroy(void *preset) {
    GlslShader_Destroy((GlslShader *)preset);
}

static void preset_render(
    void *preset,
    uint32_t source_texture,
    int source_width,
    int source_height,
    int viewport_x,
    int viewport_y,
    int viewport_width,
    int viewport_height) {
    GlTextureWithSize texture;
    texture.gl_texture = source_texture;
    texture.width = (uint16)source_width;
    texture.height = (uint16)source_height;
    GlslShader_Render(
        (GlslShader *)preset,
        &texture,
        viewport_x,
        viewport_y,
        viewport_width,
        viewport_height);
}

const SnesRecompShaderPresetInterface *
snesrecomp_glsl_shader_preset_interface(void) {
    static const SnesRecompShaderPresetInterface interface = {
        preset_create,
        preset_destroy,
        preset_render,
    };
    return &interface;
}
