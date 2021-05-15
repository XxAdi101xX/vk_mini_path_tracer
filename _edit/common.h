// Common files shared across c++ CPU code and GLSL GPU Code
#pragma once

#ifdef __cplusplus
#include <cstdint>
using uint = uint32_t;
#endif  // #ifdef __cplusplus

struct PushConstants
{
	uint render_width;
	uint render_height;
};

#define WORKGROUP_WIDTH 16
#define WORKGROUP_HEIGHT 8

#define BINDING_IMAGEDATA 0
#define BINDING_TLAS 1
#define BINDING_VERTICES 2
#define BINDING_INDICES 3