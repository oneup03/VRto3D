/*
 * This file is part of WibbleWobbleLinux.
 *
 * WibbleWobbleLinux is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WibbleWobbleLinux is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with WibbleWobbleLinux. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef WWCLIENT_H
#define WWCLIENT_H

/* libwwclient — C ABI for feeding stereo frames to wwserver. Shape mirrors
 * the Windows WibbleWobbleClientCApi (Create/IsRunning/Destroy/
 * SetSourceFormat/PresentFrame) with dmabuf buffer registration replacing
 * D3D shared handles. Thread-safe per-handle; PresentFrame is non-blocking
 * (drops when the server is behind — the server's frame-selection policy
 * decides what displays).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WWCLIENT_EXPORT __attribute__((visibility("default")))

typedef struct WWClientOpaque* WWClientHandle;

/* Source formats — mirror ww::proto::SourceFormat / Windows WWSourceFormat. */
#define WWSF_SINGLE 0
#define WWSF_SIDE_BY_SIDE_HALF 1
#define WWSF_CHECKERBOARD 2
#define WWSF_SIDE_BY_SIDE_FULL 3

/* Connects to wwserver ($XDG_RUNTIME_DIR/wibblewobble-0.sock). Returns NULL
 * only on allocation failure — connection is established lazily/retried, so
 * a client may Create before the server is up. `source_name` is diagnostic
 * (may be NULL). */
WWCLIENT_EXPORT WWClientHandle WWClient_Create(const char* source_name);

/* 1 when connected to a live server, else 0 (reconnects internally). */
WWCLIENT_EXPORT int WWClient_IsRunning(WWClientHandle h);

WWCLIENT_EXPORT void WWClient_Destroy(WWClientHandle h);

/* Declare the source geometry/format. Must be called before RegisterBuffer;
 * calling again re-handshakes (drops registered buffers). drm_fourcc is a
 * DRM_FORMAT_* code; modifier a DRM_FORMAT_MOD_* value. */
WWCLIENT_EXPORT void WWClient_SetSourceFormat(WWClientHandle h, int wwsf,
                                              uint32_t width, uint32_t height,
                                              uint32_t drm_fourcc,
                                              uint64_t drm_modifier);

/* Register a dmabuf as a reusable frame buffer (single-plane convenience).
 * The fd is dup()ed; caller keeps ownership of its copy. Returns buffer
 * index >= 0, or -1 on failure/limit. */
WWCLIENT_EXPORT int WWClient_RegisterBuffer(WWClientHandle h, int dmabuf_fd,
                                            uint32_t stride, uint32_t offset,
                                            uint64_t size);

/* Multi-plane variant. */
WWCLIENT_EXPORT int WWClient_RegisterBufferPlanes(WWClientHandle h, int num_planes,
                                                  const int* dmabuf_fds,
                                                  const uint32_t* strides,
                                                  const uint32_t* offsets,
                                                  const uint64_t* sizes);

/* Present buffer `index` as frame `frame_id` (monotonic — drives the
 * server's pair-atomicity guard). Returns 1 on send, 0 when not connected.
 * Do not reuse the buffer until the server releases it (poll
 * WWClient_BufferBusy or just cycle >= 3 buffers). */
WWCLIENT_EXPORT int WWClient_PresentFrame(WWClientHandle h, int buffer_index,
                                          uint64_t frame_id);

/* 1 while the server still holds the buffer, 0 when free/unknown. */
WWCLIENT_EXPORT int WWClient_BufferBusy(WWClientHandle h, int buffer_index);

#ifdef __cplusplus
}
#endif

#endif /* WWCLIENT_H */
