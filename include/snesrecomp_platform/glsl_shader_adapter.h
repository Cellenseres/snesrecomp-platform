#ifndef SNESRECOMP_PLATFORM_GLSL_SHADER_ADAPTER_H
#define SNESRECOMP_PLATFORM_GLSL_SHADER_ADAPTER_H

#include "snesrecomp_platform/presenter.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Adapter for snesrecomp's shared desktop GLSL preset renderer. Consumers
 * enable its implementation with snesrecomp_platform_target_glsl_presets().
 */
const SnesRecompShaderPresetInterface *
snesrecomp_glsl_shader_preset_interface(void);

#ifdef __cplusplus
}
#endif

#endif
