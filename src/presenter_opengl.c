#include "snesrecomp_platform/presenter_backend.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"

#include "gl_core_3_1.h"
#include <SDL3/SDL.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct OpenGlPresenterContext {
    SDL_Window *window;
    SDL_GLContext gl_context;
    GLuint texture;
    GLuint vertex_array;
    GLuint vertex_buffer;
    GLuint program;
    SnesRecompPixelFormat texture_format;
    int texture_width;
    int texture_height;
    bool preserve_aspect;
    bool linear_filtering;
    const SnesRecompShaderPresetInterface *preset_interface;
    void *preset;

    /* Lazily created so the authentic path allocates and executes exactly the
     * same resources and calls as before when HD Mode 7 is off. */
    GLuint mode7_program;
    GLuint mode7_fbo;
    GLuint mode7_target;
    GLuint mode7_map_texture;
    GLuint mode7_char_texture;
    GLuint mode7_palette_texture;
    GLuint mode7_line_texture;
    GLuint mode7_flags_texture;
    GLuint mode7_obj_texture;
    GLuint mode7_window_texture;
    GLuint mode7_math_texture;
    int mode7_target_width;
    int mode7_target_height;
    uint8_t *mode7_obj_pixels;
    size_t mode7_obj_capacity;
    uint8_t *mode7_window_pixels;
    size_t mode7_window_capacity;
} OpenGlPresenterContext;

static bool set_sdl_error(
    SnesRecompPresenter *presenter,
    const char *operation) {
    const char *detail = SDL_GetError();
    snesrecomp_presenter_set_error(
        presenter,
        "%s failed: %s",
        operation,
        detail && detail[0] ? detail : "unknown SDL error");
    return false;
}

static bool make_current(SnesRecompPresenter *presenter) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!SDL_GL_MakeCurrent(context->window, context->gl_context))
        return set_sdl_error(presenter, "SDL_GL_MakeCurrent");
    return true;
}

static bool compile_shader(
    SnesRecompPresenter *presenter,
    GLenum type,
    const char *source,
    GLuint *out_shader) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        snesrecomp_presenter_set_error(
            presenter, "glCreateShader returned zero");
        return false;
    }

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[512];
        GLsizei length = 0;
        glGetShaderInfoLog(
            shader, (GLsizei)sizeof(log), &length, log);
        log[sizeof(log) - 1] = '\0';
        snesrecomp_presenter_set_error(
            presenter,
            "%s shader compilation failed: %s",
            type == GL_VERTEX_SHADER ? "vertex" : "fragment",
            length > 0 ? log : "no driver log");
        glDeleteShader(shader);
        return false;
    }

    *out_shader = shader;
    return true;
}

