// Common files shared across c++ CPU code and GLSL GPU Code
#ifndef VK_MINI_PATH_TRACER_COMMON_H
#define VK_MINI_PATH_TRACER_COMMON_H

#ifdef __cplusplus
#include <cstdint>
using uint = uint32_t;
#endif  // #ifdef __cplusplus

struct PushConstants
{
	uint sample_batch;
};

#define WORKGROUP_WIDTH 16
#define WORKGROUP_HEIGHT 8

#define BINDING_IMAGEDATA 0
#define BINDING_TLAS 1
#define BINDING_VERTICES 2
#define BINDING_INDICES 3

#endif // #ifndef VK_MINI_PATH_TRACER_COMMON_H