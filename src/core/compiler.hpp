#pragma once

// NDK Clang extensions live here, independently of the optimization level.
// Use only for measured call boundaries; never apply to the whole project.
#if defined(__clang__)
#define KAGAMI_NOINLINE [[clang::noinline]]
#else
#define KAGAMI_NOINLINE
#endif
