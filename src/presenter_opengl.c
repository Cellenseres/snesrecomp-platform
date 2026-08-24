#include "presenter_internal.h"

#include "gl_core_3_1.h"
#include <SDL3/SDL.h>

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

    int drawable_width = 0;
    int drawable_height = 0;
    if (!SDL_GetWindowSizeInPixels(
            context->window, &drawable_width, &drawable_height)) {
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    }
    if (drawable_width <= 0 || drawable_height <= 0)
        return true;

    int viewport_width = drawable_width;
    int viewport_height = drawable_height;
    if (context->preserve_aspect) {
        if (viewport_width * frame->height <
            viewport_height * frame->width) {
            viewport_height =
                viewport_width * frame->height / frame->width;
        } else {
            viewport_width =
                viewport_height * frame->width / frame->height;
        }
    }
    const int viewport_x = (drawable_width - viewport_width) / 2;
    const int viewport_y = (drawable_height - viewport_height) / 2;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (context->preset) {
        context->preset_interface->render(
            context->preset,
            context->texture,
            frame->width,
            frame->height,
            viewport_x,
            viewport_y,
            viewport_width,
            viewport_height);
    } else {
        glViewport(
            viewport_x,
            viewport_y,
            viewport_width,
            viewport_height);
        glUseProgram(context->program);
        glBindVertexArray(context->vertex_array);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        snesrecomp_presenter_set_error(
            presenter,
            "OpenGL present failed with error 0x%04x",
            (unsigned)gl_error);
        return false;
    }

    if (!SDL_GL_SwapWindow(context->window))
        return set_sdl_error(presenter, "SDL_GL_SwapWindow");
    return true;
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
            presenter->frame_width * scale,
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
        config->frame_width * config->window_scale,
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