static bool create_program(SnesRecompPresenter *presenter) {
    static const char vertex_source[] =
        "#version 330 core\n"
        "layout(location = 0) in vec2 position;\n"
        "layout(location = 1) in vec2 tex_coord;\n"
        "out vec2 fragment_tex_coord;\n"
        "void main(void) {\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  fragment_tex_coord = tex_coord;\n"
        "}\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "in vec2 fragment_tex_coord;\n"
        "out vec4 color;\n"
        "uniform sampler2D source_texture;\n"
        "void main(void) {\n"
        "  color = texture(source_texture, fragment_tex_coord);\n"
        "}\n";

    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    GLuint vertex_shader = 0;
    GLuint fragment_shader = 0;
    if (!compile_shader(
            presenter,
            GL_VERTEX_SHADER,
            vertex_source,
            &vertex_shader)) {
        return false;
    }
    if (!compile_shader(
            presenter,
            GL_FRAGMENT_SHADER,
            fragment_source,
            &fragment_shader)) {
        glDeleteShader(vertex_shader);
        return false;
    }

    context->program = glCreateProgram();
    glAttachShader(context->program, vertex_shader);
    glAttachShader(context->program, fragment_shader);
    glLinkProgram(context->program);

    GLint linked = GL_FALSE;
    glGetProgramiv(context->program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512];
        GLsizei length = 0;
        glGetProgramInfoLog(
            context->program, (GLsizei)sizeof(log), &length, log);
        log[sizeof(log) - 1] = '\0';
        snesrecomp_presenter_set_error(
            presenter,
            "OpenGL program link failed: %s",
            length > 0 ? log : "no driver log");
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    glDetachShader(context->program, vertex_shader);
    glDetachShader(context->program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    glUseProgram(context->program);
    const GLint sampler =
        glGetUniformLocation(context->program, "source_texture");
    if (sampler >= 0)
        glUniform1i(sampler, 0);
    return true;
}

static bool link_mode7_program(SnesRecompPresenter *presenter) {
    static const char vertex_source[] =
        "#version 330 core\n"
        "layout(location = 0) in vec2 position;\n"
        "void main(void) { gl_Position = vec4(position, 0.0, 1.0); }\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "out vec4 color;\n"
        "uniform sampler2D map_texture;\n"
        "uniform sampler2D char_texture;\n"
        "uniform sampler2D palette_texture;\n"
        "uniform sampler2D line_texture;\n"
        "uniform sampler2D flags_texture;\n"
        "uniform sampler2D obj_texture;\n"
        "uniform sampler2D window_texture;\n"
        "uniform sampler2D math_texture;\n"
        "uniform float hd_scale;\n"
        "uniform float canvas_extra;\n"
        "uniform float native_height;\n"
        "uniform vec2 map_fixed_wrap;\n"
        "float byte_value(float v) { return floor(v * 255.0 + 0.5); }\n"
        "vec3 palette_rgb5(float index) {\n"
        "  return floor(texelFetch(palette_texture,\n"
        "      ivec2(int(index), 0), 0).rgb * 255.0 + 0.5);\n"
        "}\n"
        "vec3 brightness_rgb(vec3 c5, float brightness, bool half) {\n"
        "  if (half) c5 = floor(c5 * 0.5);\n"
        "  c5 = min(c5, vec3(31.0));\n"
        "  vec3 c8 = c5 * 8.0 + floor(c5 * 0.25);\n"
        "  return floor(c8 * brightness / 15.0) / 255.0;\n"
        "}\n"
        "void main(void) {\n"
        "  int line = int(floor(gl_FragCoord.y / hd_scale));\n"
        "  line = clamp(line, 0, int(native_height) - 1);\n"
        "  vec4 flags = texelFetch(flags_texture, ivec2(0, line), 0);\n"
        "  float brightness = byte_value(flags.r);\n"
        "  float margin_left = byte_value(flags.g);\n"
        "  int enables = int(byte_value(flags.b));\n"
        "  float margin_right = byte_value(flags.a);\n"
        "  if (brightness < 0.5) { color = vec4(0,0,0,1); return; }\n"
        "  float local_hd_x = gl_FragCoord.x - canvas_extra * hd_scale;\n"
        "  bool in_bg = local_hd_x >= -margin_left * hd_scale &&\n"
        "               local_hd_x < (256.0 + margin_right) * hd_scale;\n"
        "  int native_xi = int(floor(gl_FragCoord.x / hd_scale));\n"
        "  int window_bits = int(byte_value(texelFetch(window_texture,\n"
        "      ivec2(native_xi, line), 0).r));\n"
        "  bool bg_window_visible = (window_bits & 1) != 0;\n"
        "  bool obj_window_visible = (window_bits & 2) != 0;\n"
        "  bool obj_sub_visible = (window_bits & 4) != 0;\n"
        "  bool main_rgb_visible = (window_bits & 64) != 0;\n"
        "  bool math_visible = (window_bits & 128) != 0;\n"
        "  float bg_index = 0.0;\n"
        "  if (in_bg && bg_window_visible && (enables & 1) != 0) {\n"
        "    vec4 affine = texelFetch(line_texture, ivec2(0, line), 0);\n"
        "    float native_x = (local_hd_x + 0.5) / hd_scale - 0.5;\n"
        "    vec2 fixed_coord = floor(affine.xy + affine.zw * native_x);\n"
        "    fixed_coord -= floor(fixed_coord / map_fixed_wrap) * map_fixed_wrap;\n"
        "    ivec2 pixel = ivec2(floor(fixed_coord / 256.0));\n"
        "    ivec2 tile_xy = pixel / 8;\n"
        "    int tile = int(byte_value(texelFetch(map_texture, tile_xy, 0).r));\n"
        "    ivec2 in_tile = pixel - tile_xy * 8;\n"
        "    int address = tile * 64 + in_tile.y * 8 + in_tile.x;\n"
        "    bg_index = byte_value(texelFetch(char_texture,\n"
        "        ivec2(address & 127, address >> 7), 0).r);\n"
        "  }\n"
        "  vec2 obj = texelFetch(obj_texture, ivec2(native_xi, line), 0).rg;\n"
        "  float obj_index = byte_value(obj.r);\n"
        "  float obj_priority = byte_value(obj.g);\n"
        "  bool obj_wins = obj_window_visible && (enables & 16) != 0 &&\n"
        "                  obj_index > 0.5 &&\n"
        "                  (bg_index < 0.5 || obj_priority > 0.5);\n"
        "  float palette_index = obj_wins ? obj_index : bg_index;\n"
        "  int source = obj_wins ? (obj_index >= 192.0 ? 4 : 6)\n"
        "                        : (bg_index > 0.5 ? 0 : 5);\n"
        "  vec3 result5 = main_rgb_visible\n"
        "      ? palette_rgb5(palette_index) : vec3(0.0);\n"
        "  vec4 math0 = texelFetch(math_texture, ivec2(0, line), 0);\n"
        "  vec4 math1 = texelFetch(math_texture, ivec2(1, line), 0);\n"
        "  int cgadsub = int(byte_value(math0.r));\n"
        "  int cgwsel = int(byte_value(math0.g));\n"
        "  vec3 fixed5 = vec3(byte_value(math0.b),\n"
        "                    byte_value(math0.a),\n"
        "                    byte_value(math1.r));\n"
        "  bool half = false;\n"
        "  bool source_math = source < 6 &&\n"
        "      (cgadsub & (1 << source)) != 0;\n"
        "  if (math_visible && source_math) {\n"
        "    bool add_subscreen = (cgwsel & 2) != 0;\n"
        "    bool sub_has_obj = add_subscreen && obj_sub_visible &&\n"
        "                       obj_index > 0.5;\n"
        "    vec3 second5 = sub_has_obj ? palette_rgb5(obj_index) : fixed5;\n"
        "    half = (cgadsub & 64) != 0 &&\n"
        "           (!add_subscreen || sub_has_obj);\n"
        "    if ((cgadsub & 128) != 0)\n"
        "      result5 = max(result5 - second5, vec3(0.0));\n"
        "    else\n"
        "      result5 += second5;\n"
        "  }\n"
        "  color = vec4(brightness_rgb(result5, brightness, half), 1.0);\n"
        "}\n";
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    GLuint vs = 0, fs = 0;
    GLint linked = GL_FALSE;

    if (!compile_shader(presenter, GL_VERTEX_SHADER, vertex_source, &vs) ||
        !compile_shader(presenter, GL_FRAGMENT_SHADER, fragment_source, &fs)) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    context->mode7_program = glCreateProgram();
    glAttachShader(context->mode7_program, vs);
    glAttachShader(context->mode7_program, fs);
    glLinkProgram(context->mode7_program);
    glGetProgramiv(context->mode7_program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (linked != GL_TRUE) {
        char log[512];
        GLsizei length = 0;
        glGetProgramInfoLog(context->mode7_program, (GLsizei)sizeof(log),
                            &length, log);
        log[sizeof(log) - 1] = '\0';
        snesrecomp_presenter_set_error(
            presenter, "HD Mode 7 program link failed: %s",
            length > 0 ? log : "no driver log");
        return false;
    }

    glUseProgram(context->mode7_program);
    static const char *const samplers[] = {
        "map_texture", "char_texture", "palette_texture", "line_texture",
        "flags_texture", "obj_texture", "window_texture", "math_texture",
    };
    for (unsigned i = 0; i < sizeof samplers / sizeof samplers[0]; i++) {
        const GLint location =
            glGetUniformLocation(context->mode7_program, samplers[i]);
        if (location >= 0)
            glUniform1i(location, (GLint)i);
    }
    return true;
}

static void mode7_texture_parameters(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool create_mode7_resources(SnesRecompPresenter *presenter) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    GLuint textures[8] = {0};

    if (context->mode7_program)
        return true;
    if (!link_mode7_program(presenter))
        return false;
    glGenFramebuffers(1, &context->mode7_fbo);
    glGenTextures(1, &context->mode7_target);
    glGenTextures(8, textures);
    context->mode7_map_texture = textures[0];
    context->mode7_char_texture = textures[1];
    context->mode7_palette_texture = textures[2];
    context->mode7_line_texture = textures[3];
    context->mode7_flags_texture = textures[4];
    context->mode7_obj_texture = textures[5];
    context->mode7_window_texture = textures[6];
    context->mode7_math_texture = textures[7];
    if (!context->mode7_fbo || !context->mode7_target ||
        !context->mode7_map_texture || !context->mode7_char_texture ||
        !context->mode7_palette_texture || !context->mode7_line_texture ||
        !context->mode7_flags_texture || !context->mode7_obj_texture ||
        !context->mode7_window_texture || !context->mode7_math_texture) {
        snesrecomp_presenter_set_error(
            presenter, "HD Mode 7 OpenGL resource allocation failed");
        return false;
    }
    return true;
}

static bool create_geometry(SnesRecompPresenter *presenter) {
    static const GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
    };
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;

    glGenVertexArrays(1, &context->vertex_array);
    glGenBuffers(1, &context->vertex_buffer);
    if (!context->vertex_array || !context->vertex_buffer) {
        snesrecomp_presenter_set_error(
            presenter, "OpenGL geometry allocation failed");
        return false;
    }

    glBindVertexArray(context->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, context->vertex_buffer);
    glBufferData(
        GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * (GLsizei)sizeof(GLfloat),
        (void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    return true;
}

static bool allocate_texture(
    SnesRecompPresenter *presenter,
    SnesRecompPixelFormat format,
    int width,
    int height,
    const void *pixels,
    int pitch) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (format != SNESRECOMP_PIXEL_FORMAT_ARGB8888) {
        snesrecomp_presenter_set_error(
            presenter, "unsupported OpenGL pixel format");
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, context->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pixels ? pitch / 4 : 0);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        width,
        height,
        0,
        GL_BGRA,
        GL_UNSIGNED_INT_8_8_8_8_REV,
        pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLint filter =
        context->linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    context->texture_format = format;
    context->texture_width = width;
    context->texture_height = height;
    presenter->frame_width = width;
    presenter->frame_height = height;
    return true;
}

static void opengl_destroy(SnesRecompPresenter *presenter) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!context)
        return;

    if (context->gl_context && context->window)
        (void)SDL_GL_MakeCurrent(context->window, context->gl_context);
    if (context->preset && context->preset_interface &&
        context->preset_interface->destroy) {
        context->preset_interface->destroy(context->preset);
    }
    free(context->mode7_obj_pixels);
    free(context->mode7_window_pixels);
    if (context->mode7_program)
        glDeleteProgram(context->mode7_program);
    if (context->mode7_fbo)
        glDeleteFramebuffers(1, &context->mode7_fbo);
    {
        const GLuint mode7_textures[] = {
            context->mode7_target,
            context->mode7_map_texture,
            context->mode7_char_texture,
            context->mode7_palette_texture,
            context->mode7_line_texture,
            context->mode7_flags_texture,
            context->mode7_obj_texture,
            context->mode7_window_texture,
            context->mode7_math_texture,
        };
        glDeleteTextures(
            (GLsizei)(sizeof mode7_textures / sizeof mode7_textures[0]),
            mode7_textures);
    }
    if (context->program)
        glDeleteProgram(context->program);
    if (context->vertex_buffer)
        glDeleteBuffers(1, &context->vertex_buffer);
    if (context->vertex_array)
        glDeleteVertexArrays(1, &context->vertex_array);
    if (context->texture)
        glDeleteTextures(1, &context->texture);
    if (context->gl_context)
        (void)SDL_GL_DestroyContext(context->gl_context);
    if (context->window)
        SDL_DestroyWindow(context->window);
    free(context);
    presenter->context = NULL;
}

