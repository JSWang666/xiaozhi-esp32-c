/**
 * Battery / charging helpers for this board were historically implemented as
 * C++ `PowerManager` in `power_manager_legacy.hpp`. The C board file
 * `esp32-s3-lcd-0.85.c` does not include this header — keep a C-safe stub so
 * tooling that globs headers never feeds C++ into a C-only compile.
 */
#pragma once

#ifdef __cplusplus
#include "power_manager_legacy.hpp"
#endif
