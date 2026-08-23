#ifndef HB_DEMO_COMMON_H
#define HB_DEMO_COMMON_H

/* Minimal placeholder for harfbuzz's demo-support header.
 *
 * This header normally lives in the upstream harfbuzz source tree and is only
 * consumed by harfbuzz's standalone demo tools (hb-view / hb-shape / ...), which
 * are NOT compiled into the Aurora library (Aurora links the `harfbuzz.cc`
 * amalgam only). It is listed in harfbuzz's `project_headers` purely for
 * install/IDE purposes, so CMake's source-existence check requires the file to
 * be present. The full upstream header is absent from this checkout, hence this
 * stub unblocks configuration without affecting the library build. */

#endif /* HB_DEMO_COMMON_H */