static bool opengl_present_texture(
    SnesRecompPresenter *presenter, GLuint texture,
    int source_width, int source_height,
    int logical_width, int logical_height) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    int drawable_width = 0, drawable_height = 0;
    int viewport_width, viewport_height, viewport_x, viewport_y;

    if (!SDL_GetWindowSizeInPixels(
            context->window, &drawable_width, &drawable_height)) {
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    }
    if (drawable_width <= 0 || drawable_height <= 0)
        return true;

    viewport_width = drawable_width;
    viewport_height = drawable_height;
    if (context->preserve_aspect) {
        const int display_width =
            snesrecomp_presenter_display_width(presenter, logical_width);
        if ((int64_t)viewport_width * logical_height <
            (int64_t)viewport_height * display_width) {
            viewport_height = viewport_width * logical_height / display_width;
        } else {
            viewport_width = viewport_height * display_width / logical_height;
        }
    }
    viewport_x = (drawable_width - viewport_width) / 2;
    viewport_y = (drawable_height - viewport_height) / 2;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (context->preset) {
        context->preset_interface->render(
            context->preset, texture, source_width, source_height,
            viewport_x, viewport_y, viewport_width, viewport_height);
    } else {
        glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUseProgram(context->program);
        glBindVertexArray(context->vertex_array);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    {
        const GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            snesrecomp_presenter_set_error(
                presenter, "OpenGL present failed with error 0x%04x",
                (unsigned)gl_error);
            return false;
        }
    }
    if (!SDL_GL_SwapWindow(context->window))
        return set_sdl_error(presenter, "SDL_GL_SwapWindow");
    return true;
}

