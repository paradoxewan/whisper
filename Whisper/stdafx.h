#pragma once
#define _USE_MATH_DEFINES
#include <stdint.h>
#include <assert.h>
#include <array>
#include <vector>
#include <algorithm>
#include <emmintrin.h>	// SSE 2
#include <smmintrin.h>	// SSE 4.1

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Setup Windows SDK to only enable features available since Windows 8.0
#include <WinSDKVer.h>
#define _WIN32_WINNT _WIN32_WINNT_WIN8
#define NTDDI_VERSION NTDDI_WIN8
#include <sdkddkver.h>

#include <windows.h>
#include <d3d11.h>
#include <atlcomcli.h>
#include "Utils/Logger.h"
#include "Utils/miscUtils.h"

// Build both legacy and DirectCompute implementations
#define BUILD_BOTH_VERSIONS 0

// Build hybrid model which uses DirectCompute only for the encode step of the algorithm, and decodes on CPU, using AVX SIMD and the Windows' built-in thread pool.
// Disabled because on all computers I have in this house that hybrid model performed worse than D3D11 GPGPU model
#define BUILD_HYBRID_VERSION 0

// Enable debug traces. Should be disabled in production, the feature comes with a huge performance overhead.
// When enabled, while computing things it streams gigabytes of data into that binary file.
// See Tools / compareTraces project for a command-line app to compare these traces.
#define SAVE_DEBUG_TRACE 0

// In addition to collecting total GPU times per compute shader, also collect and print performance data about individual invocations of some of the most expensive shaders
// The feature is relatively cheap in terms of performance overhead, but pretty much useless in production, and clutters debug console with all these numbers
#define PROFILER_COLLECT_TAGS 0

// Reshape some of the tensors to a better VRAM layout while loading a model
// So far, the feature is only used on AMD GPUs. On AMD Vega integrated GPUs it helps by up to 30%.
// Should be enabled in production build
#define RESHAPED_MATRIX_MULTIPLY 1