static bool opengl_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!frame || !frame->pixels || frame->width <= 0 ||
        frame->height <= 0 || frame->pitch < frame->width * 4 ||
        (frame->pitch & 3) != 0) {
        snesrecomp_presenter_set_error(
            presenter, "invalid video frame");
        return false;
    }
    if (!make_current(presenter))
        return false;

    while (glGetError() != GL_NO_ERROR) {
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, context->texture);
    if (context->texture_format != frame->pixel_format ||
        context->texture_width != frame->width ||
        context->texture_height != frame->height) {
        if (!allocate_texture(
                presenter,
                frame->pixel_format,
                frame->width,
                frame->height,
                frame->pixels,
                frame->pitch)) {
            return false;
        }
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->pitch / 4);
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            frame->width,
            frame->height,
            GL_BGRA,
            GL_UNSIGNED_INT_8_8_8_8_REV,
            frame->pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    return opengl_present_texture(
        presenter, context->texture, frame->width, frame->height,
        frame->width, frame->height);
}

static uint8_t mode7_obj_texel(const uint16_t *vram,
                               const SnesRecompObjSliver *sliver,
                               unsigned x) {
    const unsigned word = (unsigned)sliver->tile * 16u + sliver->row;
    const uint16_t lo = vram[word & 0x7fffu];
    const uint16_t hi = vram[(word + 8u) & 0x7fffu];
    const unsigned bit = sliver->flip_h ? x : 7u - x;
    return (uint8_t)(((lo >> bit) & 1u) |
                     (((lo >> (bit + 8u)) & 1u) << 1u) |
                     (((hi >> bit) & 1u) << 2u) |
                     (((hi >> (bit + 8u)) & 1u) << 3u));
}

static bool build_mode7_obj_plane(
    SnesRecompPresenter *presenter, const SnesRecompMode7HdFrame *frame) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    const SnesPpuFrameCapture *cap = frame->capture;
    const size_t bytes =
        (size_t)cap->canvas_width * cap->visible_height * 2u;

    if (context->mode7_obj_capacity < bytes) {
        uint8_t *grown = (uint8_t *)realloc(context->mode7_obj_pixels, bytes);
        if (!grown) {
            snesrecomp_presenter_set_error(
                presenter, "out of memory for HD Mode 7 OBJ plane");
            return false;
        }
        context->mode7_obj_pixels = grown;
        context->mode7_obj_capacity = bytes;
    }
    memset(context->mode7_obj_pixels, 0, bytes);
    if (!frame->obj)
        return true;
    if (!frame->obj->slivers || frame->obj->count > frame->obj->capacity) {
        snesrecomp_presenter_set_error(
            presenter, "invalid HD Mode 7 OBJ frame");
        return false;
    }

    /* The evaluator emits back-to-front slivers. Plain overwrite here first
     * resolves OAM order into one native OBJ plane; priority is consulted only
     * afterwards by the Mode 7 compositor. */
    for (unsigned i = 0; i < frame->obj->count; i++) {
        const SnesRecompObjSliver *s = &frame->obj->slivers[i];
        if (s->line >= cap->visible_height || s->priority > 3u) {
            snesrecomp_presenter_set_error(
                presenter, "invalid HD Mode 7 OBJ sliver");
            return false;
        }
        for (unsigned px = 0; px < 8u; px++) {
            const int canvas_x =
                (int)cap->canvas_extra + (int)s->screen_x + (int)px;
            uint8_t index;
            uint8_t *dst;
            if (canvas_x < 0 || canvas_x >= (int)cap->canvas_width)
                continue;
            index = mode7_obj_texel(cap->vram, s, px);
            if (!index)
                continue;
            dst = context->mode7_obj_pixels +
                ((size_t)s->line * cap->canvas_width +
                 (unsigned)canvas_x) * 2u;
            dst[0] = (uint8_t)(s->palette_base + index);
            dst[1] = s->priority;
        }
    }
    return true;
}

static bool build_mode7_window_plane(
    SnesRecompPresenter *presenter, const SnesRecompMode7HdFrame *frame,
    SnesRecompSemanticLineState *lines) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    const SnesPpuFrameCapture *cap = frame->capture;
    const size_t pixels =
        (size_t)cap->canvas_width * cap->visible_height;
    const size_t bytes = pixels * 2u;
    if (!lines) {
        snesrecomp_presenter_set_error(
            presenter, "missing HD Mode 7 semantic line output");
        return false;
    }
    if (context->mode7_window_capacity < bytes) {
        uint8_t *grown =
            (uint8_t *)realloc(context->mode7_window_pixels, bytes);
        if (!grown) {
            snesrecomp_presenter_set_error(
                presenter, "out of memory for HD Mode 7 window plane");
            return false;
        }
        context->mode7_window_pixels = grown;
        context->mode7_window_capacity = bytes;
    }
    if (!snesrecomp_ppu_compile_semantic_input(
            cap, context->mode7_window_pixels,
            context->mode7_window_pixels + pixels, cap->canvas_width,
            lines, SNES_PPU_MAX_BANDS)) {
        snesrecomp_presenter_set_error(
            presenter, "invalid HD Mode 7 BG1 window state");
        return false;
    }
    /* Bits 1/2 are unavailable to BG2/BG3 in the accepted Mode 7 subset, so
     * they carry OBJ main/sub permissions in the same compact R8 texture.
     * BG1-sub remains in its native bit 3 for a future larger subset. */
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t obj_bits = context->mode7_window_pixels[pixels + i];
        if (obj_bits & SNESRECOMP_SEMANTIC_OBJ_MAIN)
            context->mode7_window_pixels[i] |= 2u;
        else
            context->mode7_window_pixels[i] &= (uint8_t)(~2u & 0xFFu);
        if (obj_bits & SNESRECOMP_SEMANTIC_OBJ_SUB)
            context->mode7_window_pixels[i] |= 4u;
        else
            context->mode7_window_pixels[i] &= (uint8_t)(~4u & 0xFFu);
    }
    return true;
}

static void upload_mode7_texture(GLuint texture, GLint internal_format,
                                 GLenum format, GLenum type,
                                 int width, int height, const void *pixels) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
                 format, type, pixels);
    mode7_texture_parameters();
}

static bool allocate_mode7_target(SnesRecompPresenter *presenter,
                                  int width, int height) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (context->mode7_target_width == width &&
        context->mode7_target_height == height)
        return true;

    glBindTexture(GL_TEXTURE_2D, context->mode7_target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    mode7_texture_parameters();
    {
        const GLint filter =
            context->linear_filtering ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, context->mode7_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, context->mode7_target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snesrecomp_presenter_set_error(
            presenter, "HD Mode 7 framebuffer is incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    context->mode7_target_width = width;
    context->mode7_target_height = height;
    return true;
}

static bool opengl_present_mode7_hd(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *frame) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    uint8_t map_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    uint8_t char_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    uint8_t palette[SNES_PPU_CGRAM_ENTRIES * 4u];
    float affine[SNES_PPU_MAX_BANDS * 4u];
    uint8_t flags[SNES_PPU_MAX_BANDS * 4u];
    SnesRecompSemanticLineState semantic_lines[SNES_PPU_MAX_BANDS];
    const SnesPpuFrameCapture *cap;
    const uint8_t *map_pixels;
    int map_width, map_height;
    int target_width, target_height;
    bool wants_obj = false;

    if (!frame || !(cap = frame->capture) || frame->scale != 2u ||
        !frame->lines || frame->line_count < cap->visible_height ||
        (frame->map_source &&
         !snesrecomp_ppu_mode7_map_source_valid(frame->map_source)) ||
        snesrecomp_ppu_mode7_supports(cap) != SNES_PPU_SUPPORTED ||
        cap->visible_height > SNES_PPU_MAX_BANDS ||
        cap->canvas_width > INT_MAX / 2 ||
        cap->visible_height > INT_MAX / 2) {
        snesrecomp_presenter_set_error(
            presenter, "unsupported HD Mode 7 frame");
        return false;
    }
    for (unsigned bi = 0; bi < cap->band_count; bi++)
        if (!cap->bands[bi].forced_blank &&
            ((cap->bands[bi].main_enable |
              cap->bands[bi].sub_enable) & 0x10u))
            wants_obj = true;
    if (wants_obj && !frame->obj) {
        snesrecomp_presenter_set_error(
            presenter, "HD Mode 7 frame is missing qualified OBJ state");
        return false;
    }
    if (!make_current(presenter) || !create_mode7_resources(presenter) ||
        !build_mode7_obj_plane(presenter, frame) ||
        !build_mode7_window_plane(presenter, frame, semantic_lines))
        return false;

    target_width = (int)cap->canvas_width * 2;
    target_height = (int)cap->visible_height * 2;
    if (!allocate_mode7_target(presenter, target_width, target_height) ||
        !snesrecomp_ppu_mode7_unpack_vram(cap->vram,
                                          frame->map_source ? NULL : map_tex,
                                          char_tex))
        return false;

    if (frame->map_source) {
        map_pixels = frame->map_source->tiles;
        map_width = (int)frame->map_source->width_tiles;
        map_height = (int)frame->map_source->height_tiles;
    } else {
        map_pixels = map_tex;
        map_width = 128;
        map_height = 128;
    }

    memset(flags, 0, sizeof flags);
    for (unsigned y = 0; y < cap->visible_height; y++) {
        affine[y * 4u + 0u] = (float)frame->lines[y].start_x;
        affine[y * 4u + 1u] = (float)frame->lines[y].start_y;
        affine[y * 4u + 2u] = (float)frame->lines[y].step_x;
        affine[y * 4u + 3u] = (float)frame->lines[y].step_y;
    }
    for (unsigned bi = 0; bi < cap->band_count; bi++) {
        const SnesPpuRasterBand *band = &cap->bands[bi];
        for (unsigned y = band->y_begin; y < band->y_end; y++) {
            flags[y * 4u + 0u] =
                band->forced_blank ? 0u : band->brightness;
            flags[y * 4u + 1u] = band->bg[0].margin_left;
            flags[y * 4u + 2u] = band->main_enable;
            flags[y * 4u + 3u] = band->bg[0].margin_right;
        }
    }
    for (unsigned i = 0; i < SNES_PPU_CGRAM_ENTRIES; i++) {
        const uint16_t c = cap->cgram[i];
        palette[i * 4u + 0u] = (uint8_t)(c & 31u);
        palette[i * 4u + 1u] = (uint8_t)((c >> 5u) & 31u);
        palette[i * 4u + 2u] = (uint8_t)((c >> 10u) & 31u);
        palette[i * 4u + 3u] = 255u;
    }

    glActiveTexture(GL_TEXTURE0);
    upload_mode7_texture(context->mode7_map_texture, GL_R8, GL_RED,
                         GL_UNSIGNED_BYTE, map_width, map_height, map_pixels);
    glActiveTexture(GL_TEXTURE1);
    upload_mode7_texture(context->mode7_char_texture, GL_R8, GL_RED,
                         GL_UNSIGNED_BYTE, 128, 128, char_tex);
    glActiveTexture(GL_TEXTURE2);
    upload_mode7_texture(context->mode7_palette_texture, GL_RGBA8, GL_RGBA,
                         GL_UNSIGNED_BYTE, 256, 1, palette);
    glActiveTexture(GL_TEXTURE3);
    upload_mode7_texture(context->mode7_line_texture, GL_RGBA32F, GL_RGBA,
                         GL_FLOAT, 1, (int)cap->visible_height, affine);
    glActiveTexture(GL_TEXTURE4);
    upload_mode7_texture(context->mode7_flags_texture, GL_RGBA8, GL_RGBA,
                         GL_UNSIGNED_BYTE, 1, (int)cap->visible_height, flags);
    glActiveTexture(GL_TEXTURE5);
    upload_mode7_texture(
        context->mode7_obj_texture, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
        (int)cap->canvas_width, (int)cap->visible_height,
        context->mode7_obj_pixels);
    glActiveTexture(GL_TEXTURE6);
    upload_mode7_texture(
        context->mode7_window_texture, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
        (int)cap->canvas_width, (int)cap->visible_height,
        context->mode7_window_pixels);
    glActiveTexture(GL_TEXTURE7);
    upload_mode7_texture(
        context->mode7_math_texture, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
        2, (int)cap->visible_height, semantic_lines);

    glBindFramebuffer(GL_FRAMEBUFFER, context->mode7_fbo);
    glViewport(0, 0, target_width, target_height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(context->mode7_program);
    glUniform1f(glGetUniformLocation(context->mode7_program, "hd_scale"),
                2.0f);
    glUniform1f(glGetUniformLocation(context->mode7_program, "canvas_extra"),
                (float)cap->canvas_extra);
    glUniform1f(glGetUniformLocation(context->mode7_program, "native_height"),
                (float)cap->visible_height);
    glUniform2f(glGetUniformLocation(context->mode7_program, "map_fixed_wrap"),
                (float)map_width * 8.0f * 256.0f,
                (float)map_height * 8.0f * 256.0f);
    glBindVertexArray(context->vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    return opengl_present_texture(
        presenter, context->mode7_target, target_width, target_height,
        (int)cap->canvas_width, (int)cap->visible_height);
}

static bool opengl_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!SDL_SetWindowFullscreen(context->window, fullscreen))
        return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
    return true;
}

static bool opengl_set_window_scale(
    SnesRecompPresenter *presenter,
    int scale) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (scale <= 0) {
        snesrecomp_presenter_set_error(
            presenter, "window scale must be positive");
        return false;
    }
    if (!SDL_SetWindowSize(
            context->window,
            snesrecomp_presenter_display_width(
                presenter, presenter->frame_width) * scale,
            presenter->frame_height * scale)) {
        return set_sdl_error(presenter, "SDL_SetWindowSize");
    }
    return true;
}

static bool opengl_set_window_title(
    SnesRecompPresenter *presenter,
    const char *title) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!title || !title[0]) {
        snesrecomp_presenter_set_error(
            presenter, "window title is empty");
        return false;
    }
    if (!SDL_SetWindowTitle(context->window, title))
        return set_sdl_error(presenter, "SDL_SetWindowTitle");
    return true;
}

static bool opengl_get_drawable_size(
    SnesRecompPresenter *presenter,
    int *width,
    int *height) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)presenter->context;
    if (!width || !height) {
        snesrecomp_presenter_set_error(
            presenter, "drawable-size output is null");
        return false;
    }
    if (!SDL_GetWindowSizeInPixels(context->window, width, height))
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    return true;
}

static const SnesRecompPresenterOps kOpenGlPresenterOps = {
    opengl_destroy,
    opengl_present,
    opengl_set_fullscreen,
    opengl_set_window_scale,
    opengl_set_window_title,
    opengl_get_drawable_size,
    opengl_present_mode7_hd,
};

static bool set_gl_attribute(
    SnesRecompPresenter *presenter,
    SDL_GLAttr attribute,
    int value,
    const char *name) {
    if (SDL_GL_SetAttribute(attribute, value))
        return true;
    snesrecomp_presenter_set_error(
        presenter,
        "SDL_GL_SetAttribute(%s) failed: %s",
        name,
        SDL_GetError());
    return false;
}

bool snesrecomp_presenter_opengl_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config) {
    OpenGlPresenterContext *context =
        (OpenGlPresenterContext *)calloc(1, sizeof(*context));
    if (!context) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory creating OpenGL presenter");
        return false;
    }

    presenter->ops = &kOpenGlPresenterOps;
    presenter->context = context;
    context->preserve_aspect = config->preserve_aspect;
    context->linear_filtering = config->linear_filtering;
    context->preset_interface = config->shader_preset_interface;

    if (!set_gl_attribute(
            presenter,
            SDL_GL_CONTEXT_PROFILE_MASK,
            SDL_GL_CONTEXT_PROFILE_CORE,
            "profile") ||
        !set_gl_attribute(
            presenter, SDL_GL_CONTEXT_MAJOR_VERSION, 3, "major") ||
        !set_gl_attribute(
            presenter, SDL_GL_CONTEXT_MINOR_VERSION, 3, "minor") ||
        !set_gl_attribute(
            presenter, SDL_GL_DOUBLEBUFFER, 1, "double-buffer")) {
        return false;
    }

    context->window = SDL_CreateWindow(
        config->window_title,
        snesrecomp_presenter_display_width(
            presenter, config->frame_width) * config->window_scale,
        config->frame_height * config->window_scale,
        SDL_WINDOW_OPENGL |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!context->window)
        return set_sdl_error(presenter, "SDL_CreateWindow(OpenGL)");

    context->gl_context = SDL_GL_CreateContext(context->window);
    if (!context->gl_context)
        return set_sdl_error(presenter, "SDL_GL_CreateContext");
    if (!make_current(presenter))
        return false;

    if (ogl_LoadFunctions() == ogl_LOAD_FAILED) {
        snesrecomp_presenter_set_error(
            presenter, "could not load required OpenGL functions");
        return false;
    }
    if (!ogl_IsVersionGEQ(3, 3)) {
        const char *version = (const char *)glGetString(GL_VERSION);
        snesrecomp_presenter_set_error(
            presenter,
            "OpenGL 3.3 is required (driver reports %s)",
            version ? version : "unknown");
        return false;
    }

    if (!SDL_GL_SetSwapInterval(config->vsync ? 1 : 0)) {
        fprintf(
            stderr,
            "[snesrecomp-platform] could not set OpenGL swap interval: %s\n",
            SDL_GetError());
    }
    int swap_interval = 0;
    presenter->vsync_state =
        SDL_GL_GetSwapInterval(&swap_interval)
            ? (swap_interval != 0
                ? SNESRECOMP_VSYNC_ENABLED
                : SNESRECOMP_VSYNC_DISABLED)
            : SNESRECOMP_VSYNC_UNKNOWN;

    glGenTextures(1, &context->texture);
    if (!context->texture) {
        snesrecomp_presenter_set_error(
            presenter, "OpenGL texture allocation failed");
        return false;
    }
    if (!allocate_texture(
            presenter,
            config->pixel_format,
            config->frame_width,
            config->frame_height,
            NULL,
            0) ||
        !create_geometry(presenter) ||
        !create_program(presenter)) {
        return false;
    }

    if (config->shader_preset_path && config->shader_preset_path[0]) {
        const SnesRecompShaderPresetInterface *interface =
            config->shader_preset_interface;
        if (!interface || !interface->create || !interface->destroy ||
            !interface->render) {
            snesrecomp_presenter_set_error(
                presenter,
                "a shader preset was requested without a complete adapter");
            return false;
        }
        context->preset = interface->create(
            config->shader_preset_path,
            presenter->last_error,
            sizeof(presenter->last_error));
        if (!context->preset) {
            if (!presenter->last_error[0]) {
                snesrecomp_presenter_set_error(
                    presenter,
                    "could not load shader preset: %s",
                    config->shader_preset_path);
            }
            return false;
        }
    }

    if (config->fullscreen &&
        !SDL_SetWindowFullscreen(context->window, true)) {
        return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
    }

    presenter->backend = SNESRECOMP_PRESENT_BACKEND_OPENGL;
    presenter->capabilities = SNESRECOMP_PRESENT_CAP_BASIC;
    presenter->capabilities |=
        SNESRECOMP_PRESENT_CAP_3D |
        SNESRECOMP_PRESENT_CAP_HD_MODE7;
    if (config->shader_preset_interface) {
        presenter->capabilities |=
            SNESRECOMP_PRESENT_CAP_SHADER |
            SNESRECOMP_PRESENT_CAP_MULTIPASS;
    }

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    snprintf(
        presenter->backend_name,
        sizeof(presenter->backend_name),
        "OpenGL 3.3/%s",
        renderer && renderer[0] ? renderer : "unknown");
    presenter->last_error[0] = '\0';
    return true;
}
