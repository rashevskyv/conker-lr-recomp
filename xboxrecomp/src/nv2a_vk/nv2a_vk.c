/*
 * nv2a_vk.c -- a Vulkan 1.3 renderer for the pushbuffer executor.
 *
 * The same job as nv2a_gl.c, on Vulkan: registered as the executor's back end
 * (nv2a_backend.h, draw_raw), it turns the method shadow into draws. Built
 * instead of nv2a_gl for the Switch's NVK (mesa-switch, linked statically)
 * and, on Linux, any Vulkan driver (lavapipe runs it headless).
 *
 *   surfaces   one image per colour-surface address (B8G8R8A8, rows top-first
 *              like guest memory), depth per zeta address (as nv2a_gl). All
 *              images live in VK_IMAGE_LAYOUT_GENERAL: a surface is drawn
 *              into, sampled and blitted without layout changes, ordered by
 *              memory barriers between rendering passes.
 *   textures   the GL renderer's cache and decoders; uploaded through the
 *              frame's staging ring. DXT stays compressed (BC1/2/3).
 *   shaders    gl_vsh.c / gl_psh.c in their Vulkan dialect, compiled to
 *              SPIR-V at run time by glslang, cached by what generated them.
 *   pipelines  keyed by the shader pair, blend state, colour mask, whether
 *              there is a depth buffer and the topology class; everything
 *              else (depth, stencil, cull, winding, topology, vertex layout,
 *              viewport) is dynamic state.
 *   uniforms   two std140 blocks per draw in the frame ring, textures as
 *              combined image samplers, all through push descriptors.
 *
 * Two frames in flight. Everything runs on the executor's thread.
 *
 * Runtime switches (the GL renderer's names where they mean the same):
 *   RECOMP_GL_DUMP=<prefix>[,every]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources on compile errors
 *   RECOMP_GL_SCALE=<k>              render surfaces at k times the title's
 *                                    resolution, fractions allowed (1.5;
 *                                    0.5..4, default 1)
 *   RECOMP_GL_SCALE_X=<k>            horizontal scale, if not the same
 *   RECOMP_VK_SQUARE=0               widescreen without 4/3 more columns on
 *                                    surfaces without horizontal AA
 *   RECOMP_VK_MIPS=0                 sample level 0 only (no mip chains)
 *   RECOMP_VK_ANISO=<n>              anisotropic filtering of mipmapped
 *                                    textures (default 16; 1 = off)
 *   RECOMP_VK_LOD_BIAS=<b>           mip bias of mipmapped textures
 *                                    (default -0.25: a little sharper)
 *   RECOMP_VK_FXAA=0                 present with a plain blit, no FXAA
 *   RECOMP_VK_VALIDATION=1           (Linux) enable the Khronos validation layer
 *   RECOMP_VK_HEADLESS=1             (Linux) no window, no present
 */
#include "nv2a_vk.h"
#include "../nv2a_gl/gl_psh.h"
#include "../nv2a_gl/gl_vsh.h"
#include "../kernel/nv2a_backend.h"
#include "../platform/frame_stats.h"
#include "../platform/overlay.h"

#if defined(__SWITCH__)
#include <switch.h>
#define VK_USE_PLATFORM_VI_NN 1
#endif
#if defined(__ANDROID__)
#include <volk.h>                   /* vk* through pointers: see vk_load */
#include <adrenotools/driver.h>
#include <dlfcn.h>
#else
#include <vulkan/vulkan.h>
#endif
#if !defined(__SWITCH__)
#include <SDL.h>
#include <SDL_vulkan.h>
#endif
#if defined(__ANDROID__)
#include <SDL_syswm.h>
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define LOGE(...) fprintf(stderr, "  [VK] " __VA_ARGS__)

static int s_state;            /* 0 untried, 1 ready, -1 failed */
static int s_trace;
/* RECOMP_VK_TRACE=<n>: name every step of the first n draws, clears and
 * flips (a crash inside the driver then ends the log at the step). */
static int s_vtrace;
#define VT(...) do { if (s_vtrace > 0) { fprintf(stderr, "  [VKT] " __VA_ARGS__); fflush(stderr); } } while (0)
static uint32_t s_frame;
/* RECOMP_TEX_CHANGES=1: log every cached texture whose guest bytes changed
 * (re-uploaded). Finds textures a title frees while still drawing them. */
static int s_tex_changes = -1;
static int s_drew_any;
static int fxaa_on(void);
static const uint32_t *s_regs;
static const Nv2aRawBatch *s_batch;

/* Hitch accounting, as in nv2a_gl. */
static uint32_t s_hz_progs, s_hz_texs, s_hz_draws, s_hz_pipes;
static uint64_t s_hz_prog_ns, s_hz_tex_ns, s_hz_tex_bytes, s_hz_pipe_ns;
static uint64_t hz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Device ───────────────────────────────────────────────────────── */

static VkInstance       s_inst;
static VkPhysicalDevice s_pd;
static VkDevice         s_dev;
static VkQueue          s_queue;
static uint32_t         s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkDeviceSize     s_ubo_align = 256;
static uint32_t         s_ubo_range = 16384;
static VkFormat         s_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
static int              s_have_bc, s_have_depth_clamp;

static PFN_vkCmdPushDescriptorSetKHR  p_push_desc;
static PFN_vkCmdSetVertexInputEXT     p_vertex_input;
static PFN_vkCmdBeginRendering        p_begin_rendering;
static PFN_vkCmdEndRendering          p_end_rendering;
static PFN_vkCmdSetCullMode           p_cull_mode;
static PFN_vkCmdSetFrontFace          p_front_face;
static PFN_vkCmdSetPrimitiveTopology  p_topology;
static PFN_vkCmdSetDepthTestEnable    p_depth_test;
static PFN_vkCmdSetDepthWriteEnable   p_depth_write;
static PFN_vkCmdSetDepthCompareOp     p_depth_op;
static PFN_vkCmdSetStencilTestEnable  p_stencil_test;
static PFN_vkCmdSetStencilOp          p_stencil_op;

/* Presentation. */
static VkSurfaceKHR   s_surface;
static VkSwapchainKHR s_swapchain;
static VkFormat       s_sc_format;
static VkExtent2D     s_sc_extent;
/* The swapchain is not in the panel's rotation, by choice (create_swapchain):
 * every present then reports SUBOPTIMAL, which is not a reason to rebuild. */
static int            s_sc_unrotated;
static uint32_t       s_sc_count;
static VkImage        s_sc_images[8];
static VkImageView    s_sc_views[8];
/* Signalled by the frame's submit, waited on by the present of the same
 * image. Per image, not per frame: a frame's fence does not cover the
 * presentation wait, so a per-frame semaphore could be re-signalled while
 * the previous present still waits on it. */
static VkSemaphore    s_sc_rendered[8];
static int            s_headless;
#if !defined(__SWITCH__)
static SDL_Window    *s_win;
#endif

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements rq, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m = VK_NULL_HANDLE;
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return VK_NULL_HANDLE;
    if (vkAllocateMemory(s_dev, &ai, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

/* ── Frames in flight ─────────────────────────────────────────────── */

#define VK_FRAMES 2
#define RING_BYTES (48u << 20)

typedef struct {
    VkImage image; VkImageView view; VkDeviceMemory mem; VkPipeline pipe;
} Garbage;

/* Command buffers a frame can be split into (RECOMP_VK_KICK). */
#define VK_CBS 8

typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cbs[VK_CBS];
    uint32_t        ncb;           /* the one being recorded */
    VkFence         fence;
    VkSemaphore     acquired;
    VkBuffer        ring;
    VkDeviceMemory  ring_mem;
    uint8_t        *ring_ptr;
    VkDeviceSize    ring_off;
    int             submitted;
    Garbage        *garbage;
    uint32_t        ngarbage, garbage_cap;
} Frame;

static Frame  s_fr[VK_FRAMES];
static Frame *s_f;             /* the frame being recorded */
static uint32_t s_fi;
static VkCommandBuffer s_cb;   /* s_f->cbs[s_f->ncb] while recording */
static uint32_t s_cb_gen;      /* bumped at every vkBeginCommandBuffer */
static int s_dyn_valid;
static uint32_t s_kick_draws, s_st_kicks;   /* RECOMP_VK_KICK */
static uint32_t s_ring_gen;    /* bumped whenever a ring starts over */
static int kick_due(int mid_pass);
static void kick_submit(void);
static void pool_release(VkImage img);
static int s_pipe_foreign;      /* a pipeline not from pipe_get is bound */
static int s_transfer_pending; /* see pass_barrier */

static void garbage_add_pipe(VkPipeline pipe);
static void garbage_add(VkImage img, VkImageView view, VkDeviceMemory mem)
{
    Frame *f = s_f;
    if (f->ngarbage == f->garbage_cap) {
        f->garbage_cap = f->garbage_cap ? f->garbage_cap * 2 : 64;
        f->garbage = (Garbage *)realloc(f->garbage, f->garbage_cap * sizeof *f->garbage);
    }
    f->garbage[f->ngarbage].image = img;
    f->garbage[f->ngarbage].view = view;
    f->garbage[f->ngarbage].mem = mem;
    f->garbage[f->ngarbage].pipe = VK_NULL_HANDLE;
    f->ngarbage++;
}

static void garbage_add_pipe(VkPipeline pipe)
{
    garbage_add(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    s_f->garbage[s_f->ngarbage - 1].pipe = pipe;
}

static void garbage_free(Frame *f)
{
    uint32_t i;
    for (i = 0; i < f->ngarbage; i++) {
        if (f->garbage[i].view) vkDestroyImageView(s_dev, f->garbage[i].view, NULL);
        if (f->garbage[i].image) vkDestroyImage(s_dev, f->garbage[i].image, NULL);
        if (f->garbage[i].mem) vkFreeMemory(s_dev, f->garbage[i].mem, NULL);
        else if (f->garbage[i].image) pool_release(f->garbage[i].image);
        if (f->garbage[i].pipe) vkDestroyPipeline(s_dev, f->garbage[i].pipe, NULL);
    }
    f->ngarbage = 0;
}

/* RECOMP_LAT_TRACE: the frame slot whose GPU completion is awaited, -1 none.
 * Polled at flips and draws (a fence query), so "GPU done" is late by at
 * most one draw. */
static int s_lat_slot = -1;

static void lat_poll(void)
{
    if (s_lat_slot >= 0 && vkGetFenceStatus(s_dev, s_fr[s_lat_slot].fence) == VK_SUCCESS) {
        recomp_lat_gpu_done();
        s_lat_slot = -1;
    }
}

/* RECOMP_FRAME_STATS: GPU time per frame, from timestamps at the start of
 * the frame's first command buffer and the end of its last (a span: idle
 * gaps between early submits count too). Two queries per frame slot. */
static VkQueryPool s_tsq;
static double s_ts_ns;               /* ns per timestamp tick */
static uint8_t s_ts_began[VK_FRAMES], s_ts_done[VK_FRAMES];

static int ts_ready(void)
{
    static int tried;
    if (!tried && fs_want_gpu()) {
        VkPhysicalDeviceProperties pp;
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        tried = 1;
        vkGetPhysicalDeviceProperties(s_pd, &pp);
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * VK_FRAMES;
        s_ts_ns = pp.limits.timestampPeriod;
        if (!pp.limits.timestampComputeAndGraphics
                || vkCreateQueryPool(s_dev, &qi, NULL, &s_tsq) != VK_SUCCESS)
            s_tsq = VK_NULL_HANDLE;
        fprintf(stderr, "  [VK] GPU timestamps %s (%.1f ns per tick)\n",
                s_tsq ? "on" : "unavailable", s_ts_ns);
    }
    return s_tsq != VK_NULL_HANDLE;
}

static void frame_begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    s_f = &s_fr[s_fi];
    if (s_f->submitted) {
        uint64_t t0 = hz_now();
        vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
        fs_add(FS_SLOT_WAIT, hz_now() - t0);
        if (s_ts_done[s_fi]) {
            uint64_t ts[2];
            if (vkGetQueryPoolResults(s_dev, s_tsq, 2 * s_fi, 2, sizeof ts, ts, sizeof ts[0],
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
                fs_add(FS_GPU, (uint64_t)((double)(ts[1] - ts[0]) * s_ts_ns));
            s_ts_done[s_fi] = 0;
        }
        if (s_lat_slot == (int)s_fi) {
            recomp_lat_gpu_done();
            s_lat_slot = -1;
        }
        vkResetFences(s_dev, 1, &s_f->fence);
        s_f->submitted = 0;
    }
    garbage_free(s_f);
    s_f->ring_off = 0;
    s_ring_gen++;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s_f->ncb = 0;
    s_cb = s_f->cbs[0];
    vkBeginCommandBuffer(s_cb, &bi);
    s_ts_began[s_fi] = ts_ready();
    if (s_ts_began[s_fi]) {
        vkCmdResetQueryPool(s_cb, s_tsq, 2 * s_fi, 2);
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tsq, 2 * s_fi);
    }
    s_cb_gen++;
    s_dyn_valid = 0;
    s_kick_draws = 0;
    s_transfer_pending = 1;
}

/* Space in this frame's ring: returns the offset, *ptr the mapping. */
static void flush_frame_wait(void);
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **ptr)
{
    VkDeviceSize at = (s_f->ring_off + align - 1) & ~(align - 1);
    if (at + size > RING_BYTES) {
        /* A frame larger than the ring: finish what is recorded, wait for
         * it, and start over in the same ring. Slow, but only in extremes. */
        static int warned;
        if (!warned++)
            LOGE("frame ring (%u MB) full: flushing mid-frame\n", RING_BYTES >> 20);
        flush_frame_wait();
        at = 0;
        if (size > RING_BYTES) {
            *ptr = NULL;
            return 0;
        }
    }
    s_f->ring_off = at + size;
    *ptr = s_f->ring_ptr + at;
    return at;
}

/* ── Rendering passes and barriers ────────────────────────────────── */

/* w, h are the surface in the title's pixels (anti-aliasing included) --
 * what the shaders, clip rectangles and texture lookups work in. pw, ph are
 * the image: w, h times the render scale. */
typedef struct {
    uint32_t va, w, h, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
    uint32_t aa_sx, aa_sy;
    int dirty;                 /* written since the last barrier */
    int sampled;               /* read by a draw since the last barrier */
    uint32_t gen;              /* bumped by every draw and clear into it */
} VkSurf;

typedef struct {
    uint32_t va, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
    int dirty;                 /* used as an attachment since the last barrier */
    int fresh;                 /* not cleared yet: the first pass clears it */
    int sampleable;            /* made with SAMPLED usage (zs_wanted) */
    VkImageView sview;         /* depth-only view for sampling, made on first use */
    uint32_t wgen;             /* bumped by every pass that attaches it */
} VkDepthBuf;

/* 128, not 32: Conker's per-object 32x32 specular maps (nv2a_gl.c's
 * GL_MAX_SURF) evicted each other, and shiny props sampled garbage. */
#define VK_MAX_SURF 128
#define VK_MAX_DEPTH 16

/* RECOMP_GL_SCALE, as in nv2a_gl: only the pixels behind a surface grow, so
 * the viewport, clear rectangles, read-backs and the present blit scale and
 * nothing else does. Capped per surface by the device's image limits.
 * Horizontal: s_scale times s_xratio, 1 unless RECOMP_GL_SCALE_X sets the
 * horizontal scale outright (a game option's rescale keeps the ratio).
 * s_square: widescreen shows the title's 640x480 at 16:9 (xbox_wide_ratio;
 * s_wide is the one the surfaces were sized for), so surfaces
 * without horizontal AA get 4/3 more columns (21:9: 7/4) -- square pixels (races: 1.5x
 * -> 1280x720); surfaces with horizontal AA get 4/3 divided by it (the
 * front end's 1280x480, aa 2x1: 1280x720 at 1.5x, not 1920x720 -- the
 * title's horizontal supersampling is dropped).
 * RECOMP_VK_SQUARE=0 off; RECOMP_GL_SCALE_X overrides it. */
static double   s_scale = 1.0, s_scale_x = 1.0, s_xratio = 1.0;
static int      s_square, s_square_env;
static float    s_wide = 16.0f / 9.0f;

/* Picture options set at run time (NFSU2: Options -> Video), read by the
 * renderer: FXAA on/off (at every present), anisotropic level 1..16 (per
 * sampler key, capped by RECOMP_VK_ANISO and the device) and square pixels
 * on/off (at the next flip, like nv2a_vk_scale_pct). The env switches
 * (RECOMP_VK_FXAA=0, RECOMP_VK_SQUARE=0, ...) still force them off. */
volatile int nv2a_vk_fxaa = 1;
volatile int nv2a_vk_aniso = 16;
volatile int nv2a_vk_square = 1;
static uint32_t s_max_size = 4096;

/* Render scale asked for at run time, in percent (a game option: NFSU2's
 * Options -> Video); 0 = none, keep RECOMP_GL_SCALE. Applied at the next
 * flip (rescale_surfaces). */
volatile int nv2a_vk_scale_pct;

/* The stored size of a w x h surface at the current scale, capped per axis
 * by the device's image limits. */
static void stored_size(uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t *pw, uint32_t *ph)
{
    double kx = s_scale_x * (s_square ? s_wide * 0.75 / (aa_sx > 1 ? aa_sx : 1) : 1.0), ky = s_scale;
    if (w * kx > (double)s_max_size)
        kx = (double)s_max_size / w;
    if (h * ky > (double)s_max_size)
        ky = (double)s_max_size / h;
    *pw = (uint32_t)(w * kx + 0.5); *ph = (uint32_t)(h * ky + 0.5);
    if (!*pw) *pw = 1;
    if (!*ph) *ph = 1;
}

/* v title pixels of a surface l wide, in the p stored pixels behind it. */
static uint32_t to_stored(uint32_t v, uint32_t p, uint32_t l)
{
    return (uint32_t)(((uint64_t)v * p + l / 2) / l);
}
static VkSurf     s_surf[VK_MAX_SURF];
static VkDepthBuf s_depth[VK_MAX_DEPTH];
static VkSurf    *s_last;

static int         s_in_render;
static VkSurf     *s_rt;
static VkDepthBuf *s_rt_depth;

/* Barriers. Every one drains the GPU on NVK, so a pass only gets one when
 * it depends on earlier work (RECOMP_VK_SYNC=0: before every pass, as it
 * used to be):
 *  - a copy, clear or blit since the last barrier (s_transfer_pending; also
 *    set at the start of every command buffer) -> barrier_all;
 *  - its colour surface or depth buffer was drawn into, or the surface was
 *    sampled, since the last barrier, or the draw samples a surface drawn
 *    into since then (s_read_pending) -> a barrier over the attachment and
 *    fragment stages;
 *  - otherwise (a surface not touched since the last barrier: the cube
 *    faces, reflection targets, ...) none.
 * Both kinds cover every image, so either one clears all the flags. */
static int s_read_pending;
static uint32_t s_st_bar_full, s_st_bar_gfx, s_st_bar_skip;

static void barrier_flags_clear(void)
{
    int i;
    for (i = 0; i < VK_MAX_SURF; i++)
        s_surf[i].dirty = s_surf[i].sampled = 0;
    for (i = 0; i < VK_MAX_DEPTH; i++)
        s_depth[i].dirty = 0;
    s_read_pending = 0;
}

static void barrier_all(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    barrier_flags_clear();
    s_transfer_pending = 0;
    s_st_bar_full++;
}

/* Attachment writes -> later attachment use and fragment shader reads;
 * fragment shader reads -> later attachment writes. */
static void barrier_gfx(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb, st, st, 0, 1, &mb, 0, NULL, 0, NULL);
    barrier_flags_clear();
    s_st_bar_gfx++;
}

/* Before a pass on s (and d): the barrier it needs, if any. */
static void pass_barrier(VkSurf *s, VkDepthBuf *d)
{
    static int sync = -1;
    if (sync < 0) {
        const char *e = getenv("RECOMP_VK_SYNC");
        sync = !(e && *e == '0');
        fprintf(stderr, "  [VK] pass barriers: %s\n", sync ? "only where needed" : "every pass");
    }
    if (!sync || s_transfer_pending)
        barrier_all();
    else if (s_read_pending || s->dirty || s->sampled || (d && d->dirty))
        barrier_gfx();
    else
        s_st_bar_skip++;
}

static void end_rendering(void)
{
    if (s_in_render) {
        p_end_rendering(s_cb);
        s_in_render = 0;
    }
}

static VkRect2D s_sc_cur;               /* scissor set in this pass */

static void set_viewport(uint32_t pw, uint32_t ph)
{
    VkViewport vp = { 0, 0, (float)pw, (float)ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { pw, ph } };
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    s_sc_cur = sc;
}

/* SET_SURFACE_CLIP as the scissor, in s's stored pixels. NFSU2's split
 * screen draws each player's world with the clip set to that player's
 * half; the vertex programs place it there but do not stop at the edge.
 * Same as nv2a_gl.c (and xemu). Call inside the pass (begin_rendering
 * resets it to the whole surface). */
static void set_surface_scissor(const uint32_t *r, const VkSurf *s)
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;
    VkRect2D sc = { { 0, 0 }, { s->pw, s->ph } };

    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 > x0 && y1 > y0) {
        sc.offset.x = (int32_t)to_stored(x0, s->pw, s->w);
        sc.offset.y = (int32_t)to_stored(y0, s->ph, s->h);
        sc.extent.width = to_stored(x1, s->pw, s->w) - (uint32_t)sc.offset.x;
        sc.extent.height = to_stored(y1, s->ph, s->h) - (uint32_t)sc.offset.y;
    }
    if (memcmp(&sc, &s_sc_cur, sizeof sc)) {
        vkCmdSetScissor(s_cb, 0, 1, &sc);
        s_sc_cur = sc;
    }
}

static void begin_rendering(VkSurf *s, VkDepthBuf *d)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };

    if (s_in_render && s_rt == s && s_rt_depth == d)
        return;
    if (kick_due(0))
        kick_submit();
    end_rendering();
    pass_barrier(s, d);
    ca.imageView = s->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    if (d) {
        if (d->pw < ri.renderArea.extent.width) ri.renderArea.extent.width = d->pw;
        if (d->ph < ri.renderArea.extent.height) ri.renderArea.extent.height = d->ph;
    }
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (d->fresh) {
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.clearValue.depthStencil.depth = 1.0f;
            da.clearValue.depthStencil.stencil = 0;
            d->fresh = 0;
        }
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    p_begin_rendering(s_cb, &ri);
    s_in_render = 1;
    s_rt = s;
    s_rt_depth = d;
    if (d) {
        d->dirty = 1;
        d->wgen++;
    }
    set_viewport(s->pw, s->ph);
}

/* An image in GENERAL layout, from UNDEFINED; recorded outside rendering. */
static void image_to_general(VkImage img, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    end_rendering();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ── Image memory pool (RECOMP_VK_TEX_POOL) ───────────────────────────
 *
 * Every image had an allocation of its own: a vkAllocateMemory when a
 * texture or surface first appeared and a vkFreeMemory when it went, each
 * a round trip through the kernel on Horizon -- a burst of them when a race
 * loads made the hitches. Images up to 16 MB now take a range of a 32 MB
 * device-local block (up to 64 blocks, first fit); the range goes back
 * when the image is destroyed (garbage_free, after its frame's fence).
 * Larger images, and everything once the blocks are full, allocate as
 * before. RECOMP_VK_TEX_POOL=0 off. */
#define POOL_BLOCK      (32ull << 20)
#define POOL_MAX_BLOCKS 64
#define POOL_MAX_ALLOC  (16ull << 20)
#define POOL_HASH       8192

typedef struct { VkDeviceSize off, size; } PoolRange;
typedef struct {
    VkDeviceMemory mem;
    uint32_t       type;
    PoolRange     *free;             /* by offset, never adjacent */
    uint32_t       nfree, cap;
} PoolBlock;
static PoolBlock s_pool[POOL_MAX_BLOCKS];
static uint32_t  s_npool;
static int       s_pool_on = -1;
/* Pooled images: which range they hold. Linear probing. */
static struct { VkImage img; uint32_t block; VkDeviceSize off, size; } s_pool_map[POOL_HASH];

static uint32_t pool_slot(VkImage img)
{
    uint64_t k = (uint64_t)(uintptr_t)img;
    k ^= k >> 29;
    k *= 0xbf58476d1ce4e5b9ull;
    k ^= k >> 32;
    return (uint32_t)k & (POOL_HASH - 1);
}

static void pool_put(PoolBlock *b, VkDeviceSize off, VkDeviceSize size)
{
    uint32_t i = 0;
    while (i < b->nfree && b->free[i].off < off)
        i++;
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == off) {
        b->free[i - 1].size += size;                /* joins the one before */
        if (i < b->nfree && off + size == b->free[i].off) {
            b->free[i - 1].size += b->free[i].size;
            memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
            b->nfree--;
        }
        return;
    }
    if (i < b->nfree && off + size == b->free[i].off) {
        b->free[i].off = off;                       /* joins the one after */
        b->free[i].size += size;
        return;
    }
    if (b->nfree == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->free = (PoolRange *)realloc(b->free, b->cap * sizeof *b->free);
    }
    memmove(&b->free[i + 1], &b->free[i], (b->nfree - i) * sizeof *b->free);
    b->free[i].off = off;
    b->free[i].size = size;
    b->nfree++;
}

static int pool_take(PoolBlock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *at)
{
    uint32_t i;
    for (i = 0; i < b->nfree; i++) {
        VkDeviceSize o = (b->free[i].off + align - 1) & ~(align - 1);
        VkDeviceSize end = b->free[i].off + b->free[i].size, lo = b->free[i].off;
        if (o + size > end)
            continue;
        memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
        b->nfree--;
        if (o > lo)
            pool_put(b, lo, o - lo);
        if (o + size < end)
            pool_put(b, o + size, end - (o + size));
        *at = o;
        return 1;
    }
    return 0;
}

/* Bind img to a pooled range; 0: allocate it as before. */
static int pool_bind(VkImage img, const VkMemoryRequirements *rq)
{
    uint32_t type, i, h;
    VkDeviceSize at;
    if (s_pool_on < 0) {
        const char *e = getenv("RECOMP_VK_TEX_POOL");
        s_pool_on = !(e && *e == '0');
    }
    if (!s_pool_on || rq->size > POOL_MAX_ALLOC)
        return 0;
    type = mem_type(rq->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return 0;
    h = pool_slot(img);
    for (i = 0; s_pool_map[h].img && i < POOL_HASH; i++)
        h = (h + 1) & (POOL_HASH - 1);
    if (s_pool_map[h].img)
        return 0;                                   /* map full */
    for (i = 0; i <= s_npool; i++) {
        PoolBlock *b = &s_pool[i];
        if (i == s_npool) {
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            if (s_npool == POOL_MAX_BLOCKS)
                return 0;
            ai.allocationSize = POOL_BLOCK;
            ai.memoryTypeIndex = type;
            memset(b, 0, sizeof *b);
            if (vkAllocateMemory(s_dev, &ai, NULL, &b->mem) != VK_SUCCESS)
                return 0;
            b->type = type;
            pool_put(b, 0, POOL_BLOCK);
            s_npool++;
            LOGE("texture pool: block %u (%u MB)\n", s_npool, (unsigned)(POOL_BLOCK >> 20));
        }
        if (b->type != type || !pool_take(b, rq->size, rq->alignment, &at))
            continue;
        if (vkBindImageMemory(s_dev, img, b->mem, at) != VK_SUCCESS) {
            pool_put(b, at, rq->size);
            return 0;
        }
        s_pool_map[h].img = img;
        s_pool_map[h].block = i;
        s_pool_map[h].off = at;
        s_pool_map[h].size = rq->size;
        return 1;
    }
    return 0;
}

/* img is being destroyed: its range, if it has one, goes back. */
static void pool_release(VkImage img)
{
    uint32_t h = pool_slot(img), n, j;
    for (n = 0; n < POOL_HASH && s_pool_map[h].img != img; n++) {
        if (!s_pool_map[h].img)
            return;
        h = (h + 1) & (POOL_HASH - 1);
    }
    if (n == POOL_HASH)
        return;
    pool_put(&s_pool[s_pool_map[h].block], s_pool_map[h].off, s_pool_map[h].size);
    s_pool_map[h].img = VK_NULL_HANDLE;
    /* Close the gap: move up later entries of the run that hash at or
     * before it. */
    for (j = (h + 1) & (POOL_HASH - 1); s_pool_map[j].img; j = (j + 1) & (POOL_HASH - 1)) {
        uint32_t k = pool_slot(s_pool_map[j].img);
        if ((j > h) ? (k <= h || k > j) : (k <= h && k > j)) {
            s_pool_map[h] = s_pool_map[j];
            s_pool_map[j].img = VK_NULL_HANDLE;
            h = j;
        }
    }
}

/* layers: 1 = a 2D image, 6 = a cube map (cube view); levels: mip levels. */
static int make_image_ml(uint32_t w, uint32_t h, uint32_t layers, uint32_t levels, VkFormat fmt,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements rq;

    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    if (layers == 6)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, *img, &rq);
    *mem = VK_NULL_HANDLE;
    if (!pool_bind(*img, &rq)) {
        *mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!*mem || vkBindImageMemory(s_dev, *img, *mem, 0) != VK_SUCCESS) {
            vkDestroyImage(s_dev, *img, NULL);
            if (*mem) vkFreeMemory(s_dev, *mem, NULL);
            *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
            return 0;
        }
    }
    vi.image = *img;
    vi.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        if (*mem) vkFreeMemory(s_dev, *mem, NULL);
        else pool_release(*img);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    image_to_general(*img, aspect);
    return 1;
}

static int make_image_n(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    return make_image_ml(w, h, layers, 1, fmt, usage, aspect, img, view, mem);
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkImageView *view,
                      VkDeviceMemory *mem)
{
    return make_image_n(w, h, 1, fmt, usage, aspect, img, view, mem);
}

/* ── Surfaces ─────────────────────────────────────────────────────── */

static VkSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

static void clear_image_color(VkImage img, float r, float g, float b, float a)
{
    VkClearColorValue c = { { r, g, b, a } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                   VK_REMAINING_ARRAY_LAYERS };
    end_rendering();
    vkCmdClearColorImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
    s_transfer_pending = 1;
}

static VkSurf *surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    VkSurf *s = surf_find(va), *victim = NULL;
    int i;

    /* h is the surface clip, not the allocation (NFSU2's split screen clips
     * the back buffer to each player's half): keep the surface while the
     * clip fits, grow it when it does not. Same as nv2a_gl.c. */
    if (s && s->w == w && s->h >= h) {
        s->used = s_frame;
        return s;
    }
    if (s && s->w == w && s->h > h)
        h = s->h;
    if (!s) {
        for (i = 0; i < VK_MAX_SURF; i++) {
            if (!s_surf[i].image) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->image) {
        if (s_rt == s) end_rendering(), s_rt = NULL;
        if (s_last == s) s_last = NULL;
        garbage_add(s->image, s->view, s->mem);
    }
    memset(s, 0, sizeof *s);
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    stored_size(w, h, aa_sx, &s->pw, &s->ph);
    if (!make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->view, &s->mem)) {
        LOGE("surface 0x%08X %ux%u: image creation failed\n", va, w, h);
        memset(s, 0, sizeof *s);
        return NULL;
    }
    clear_image_color(s->image, 0, 0, 0, 1);
    if (s_trace)
        LOGE("surface 0x%08X %ux%u (aa %ux%u, stored %ux%u)\n", va, w, h, aa_sx, aa_sy,
             s->pw, s->ph);
    return s;
}

/* A new render scale (nv2a_vk_scale_pct), at a flip, outside rendering:
 * every surface is rebuilt at its new stored size with its pixels scaled
 * over -- render targets the title keeps across frames (glow accumulators,
 * reflection faces, the frame being presented) do not go black -- and the
 * depth buffers are dropped; depth_get makes new ones on the next draw. */
static void rescale_surfaces(double k)
{
    int i, copied = 0;

    s_scale = k;
    s_scale_x = k * s_xratio;
    fprintf(stderr, "  [VK] rendering at %g x %g%s\n", s_scale_x, k,
            s_square ? ", square pixels" : "");
    for (i = 0; i < VK_MAX_SURF; i++) {
        VkSurf *s = &s_surf[i];
        VkImage img; VkImageView view; VkDeviceMemory mem;
        VkImageBlit bl;
        uint32_t pw, ph;
        if (!s->image)
            continue;
        stored_size(s->w, s->h, s->aa_sx, &pw, &ph);
        if (pw == s->pw && ph == s->ph)
            continue;
        if (!make_image(pw, ph, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &img, &view, &mem)) {
            LOGE("surface 0x%08X %ux%u: image creation failed\n", s->va, pw, ph);
            continue;
        }
        if (!copied) {
            barrier_all();
            copied = 1;
        }
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)s->pw;
        bl.srcOffsets[1].y = (int32_t)s->ph;
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1].x = (int32_t)pw;
        bl.dstOffsets[1].y = (int32_t)ph;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        s_transfer_pending = 1;
        garbage_add(s->image, s->view, s->mem);
        s->image = img; s->view = view; s->mem = mem;
        s->pw = pw; s->ph = ph;
        s->gen++;                       /* cube maps built from it refill */
    }
    for (i = 0; i < VK_MAX_DEPTH; i++)
        if (s_depth[i].image) {
            garbage_add(s_depth[i].image, s_depth[i].view, s_depth[i].mem);
            if (s_depth[i].sview) garbage_add(VK_NULL_HANDLE, s_depth[i].sview, VK_NULL_HANDLE);
            memset(&s_depth[i], 0, sizeof s_depth[i]);
        }
    if (copied)
        barrier_all();
}

/* Z buffer addresses the title has sampled as a texture (depth_as_color):
 * their depth images are made with SAMPLED usage. Only those: every other
 * depth image keeps the usage NVK can use ZCULL with (see depth_get). */
static uint32_t s_zs_va[8];

static int zs_wanted(uint32_t va)
{
    int i;
    for (i = 0; i < 8; i++)
        if (s_zs_va[i] == va)
            return 1;
    return 0;
}

/* The most recently used depth buffer at va, or NULL. */
static VkDepthBuf *depth_at(uint32_t va)
{
    VkDepthBuf *d = NULL;
    int i;
    for (i = 0; i < VK_MAX_DEPTH; i++)
        if (s_depth[i].image && s_depth[i].va == va && (!d || s_depth[i].used > d->used))
            d = &s_depth[i];
    return d;
}

static VkDepthBuf *depth_get(uint32_t va, uint32_t pw, uint32_t ph)
{
    VkDepthBuf *d, *victim = NULL;
    int i;

    for (i = 0; i < VK_MAX_DEPTH; i++) {
        if (s_depth[i].image && s_depth[i].va == va && s_depth[i].pw == pw && s_depth[i].ph == ph) {
            s_depth[i].used = s_frame;
            return &s_depth[i];
        }
        if (!victim || (victim->image && (!s_depth[i].image || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->image) {
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
        if (d->sview) garbage_add(VK_NULL_HANDLE, d->sview, VK_NULL_HANDLE);
    }
    memset(d, 0, sizeof *d);
    d->va = va; d->pw = pw; d->ph = ph; d->used = s_frame;
    d->sampleable = zs_wanted(va);
    /* No TRANSFER_DST: with it NVK leaves ZCULL off for the image (StevensND/
     * nfsmw-nx measured ~2 ms of GPU per frame; nfsu2-sw VULKAN_NOTES.md).
     * The first pass that uses it clears it (loadOp CLEAR, begin_rendering). */
    if (!make_image(pw, ph, s_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        | (d->sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    &d->image, &d->view, &d->mem)) {
        LOGE("depth 0x%08X %ux%u: image creation failed\n", va, pw, ph);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    d->fresh = 1;
    if (s_trace)
        LOGE("depth 0x%08X %ux%u\n", va, pw, ph);
    return d;
}

/* The target of a draw or clear: surface (created on first use) and the
 * depth buffer of its zeta address. */
static VkSurf *target(const Nv2aSurface *sf, uint32_t zeta_va, VkDepthBuf **dout)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;   /* the clip need not start at 0 */
    VkSurf *s;

    *dout = NULL;
    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !sf->color_va || w > 4096 || h > 4096)
        return NULL;
    if (s_trace) {
        static uint32_t last[6];
        uint32_t cur[6] = { sf->color_va, sf->pitch, sf->clip_x, sf->clip_y, sf->width, sf->height };
        if (memcmp(cur, last, sizeof cur)) {
            memcpy(last, cur, sizeof cur);
            LOGE("target 0x%08X pitch %u bpp %u clip %u,%u %ux%u -> %ux%u\n", sf->color_va,
                 sf->pitch, bpp, sf->clip_x, sf->clip_y, sf->width, sf->height, w, h);
        }
    }
    s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1, sf->aa_sy ? sf->aa_sy : 1);
    if (s && zeta_va)
        *dout = depth_get(zeta_va, s->pw, s->ph);
    if (s)
        s_last = s;
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    uint32_t face;              /* cube map: bytes per face; 0 = 2D */
    uint32_t levels;            /* mip levels in the image */
    uint32_t palette_va, palette_reg;   /* format 0x0B: the palette it was decoded with */
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkFormat fmt;
    uint32_t used, checked;
} VkTex;

/* RECOMP_VK_MIPS=0: level 0 only (the old look). RECOMP_VK_ANISO=<n>:
 * anisotropic filtering of mipmapped textures, 1 = off (default 16, capped
 * by the device). RECOMP_VK_LOD_BIAS: their mip bias. */
static int   s_mips = 1;
static float s_aniso = 16.0f, s_lod_bias = -0.25f;

#define VK_MAX_TEX 1024
#define TEX_BUCKETS 2048
static VkTex    s_tex[VK_MAX_TEX];
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[VK_MAX_TEX];
static uint64_t s_tex_bytes;
static uint32_t s_palette_reg, s_palette_va;
static VkImage  s_dummy_img;
static VkImageView s_dummy_view;
static VkDeviceMemory s_dummy_mem;
/* A transparent black cube, for a cube map stage with nothing to sample. */
static VkImage  s_dummy_cube_img;
static VkImageView s_dummy_cube_view;
static VkDeviceMemory s_dummy_cube_mem;

uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}
static void tex_index_add(VkTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}
static void tex_index_del(VkTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) { *at = s_tex_next[i]; return; }
        at = &s_tex_next[*at - 1];
    }
}

static int tex_size_from_format(uint32_t color)
{
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;
    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

/* Every byte, for linear images. A sampled hash steps bytes/256, which for a
 * power-of-two pitch is a whole number of rows: every sample lands in the same
 * column. Carbon's EA logo movie (512x256 A8R8G8B8, pitch 2048, a ring of
 * three) is black down that column, so its frames were never re-uploaded and
 * the logo cycled through the first three. Linear images are few (movies,
 * CPU-written pictures), so reading all of them once a frame is cheap. */
static uint32_t bytes_hash_full(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint64_t a = 14695981039346656037ull ^ bytes, b = 1099511628211ull, x, y;
    uint32_t i;

    for (i = 0; i + 16 <= bytes; i += 16) {
        memcpy(&x, mem + i, 8);
        memcpy(&y, mem + i + 8, 8);
        a = (a ^ x) * 1099511628211ull;
        b = (b ^ y) * 1099511628211ull;
    }
    for (; i < bytes; i++)
        a = (a ^ mem[i]) * 1099511628211ull;
    a ^= b * 31u;
    return (uint32_t)(a ^ (a >> 32));
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;
}

static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;
    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* The swizzle splits into an x part and a y part (their bits never share a
 * position), so a whole image needs two small tables instead of a bit loop
 * per texel: index(u, v) = s_swz_x[u] | s_swz_y[v]. */
static uint32_t s_swz_x[4096], s_swz_y[4096], s_swz_w, s_swz_h;

static void swizzle_tables(uint32_t w, uint32_t h)
{
    uint32_t i;
    if (w == s_swz_w && h == s_swz_h)
        return;
    for (i = 0; i < w; i++) s_swz_x[i] = swizzle_index(i, 0, w, h);
    for (i = 0; i < h; i++) s_swz_y[i] = swizzle_index(0, i, w, h);
    s_swz_w = w;
    s_swz_h = h;
}

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;
    uint32_t pal[256];
    if (!(s_palette_reg & ~0x3Fu))
        return 0;
    memcpy(pal, mem + s_palette_va, entries * 4);
    for (x = entries; x < 256; x++)
        pal[x] = pal[0];                     /* past the end reads entry 0 */
    swizzle_tables(w, h);
    for (y = 0; y < h; y++) {
        const uint8_t *row = idx + s_swz_y[y];
        uint32_t *o = out + (size_t)y * w;
        for (x = 0; x < w; x++)
            o[x] = pal[row[s_swz_x[x]]];
    }
    return 1;
}

static void tex_free(VkTex *t)
{
    garbage_add(t->image, t->view, t->mem);
    s_tex_bytes -= t->bytes;
    tex_index_del(t);
    memset(t, 0, sizeof *t);
}

static void tex_account(VkTex *t, uint32_t bytes)
{
    static uint64_t budget;
    s_hz_texs++;
    s_hz_tex_bytes += bytes;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        VkTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (c->image && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;
        tex_free(lru);
    }
}

/* Copy the image's texels, already in the ring at `at` (row length
 * `row_texels`, 0 = tight), into mip level `level` of layer `layer`. */
static void tex_copy_from_ring(VkTex *t, VkDeviceSize at, uint32_t row_texels,
                               uint32_t layer, uint32_t level)
{
    VkBufferImageCopy rg;

    end_rendering();
    if (!level)
        barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.bufferRowLength = row_texels;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.mipLevel = level;
    rg.imageSubresource.baseArrayLayer = layer;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = t->w >> level ? t->w >> level : 1;
    rg.imageExtent.height = t->h >> level ? t->h >> level : 1;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    s_transfer_pending = 1;
}

/* Copy `bytes` at `src` (row length `row_texels`, 0 = tight) into the image. */
static void tex_upload(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels,
                       uint32_t layer, uint32_t level)
{
    void *dst;
    VkDeviceSize at = ring_alloc(bytes, 16, &dst);

    if (!dst)
        return;
    memcpy(dst, src, bytes);
    tex_copy_from_ring(t, at, row_texels, layer, level);
}

/* Mip levels 1.. of every layer from level 0, by GPU blits (decoded
 * textures: building them costs no CPU, unlike decoding the title's). */
static void gen_mips(VkImage img, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    uint32_t l;

    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    for (l = 1; l < levels; l++) {
        VkImageBlit bl;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.mipLevel = l - 1;
        bl.srcSubresource.layerCount = layers;
        bl.srcOffsets[1].x = (int32_t)w;
        bl.srcOffsets[1].y = (int32_t)h;
        bl.srcOffsets[1].z = 1;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstSubresource.mipLevel = l;
        bl.dstOffsets[1].x = (int32_t)w;
        bl.dstOffsets[1].y = (int32_t)h;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        s_transfer_pending = 1;
    }
}

static uint32_t full_levels(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while ((w > h ? w : h) >> n) n++;
    return n;
}

/* Cube map faces follow each other, each with its mip chain, padded to 128
 * bytes (xemu's NV2A_CUBEMAP_FACE_ALIGNMENT); +X -X +Y -Y +Z -Z, the same
 * order and texel orientation as Vulkan's cube layers. */
static uint32_t cube_face_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t n = 0, l;

    if (!levels)
        levels = 1;
    for (l = 0; l < levels; l++) {
        n += tex_bytes(color, w, h, 0);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (n + 127) & ~127u;
}

/* One 2D image (layer 0) or cube face from guest memory at va. */
static void tex_fill(VkTex *t, uint32_t va, uint32_t layer)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t color = t->color, w = t->w, h = t->h, pitch = t->pitch, *px;
    Nv2aTexture nt;
    VkDeviceSize at;
    void *map;

    if (t->fmt != VK_FORMAT_B8G8R8A8_UNORM) {
        /* DXT as it is, with the title's mip levels (they follow level 0). */
        uint32_t l, lw = w, lh = h;
        for (l = 0; l < t->levels; l++) {
            uint32_t n = tex_bytes(color, lw, lh, 0);
            tex_upload(t, mem + va, n, 0, layer, l);
            va += n;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        return;
    }
    if (color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        /* Linear A8R8G8B8 (movie frames): the bytes are B8G8R8A8 already. */
        tex_upload(t, mem + va, pitch * (h - 1) + w * 4, pitch / 4, layer, 0);
        return;
    }
    /* Everything else is decoded straight into the upload ring. */
    at = ring_alloc(w * h * 4, 16, &map);
    px = (uint32_t *)map;
    if (!px)
        return;
    if (color == 0x06 || color == 0x07) {
        const uint32_t *src = (const uint32_t *)(mem + va);
        uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
        swizzle_tables(w, h);
        for (y = 0; y < h; y++) {
            const uint32_t *row = src + s_swz_y[y];
            uint32_t *out = px + (size_t)y * w;
            for (x = 0; x < w; x++)
                out[x] = row[s_swz_x[x]] | fill;
        }
    } else {
        int ok;
        memset(&nt, 0, sizeof nt);
        nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
        nt.addr_u = nt.addr_v = 1;
        ok = color == 0x0B ? decode_indexed(va, w, h, px)
                           : nv2a_backend_decode_texture(&nt, px);
        if (!ok) {
            uint32_t k;
            for (k = 0; k < w * h; k++)
                px[k] = 0xFFFF00FFu;
            if (s_trace)
                LOGE("texture 0x%08X format 0x%02X not decodable\n", va, color);
        }
    }
    nv2a_tex_decoded(va, color, w, h, px);       /* dump / a port's replacement art */
    tex_copy_from_ring(t, at, 0, layer, 0);
}

/* face: bytes per cube face (cube_face_bytes), 0 for a 2D texture;
 * levels: mip levels to sample (1 = none). */
static VkImageView tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                           uint32_t face, uint32_t levels)
{
    VkTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t layers = face ? 6 : 1, f, size;

    {
        /* DXT goes to the GPU compressed, past nv2a_tex_decoded: decoded
         * instead while RECOMP_TEX_DUMP is set (all of them), and up to
         * 128x128 when a port replaces textures (its icons). */
        static int dump = -1;
        int bc = s_have_bc;
        if (dump < 0) {
            const char *e = getenv("RECOMP_TEX_DUMP");
            dump = e && *e;
        }
        if (dump || (nv2a_tex_hook && w <= 128 && h <= 128))
            bc = 0;
        if (bc && color == 0x0C) fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (bc && color == 0x0E) fmt = VK_FORMAT_BC2_UNORM_BLOCK;
        if (bc && color == 0x0F) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    }

    for (i = s_tex_head[tex_bucket(va, color, w, h)]; i; i = s_tex_next[i - 1]) {
        VkTex *c = &s_tex[i - 1];
        if (c->image && c->va == va && c->color == color && c->w == w && c->h == h
            && c->face == face && c->pitch == pitch && c->levels == levels && c->fmt == fmt
            && (color != 0x0B || (c->palette_va == s_palette_va && c->palette_reg == s_palette_reg))) {
            t = c;
            break;
        }
    }
    if (t && t->checked == s_frame) {
        t->used = s_frame;
        return t->view;
    }
    size = tex_bytes(color, w, h, pitch);
    if (!face && !pitch && fmt != VK_FORMAT_B8G8R8A8_UNORM) {
        /* DXT mip levels are uploaded from guest memory (they follow level 0),
         * so a change to them alone must re-upload too (nfsu2-sw 3911d7e). */
        uint32_t lw = w, lh = h, level;
        for (level = 1; level < levels; level++) {
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
            size += tex_bytes(color, lw, lh, 0);
        }
    }
    hash = pitch ? bytes_hash_full(va, size)
             : bytes_hash(va, face ? face * 6 : size);
    if (color == 0x0B)
        hash ^= bytes_hash(s_palette_va, 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        return t->view;
    }
    if (s_tex_changes < 0) {
        const char *e = getenv("RECOMP_TEX_CHANGES");
        s_tex_changes = e && atoi(e) > 0;
    }
    if (t && s_tex_changes)
        fprintf(stderr, "[tex] frame %u: %08X format %02X %ux%u changed\n",
                s_frame, va, color, w, h);
    if (!t) {
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (!c->image) { if (!victim || victim->image) victim = c; }
            else if (!victim || (victim->image && c->used < victim->used)) victim = c;
        }
        t = victim;
        if (t->image)
            tex_free(t);
        t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch; t->fmt = fmt;
        t->face = face; t->levels = levels;
        t->palette_va = s_palette_va; t->palette_reg = s_palette_reg;
        VT("texture %08X fmt %02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
           face ? " cube" : "");
        if (!make_image_ml(w, h, layers, levels, fmt,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->view, &t->mem)) {
            memset(t, 0, sizeof *t);
            return face ? s_dummy_cube_view : s_dummy_view;
        }
        tex_index_add(t);
        if (s_trace)
            LOGE("texture 0x%08X format 0x%02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
                 face ? " cube" : "");
    }
    t->hash = hash;
    t->used = t->checked = s_frame;

    for (f = 0; f < layers; f++)
        tex_fill(t, va + f * face, f);
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM && levels > 1)
        gen_mips(t->image, w, h, levels, layers);
    size = fmt != VK_FORMAT_B8G8R8A8_UNORM ? tex_bytes(color, w, h, 0) : w * h * 4;
    if (levels > 1)
        size += size / 3;                   /* the mip chain */
    tex_account(t, size * layers);
    return t->view;
}

/* A cube map whose faces the title renders (race car reflections: six
 * 128x128 targets, one per face, every frame). The faces exist only as
 * surface images, so the cube is assembled from them on the GPU, again
 * whenever a face was drawn into since the last copy. */
typedef struct {
    uint32_t va, w, h, face, size, levels;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkSurf  *src[6];
    uint32_t gen[6];
    uint32_t used;
} VkCube;
#define VK_MAX_CUBE 4
static VkCube s_cube[VK_MAX_CUBE];

static VkImageView cube_from_surfaces(uint32_t va, uint32_t w, uint32_t h, uint32_t face,
                                      VkSurf *faces[6])
{
    VkCube *c = NULL;
    uint32_t f, size = 0;
    int i, copied = 0;

    for (f = 0; f < 6; f++)
        if (faces[f] && to_stored(w, faces[f]->pw, faces[f]->w) > size)
            size = to_stored(w, faces[f]->pw, faces[f]->w);
    for (i = 0; i < VK_MAX_CUBE; i++)
        if (s_cube[i].image && s_cube[i].va == va && s_cube[i].w == w && s_cube[i].h == h
            && s_cube[i].face == face && s_cube[i].size == size) {
            c = &s_cube[i];
            break;
        }
    if (!c) {
        for (i = 0; i < VK_MAX_CUBE; i++)
            if (!c || !s_cube[i].image || (c->image && s_cube[i].used < c->used))
                c = &s_cube[i];
        if (c->image)
            garbage_add(c->image, c->view, c->mem);
        memset(c, 0, sizeof *c);
        /* Mipmapped (sampled trilinear, bind_stage): a reflection on a
         * curved or distant body no longer sparkles. */
        c->levels = s_mips ? full_levels(size, size) : 1;
        if (!make_image_ml(size, size, 6, c->levels, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->image, &c->view, &c->mem)) {
            memset(c, 0, sizeof *c);
            return s_dummy_cube_view;
        }
        clear_image_color(c->image, 0, 0, 0, 0);
        c->va = va; c->w = w; c->h = h; c->face = face; c->size = size;
        if (s_trace)
            LOGE("cube 0x%08X %ux%u from surfaces (stored %u)\n", va, w, h, size);
    }
    c->used = s_frame;
    for (f = 0; f < 6; f++) {
        VkSurf *sf = faces[f];
        VkImageBlit bl;
        if (!sf || (c->src[f] == sf && c->gen[f] == sf->gen))
            continue;
        if (!copied) {
            end_rendering();
            barrier_all();
            copied = 1;
        }
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)to_stored(w, sf->pw, sf->w);
        bl.srcOffsets[1].y = (int32_t)to_stored(h, sf->ph, sf->h);
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.dstSubresource.baseArrayLayer = f;
        bl.dstSubresource.layerCount = 1;
        bl.dstOffsets[1].x = (int32_t)size;
        bl.dstOffsets[1].y = (int32_t)size;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, sf->image, VK_IMAGE_LAYOUT_GENERAL, c->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        s_transfer_pending = 1;
        c->src[f] = sf;
        c->gen[f] = sf->gen;
    }
    if (copied) {
        if (c->levels > 1)
            gen_mips(c->image, size, size, c->levels, 6);
        barrier_all();
    }
    return c->view;
}

/* The copy of a render target a draw into it samples (see vk_draw); one
 * scratch image the size of that surface (sampled with its coordinates),
 * refreshed once per surface write. */
static VkImage s_snap_img;
static VkImageView s_snap_view;
static VkDeviceMemory s_snap_mem;
static uint32_t s_snap_w, s_snap_h, s_snap_va, s_snap_gen, s_snap_cb = ~0u;

static VkImageView self_snapshot(VkSurf *sf)
{
    VkImageCopy cp;
    if (sf->pw != s_snap_w || sf->ph != s_snap_h) {
        uint32_t w = sf->pw, h = sf->ph;
        VkImage img; VkImageView view; VkDeviceMemory mem;
        if (!make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &img, &view, &mem))
            return sf->view;
        if (s_snap_img)
            garbage_add(s_snap_img, s_snap_view, s_snap_mem);
        s_snap_img = img; s_snap_view = view; s_snap_mem = mem;
        s_snap_w = w; s_snap_h = h;
        s_snap_va = 0;
    }
    if (s_snap_va == sf->va && s_snap_gen == sf->gen && s_snap_cb == s_cb_gen)
        return s_snap_view;
    end_rendering();
    barrier_all();
    memset(&cp, 0, sizeof cp);
    cp.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.srcSubresource.layerCount = 1;
    cp.dstSubresource = cp.srcSubresource;
    cp.extent.width = sf->pw;
    cp.extent.height = sf->ph;
    cp.extent.depth = 1;
    vkCmdCopyImage(s_cb, sf->image, VK_IMAGE_LAYOUT_GENERAL, s_snap_img,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    barrier_all();
    s_snap_va = sf->va; s_snap_gen = sf->gen; s_snap_cb = s_cb_gen;
    return s_snap_view;
}

/* Samplers: wrap u, wrap v, mag, min, mipmapped. */
typedef struct { uint32_t key; VkSampler s; } SamplerEnt;
static SamplerEnt s_samplers[512];
static int s_nsamplers;

static VkSamplerAddressMode wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler_get(uint32_t key)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i;
    for (i = 0; i < s_nsamplers; i++)
        if (s_samplers[i].key == key)
            return s_samplers[i].s;
    ci.addressModeU = wrap_mode(key & 0xF);
    ci.addressModeV = wrap_mode((key >> 8) & 0xF);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.magFilter = (key & 0x10000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (key & 0x20000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = 0.0f;
    if (key & 0x40000) {
        /* The title's mipmapped filters, always trilinear (GPU time only). */
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.maxLod = VK_LOD_CLAMP_NONE;
        ci.mipLodBias = s_lod_bias;
        if (key >> 20) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = (float)(key >> 20);
        }
    }
    ci.borderColor = (key & 0x2000) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : (key & 0x1000) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                   : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (s_nsamplers == 512)
        s_nsamplers = 0;                    /* never in practice: 6 x 6 x 8 keys x levels */
    if (vkCreateSampler(s_dev, &ci, NULL, &s_samplers[s_nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    s_samplers[s_nsamplers].key = key;
    return s_samplers[s_nsamplers++].s;
}

/* A surface kept taller than its clip (surf_get) and sampled as a texture
 * of the clip's size: texcoords cover the w x h texels at the top left, not
 * the whole surface. A texture of the surface's logical or real size keeps
 * the old full-surface mapping (anti-aliased surfaces). */
static void rt_scale(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                     uint32_t aa_sx, uint32_t aa_sy, float scale[2])
{
    uint32_t lw = sw / (aa_sx ? aa_sx : 1), lh = sh / (aa_sy ? aa_sy : 1);

    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
}

/* Car reflections on/off at run time, for a game option (NFSU2: Options ->
 * Video). Read on the executor thread at every cube stage bind. */
volatile int nv2a_vk_cube_maps = 1;

/* ── A Z buffer sampled as a colour texture ───────────────────────────
 *
 * Port of nv2a_gl.c's depth_as_color. Conker's depth of field (cutscene
 * close-ups, the dialogues at context pads) reads the scene's Z buffer
 * (Z24S8) as a LIN_R8G8B8A8 texture (format 0x41): on the NV2A both are
 * the same bytes. Our depth lives in an image the guest's memory never
 * sees, so the pass read stale bytes and the whole frame came out blurred.
 * A pass packs the depth into those bytes, as the NV2A's Z24S8 dword
 * (z << 8) | s (stencil 0), in the byte order of the format sampled
 * (nv2a_depth_texel_order: the blur mask reads 0x41, the composite 0x3F).
 * The first time an address is sampled its depth image is remade with
 * SAMPLED usage (zs_wanted), so that one frame reads nothing. */
static VkShaderModule compile(glslang_stage_t stage, const char *src);
static uint32_t s_desc_cb_gen;          /* defined with s_layout's state below */
static VkDescriptorSetLayout s_zc_dsl;
static VkPipelineLayout s_zc_layout;
static VkPipeline s_zc_pipe;
static int s_zc_failed;
static VkSurf s_zc;                     /* the packed copy (image, view, pw, ph) */
static VkImage s_zc_from;               /* the depth image and generation it holds */
static uint32_t s_zc_gen, s_zc_order;

static int zc_pipe(void)
{
    static const char vsrc[] =
        "#version 450\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fsrc[] =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D z;\n"
        "layout(push_constant) uniform P { uint order; } pc;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() {\n"
        "    uint v = uint(texelFetch(z, ivec2(gl_FragCoord.xy), 0).r * 16777215.0 + 0.5);\n"
        "    uvec4 sh = ((uvec4(pc.order) >> uvec4(0u, 2u, 4u, 6u)) & 3u) * 8u;\n"
        "    o = vec4((uvec4(v << 8) >> sh) & 255u) / 255.0;\n"
        "}\n";
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkShaderModule vs, fs;

    if (s_zc_pipe || s_zc_failed)
        return s_zc_pipe != VK_NULL_HANDLE;
    s_zc_failed = 1;
    {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_zc_dsl) != VK_SUCCESS)
            return 0;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_zc_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_zc_layout) != VK_SUCCESS)
            return 0;
    }
    vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (vs && fs) {
        memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&ba, 0, sizeof ba);
        ba.colorWriteMask = 0xF;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &fmt;
        ci.pNext = &rci;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = s_zc_layout;
        if (vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_zc_pipe) != VK_SUCCESS)
            s_zc_pipe = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
    if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
    if (!s_zc_pipe)
        LOGE("depth-as-colour pipeline failed\n");
    s_zc_failed = !s_zc_pipe;
    return s_zc_pipe != VK_NULL_HANDLE;
}

/* The packed view of d for texture format fmt (byte order:
 * nv2a_depth_texel_order), or VK_NULL_HANDLE (d not sampleable yet: it is
 * dropped and remade with SAMPLED usage at its next use). */
static VkImageView depth_as_color(VkDepthBuf *d, uint32_t fmt)
{
    uint32_t order = nv2a_depth_texel_order(fmt);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp = { 0, 0, (float)d->pw, (float)d->ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { d->pw, d->ph } };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;

    if (!d->sampleable) {
        int i;
        for (i = 0; i < 8 && s_zs_va[i] && s_zs_va[i] != d->va; i++) { }
        if (i < 8 && !s_zs_va[i]) {
            s_zs_va[i] = d->va;
            fprintf(stderr, "  [VK] Z buffer 0x%08X sampled as a texture: packed from the"
                    " next use on\n", d->va);
        }
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
        if (d->sview) garbage_add(VK_NULL_HANDLE, d->sview, VK_NULL_HANDLE);
        memset(d, 0, sizeof *d);
        return VK_NULL_HANDLE;
    }
    if (s_zc_from == d->image && s_zc_gen == d->wgen && s_zc_order == order && s_zc.view)
        return s_zc.view;
    if (!zc_pipe())
        return VK_NULL_HANDLE;
    if (!d->sview) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = d->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &d->sview) != VK_SUCCESS) {
            d->sview = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    if (s_zc.image && (s_zc.pw != d->pw || s_zc.ph != d->ph)) {
        garbage_add(s_zc.image, s_zc.view, s_zc.mem);
        memset(&s_zc, 0, sizeof s_zc);
    }
    if (!s_zc.image) {
        if (!make_image(d->pw, d->ph, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_zc.image, &s_zc.view, &s_zc.mem))
            return VK_NULL_HANDLE;
        s_zc.pw = d->pw;
        s_zc.ph = d->ph;
    }
    end_rendering();
    /* Depth writes before -> this read; earlier reads of the copy -> this write. */
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, all, all, 0, 1, &mb, 0, NULL, 0, NULL);
    ca.imageView = s_zc.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = d->pw;
    ri.renderArea.extent.height = d->ph;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_zc_pipe);
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = sampler_get(0);
    ii.imageView = d->sview;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_zc_layout, 0, 1, &wr);
    vkCmdPushConstants(s_cb, s_zc_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &order);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    /* The copy -> the draw that samples it; depth reads -> later depth writes. */
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, all, all, 0, 1, &mb, 0, NULL, 0, NULL);
    s_rt = NULL;
    s_rt_depth = NULL;
    s_desc_cb_gen = 0;                  /* s_layout's pushed set is disturbed */
    s_pipe_foreign = 1;                 /* the next draw rebinds its own */
    s_dyn_valid = 0;
    s_zc_from = d->image;
    s_zc_gen = d->wgen;
    s_zc_order = order;
    return s_zc.view;
}

/* Stage i: its image view (a surface or a cached texture), sampler and
 * texcoord scale. Records uploads, so call it outside rendering or accept
 * that it ends the pass. */
static void bind_stage(const uint32_t *regs, int i, float scale[2], VkDescriptorImageInfo *out,
                       VkSurf **sampled)
{
    /* Texture mode CUBE_MAP: the shader declares a samplerCube (gl_psh.c),
     * so the descriptor must be a cube view whatever is bound. */
    int cube = ((regs[0x1E70 / 4] >> (5 * i)) & 0x1F) == 3;
    /* RECOMP_VK_CUBE=0, or the title's option (nv2a_vk_cube_maps): cube
     * stages sample the black dummy (no car reflections, the old look). */
    static int cube_env = -1;
    if (cube_env < 0) {
        const char *e = getenv("RECOMP_VK_CUBE");
        cube_env = !(e && *e == '0');
    }
    int cube_on = cube_env && nv2a_vk_cube_maps;
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, addr, filter, key, levels = 1, minf;
    int dyn_cube = 0;
    VkSurf *rt;
    VkDepthBuf *zd;

    scale[0] = scale[1] = 1.0f;
    out->imageView = cube ? s_dummy_cube_view : s_dummy_view;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->sampler = sampler_get(0);
    *sampled = NULL;
    if (!(control0 & 0x40000000u) || !regs[base])
        return;
    va = s_batch->tex_va[i];
    if (tex_size_from_format(color)) {
        uint32_t most;
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
        /* SET_TEXTURE_FORMAT [19:16]: mip levels, down to 1x1 at most. */
        most = full_levels(w, h);
        levels = (format >> 16) & 0xF;
        if (!levels || !s_mips) levels = 1;
        if (levels > most) levels = most;
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return;
    s_palette_reg = regs[base + 8];
    s_palette_va = s_batch->pal_va[i];
    rt = cube ? NULL : surf_find(va);
    if (cube) {
        /* SET_TEXTURE_FORMAT bit 2: CUBEMAP_ENABLE; mip levels in [19:16]. */
        if (cube_on && (format & 4)) {
            uint32_t face = cube_face_bytes(color, w, h, (format >> 16) & 0xF), f;
            VkSurf *faces[6];
            int any = 0;
            for (f = 0; f < 6; f++)
                any |= (faces[f] = surf_find(va + f * face)) != NULL;
            if (any) {
                out->imageView = cube_from_surfaces(va, w, h, face, faces);
                levels = s_mips ? 2 : 1;            /* has a chain: see below */
                dyn_cube = 1;
            } else {
                uint32_t texs = s_hz_texs;
                uint64_t t0 = hz_now();
                out->imageView = tex_get(va, color, w, h, 0, face, levels);
                if (s_hz_texs != texs)
                    s_hz_tex_ns += hz_now() - t0;
            }
        }
    } else if (rt) {
        out->imageView = rt->view;
        *sampled = rt;
        rt_scale(w, h, rt->w, rt->h, rt->aa_sx, rt->aa_sy, scale);
    } else if ((zd = depth_at(va)) != NULL) {
        VkImageView v = depth_as_color(zd, color);
        int k;
        if (v) {
            out->imageView = v;
            for (k = 0; k < VK_MAX_SURF; k++)       /* its surface's logical size */
                if (s_surf[k].image && s_surf[k].pw == s_zc.pw && s_surf[k].ph == s_zc.ph) {
                    rt_scale(w, h, s_surf[k].w, s_surf[k].h, s_surf[k].aa_sx, s_surf[k].aa_sy,
                             scale);
                    break;
                }
        }
    } else {
        uint32_t texs = s_hz_texs;
        uint64_t t0 = hz_now();
        out->imageView = tex_get(va, color, w, h, pitch, 0, levels);
        if (s_hz_texs != texs)
            s_hz_tex_ns += hz_now() - t0;
    }
    addr = regs[base + 2];
    filter = regs[base + 5];
    key = (addr & 0xF0F);
    if ((addr & 0xF) == 4 || ((addr >> 8) & 0xF) == 4) {
        /* CLAMP_TO_BORDER: SET_TEXTURE_BORDER_COLOR (A8R8G8B8) as the nearest
         * fixed Vulkan border. Conker's projected shadow maps use white:
         * outside a map nothing is darkened. */
        uint32_t bc = regs[base + 9];
        if ((bc & 0x808080u) == 0x808080u) key |= 0x2000;      /* opaque white */
        else if (bc >> 24 >= 0x80) key |= 0x1000;              /* opaque black */
    }
    minf = (filter >> 16) & 0xFF;
    if (((filter >> 24) & 0xF) == 1) key |= 0x10000;
    if (minf == 1) key |= 0x20000;
    /* MIN 3..6: the *_MIPMAP_* filters. */
    if (levels > 1 && ((minf >= 3 && minf <= 6) || (dyn_cube && minf != 1))) {
        int an = nv2a_vk_aniso;
        key |= 0x40000;
        if (an > (int)s_aniso) an = (int)s_aniso;
        if (an > 1 && !(key & 0x30000))
            key |= (uint32_t)an << 20;          /* bits 20..24: anisotropy */
    }
    out->sampler = sampler_get(key);
}

/* ── Shaders and pipelines ────────────────────────────────────────── */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_layout;
static VkPipelineCache       s_pcache;

typedef struct {
    uint64_t   vkey;
    Nv2aPshKey pkey;
    VkShaderModule vs, fs;
    VkShaderModule fs_nat;      /* fs without the alpha test, made on first use */
    uint32_t   used;
    uint32_t   hash;
    int        next;
} VkProg;

#define VK_MAX_PROG 1024
#define PROG_BUCKETS 1024
static VkProg s_prog[VK_MAX_PROG];
static int s_prog_head[PROG_BUCKETS];
static uint32_t s_prog_made, s_prog_live;

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }

typedef struct {
    glslang_stage_t stage;
    const char *src;
    uint32_t *words;
    size_t nwords;
    int ok;
} CompileJob;

static void *compile_thread(void *arg)
{
    CompileJob *j = (CompileJob *)arg;
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = j->stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = j->src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        LOGE("%s shader failed:\n%s\n", j->stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
             glslang_shader_get_info_log(sh));
        if (s_trace)
            fprintf(stderr, "%s\n", j->src);
        glslang_shader_delete(sh);
        return NULL;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        LOGE("link failed: %s\n", glslang_program_get_info_log(pr));
    } else {
        glslang_program_SPIRV_generate(pr, j->stage);
        j->nwords = glslang_program_SPIRV_get_size(pr);
        j->words = (uint32_t *)malloc(j->nwords * 4);
        if (j->words) {
            glslang_program_SPIRV_get(pr, j->words);
            j->ok = 1;
        }
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return NULL;
}

/* ── Caches across runs ───────────────────────────────────────────
 *
 * A program compiled at first use stalled a race start for ~1 s (glslang,
 * then NIR and NAK inside NVK: ~50 ms each on the console). Three files, next
 * to the NRO (the current directory on Linux); RECOMP_PROG_CACHE=0 disables
 * them all, =<path> moves the program list:
 *   progcache.bin    what each program was built from -- the GL renderer's
 *                    records, shared (the format does not depend on the back end)
 *   vkspirv.bin      glslang's output by hash of the GLSL source
 *   vkpipes.bin      the pipeline variants seen (program + fixed state)
 *   vkpipecache.bin  the driver's VkPipelineCache (NAK binaries), saved while
 *                    running whenever pipelines were added
 * At start-up every recorded program and pipeline is built before the title
 * draws. */
static int s_prewarming;
static int s_cache_off = -1;

static const char *cache_dir(void)
{
    const char *e = getenv("RECOMP_CACHE_DIR");
    if (e && *e)
        return e;
#if defined(__SWITCH__)
    return "sdmc:/switch/fo2x/";
#else
    return "";
#endif
}

static int cache_path(char *out, size_t cap, const char *name)
{
    if (s_cache_off < 0) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        s_cache_off = e && e[0] == '0' && !e[1];
    }
    if (s_cache_off)
        return 0;
    if (!strcmp(name, "vkprog.bin")) {   /* GL keeps progcache.bin: its records carry a key bit VK has not */
        const char *e = getenv("RECOMP_PROG_CACHE");
        if (e && *e) {
            snprintf(out, cap, "%s", e);
            return 1;
        }
    }
    snprintf(out, cap, "%s%s", cache_dir(), name);
    return 1;
}

#define SPV_MAGIC 0x31565053u                   /* "SPV1" */
typedef struct { uint64_t hash; uint32_t stage, nwords; uint32_t *words; } SpvEnt;
static SpvEnt *s_spv;
static uint32_t s_nspv, s_spv_cap, s_spv_hits;

static uint64_t text_hash(const char *t, uint32_t stage)
{
    uint64_t h = 1469598103934665603ull ^ stage;
    while (*t)
        h = (h ^ (uint8_t)*t++) * 1099511628211ull;
    return h;
}

static void spv_add(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    if (s_nspv == s_spv_cap) {
        s_spv_cap = s_spv_cap ? s_spv_cap * 2 : 256;
        s_spv = (SpvEnt *)realloc(s_spv, s_spv_cap * sizeof *s_spv);
    }
    s_spv[s_nspv].hash = hash;
    s_spv[s_nspv].stage = stage;
    s_spv[s_nspv].nwords = n;
    s_spv[s_nspv].words = (uint32_t *)malloc((size_t)n * 4);
    memcpy(s_spv[s_nspv].words, w, (size_t)n * 4);
    s_nspv++;
}

static const SpvEnt *spv_find(uint64_t hash, uint32_t stage)
{
    uint32_t i;
    for (i = 0; i < s_nspv; i++)
        if (s_spv[i].hash == hash && s_spv[i].stage == stage)
            return &s_spv[i];
    return NULL;
}

static void spv_load(void)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "rb")))
        return;
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        uint64_t hash;
        uint32_t *w;
        if (hdr[0] != SPV_MAGIC || hdr[3] > (1u << 22))
            break;
        memcpy(&hash, &hdr[1], 8);
        w = (uint32_t *)malloc((size_t)hdr[3] * 8 + 8);
        if (!w || fread(w, 4, hdr[3], f) != hdr[3]) { free(w); break; }
        /* the stage is in the word after the header */
        spv_add(hash, w[0], w + 1, hdr[3] - 1);
        free(w);
    }
    fclose(f);
}

static void spv_append(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "ab")))
        return;
    hdr[0] = SPV_MAGIC;
    memcpy(&hdr[1], &hash, 8);
    hdr[3] = n + 1;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&stage, 4, 1, f);
    fwrite(w, 4, n, f);
    fclose(f);
}

/* GLSL to a shader module. glslang's parser recurses deeply: it runs on a
 * thread with a 4 MB stack (the executor's is smaller). */
static VkShaderModule compile(glslang_stage_t stage, const char *src)
{
    CompileJob j = { stage, src, NULL, 0, 0 };
    pthread_t t;
    pthread_attr_t at;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    uint64_t hash = text_hash(src, (uint32_t)stage);
    const SpvEnt *hit = spv_find(hash, (uint32_t)stage);

    {   /* RECOMP_VK_GLSL_DUMP=<dir>: every generated shader, <hash>.vs/.fs */
        const char *d = getenv("RECOMP_VK_GLSL_DUMP");
        if (d && *d) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/%016llx.%s", d, (unsigned long long)hash,
                     stage == GLSLANG_STAGE_VERTEX ? "vs" : "fs");
            if ((f = fopen(path, "w")) != NULL) {
                fputs(src, f);
                fclose(f);
            }
        }
    }

    if (hit) {
        s_spv_hits++;
        ci.codeSize = (size_t)hit->nwords * 4;
        ci.pCode = hit->words;
        vkCreateShaderModule(s_dev, &ci, NULL, &m);
        return m;
    }
    VT("compile %s (%u bytes of GLSL)\n", stage == GLSLANG_STAGE_VERTEX ? "vs" : "fs",
       (unsigned)strlen(src));
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, compile_thread, &j) == 0)
        pthread_join(t, NULL);
    else
        compile_thread(&j);
    pthread_attr_destroy(&at);
    if (!j.ok)
        return VK_NULL_HANDLE;
    ci.codeSize = j.nwords * 4;
    ci.pCode = j.words;
    VT("  SPIR-V %u words -> vkCreateShaderModule\n", (unsigned)j.nwords);
    vkCreateShaderModule(s_dev, &ci, NULL, &m);
    spv_add(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    spv_append(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    free(j.words);
    return m;
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;
    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)
            break;
    }
    return h ? h : 1;
}

static uint32_t prog_bucket(uint64_t vkey, const Nv2aPshKey *pk)
{
    const uint32_t *w = (const uint32_t *)pk;
    uint64_t h = vkey ^ 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *pk / 4; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (PROG_BUCKETS - 1);
}

static void pipes_forget_prog(int prog);

/* progcache.bin records: nv2a_gl.c's ProgRec, byte for byte. */
#define PROG_REC_MAGIC 0x4350564Eu              /* "NVPC" */
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;

/* Vertex attributes a program reads as packed CMP normals (type 6, fed as
 * R32_UINT and unpacked in the shader, prog_cmp_glsl). Part of the program's
 * key; stored in a record's xform word above bit 16. Programs only: fixed
 * function converts them (upload_vertices). */
static uint16_t s_prewarm_cmpm;

static uint16_t prog_cmp_mask(const Nv2aRawBatch *b)
{
    uint16_t m = 0;
    uint32_t a;
    if (s_prewarming)
        return s_prewarm_cmpm;
    if (b->xform != 2)
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a)) && b->direct[a].type == 6)
            m |= (uint16_t)(1u << a);
    return m;
}

/* The program text with each CMP input read as a uint and unpacked into the
 * vN the program uses: x, y 11-bit and z 10-bit signed, w 1 (as the
 * executor's own fetch_attr). */
static void prog_cmp_glsl(char *src, size_t cap, uint16_t m)
{
    static const char fn[] =
        "vec4 nv2a_cmp(uint u) {\n"
        "    return vec4(float(int(u << 21) >> 21) / 1023.0, float(int(u << 10) >> 21) / 1023.0,\n"
        "                float(int(u) >> 22) / 511.0, 1.0);\n"
        "}\n";
    char from[64], to[96], init[512];
    size_t ni = 0;
    uint32_t a;
    char *at;
    init[0] = 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        size_t lf, lt, len;
        if (!(m & (1u << a)))
            continue;
        snprintf(from, sizeof from, "layout(location = %u) in vec4 v%u;\n", a, a);
        snprintf(to, sizeof to, "layout(location = %u) in uint v%u_cmp;\nvec4 v%u;\n", a, a, a);
        if (!(at = strstr(src, from)))
            continue;
        lf = strlen(from); lt = strlen(to); len = strlen(src);
        if (len + lt - lf + 1 > cap)
            return;
        memmove(at + lt, at + lf, len - (size_t)(at - src) - lf + 1);
        memcpy(at, to, lt);
        ni += (size_t)snprintf(init + ni, sizeof init - ni, "    v%u = nv2a_cmp(v%u_cmp);\n", a, a);
    }
    if (!ni || !(at = strstr(src, "void main() {\n")))
        return;
    {
        size_t len = strlen(src), lfn = sizeof fn - 1, head = (size_t)(at - src);
        size_t lm = strlen("void main() {\n");
        if (len + lfn + ni + 1 > cap)
            return;
        /* the helper before main, the unpacking first thing in it */
        memmove(at + lfn + lm + ni, at + lm, len - head - lm + 1);
        memmove(at + lfn, at, lm);
        memcpy(at, fn, lfn);
        memcpy(at + lfn + lm, init, ni);
    }
}

static void prog_rec_fill(ProgRec *rec, const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    memset(rec, 0, sizeof *rec);
    rec->magic = PROG_REC_MAGIC;
    rec->xform = (b->xform == 2 ? 2u : 1u) | ((uint32_t)prog_cmp_mask(b) << 16);
    rec->pk = *pk;
    if (rec->xform == 2 && b->vp_program && b->vp_slots <= 136) {
        rec->vp_start = b->vp_start;
        rec->slots = b->vp_slots;
        memcpy(rec->prog, b->vp_program, (size_t)b->vp_slots * 16);
    }
}

static void prog_rec_append(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    char path[256];
    ProgRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "vkprog.bin"))
        return;
    prog_rec_fill(&rec, b, pk);
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

/* The batch a record describes (vertex program slots in rec). */
static void prog_rec_batch(const ProgRec *rec, Nv2aRawBatch *bb, uint32_t n)
{
    memset(bb, 0, sizeof *bb);
    bb->xform = rec->xform & 0xFFFFu;
    s_prewarm_cmpm = (uint16_t)(rec->xform >> 16);
    bb->vp_program = (const uint32_t (*)[4])rec->prog;
    bb->vp_slots = rec->slots ? rec->slots : 136;
    bb->vp_start = rec->vp_start;
    bb->vp_prog_gen = 0x80000000u + n;          /* hash it: not an executor version */
}

static VkProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    static uint32_t last_gen;
    static uint64_t last_vkey;
    static VkProg *last;
    static char vsrc[320 * 1024], fsrc[64 * 1024];
    uint64_t vkey = 0;
    uint32_t bucket;
    VkProg *p, *victim = NULL;
    int i;
    size_t n;

    uint16_t cmpm = prog_cmp_mask(b);

    if (b->xform == 2) {
        if (b->vp_prog_gen != last_gen || !last_vkey) {
            last_vkey = prog_hash(b);
            last_gen = b->vp_prog_gen;
        }
        vkey = last_vkey;
        if (cmpm) {
            vkey ^= (uint64_t)cmpm * 0x9E3779B97F4A7C15ull;
            if (!vkey) vkey = 1;
        }
    }
    if (last && last->vs && last->vkey == vkey && !memcmp(&last->pkey, pk, sizeof *pk)) {
        last->used = s_frame;
        return last;
    }
    bucket = prog_bucket(vkey, pk);
    for (i = s_prog_head[bucket]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk)) {
            c->used = s_frame;
            last = c;
            return c;
        }
    }
    for (i = 0; i < VK_MAX_PROG; i++) {
        VkProg *c = &s_prog[i];
        if (!c->vs) { if (!victim || victim->vs) victim = c; }
        else if (!victim || (victim->vs && c->used < victim->used)) victim = c;
    }
    p = victim;
    last = NULL;
    if (p->vs) {
        int *at = &s_prog_head[p->hash], me = (int)(p - s_prog) + 1;
        while (*at) {
            if (*at == me) { *at = p->next; break; }
            at = &s_prog[*at - 1].next;
        }
        pipes_forget_prog((int)(p - s_prog));
        vkDestroyShaderModule(s_dev, p->vs, NULL);
        vkDestroyShaderModule(s_dev, p->fs, NULL);
        if (p->fs_nat) vkDestroyShaderModule(s_dev, p->fs_nat, NULL);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);

    n = (size_t)snprintf(vsrc, sizeof vsrc, "%s", nv2a_gl_vsh_prelude());
    if (vkey) {
        int k = nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, vsrc + n, sizeof vsrc - n);
        if (k < 0)
            return NULL;
        n += (size_t)k;
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_main_program());
        if (cmpm)
            prog_cmp_glsl(vsrc, sizeof vsrc, cmpm);
    } else {
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_fixed());
    }
    if (nv2a_gl_psh(pk, fsrc, sizeof fsrc) < 0)
        return NULL;
    p->vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    p->fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (!p->vs || !p->fs) {
        if (p->vs) vkDestroyShaderModule(s_dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(s_dev, p->fs, NULL);
        memset(p, 0, sizeof *p);
        return NULL;
    }
    s_prog_live++;
    if ((++s_prog_made % 100) == 0)
        LOGE("%u shader programs compiled (%u live)\n", s_prog_made, s_prog_live);
    p->vkey = vkey;
    p->pkey = *pk;
    p->used = s_frame;
    p->hash = bucket;
    p->next = s_prog_head[bucket];
    s_prog_head[bucket] = (int)(p - s_prog) + 1;
    last = p;
    prog_rec_append(b, pk);
    return p;
}

/* nat: the alpha test is off (or ALWAYS), so the fragment shader is the
 * one without its discard -- early depth testing stays possible. */
typedef struct {
    int      prog;
    uint32_t blend, bsrc, bdst, beq, cmask, depth, topo, nat;
} PipeKey;

typedef struct {
    PipeKey key;
    VkPipeline pipe;
    uint32_t hash;
    int next;
} PipeEnt;

#define VK_MAX_PIPE 4096
#define PIPE_BUCKETS 4096
static PipeEnt s_pipes[VK_MAX_PIPE];
static int s_pipe_head[PIPE_BUCKETS];
static int s_npipes;

static void pipes_forget_prog(int prog)
{
    int i;
    /* Rare (1024 live programs): drop every pipeline using it. */
    for (i = 0; i < s_npipes; i++) {
        PipeEnt *e = &s_pipes[i];
        if (e->pipe && e->key.prog == prog) {
            int *at = &s_pipe_head[e->hash], me = i + 1;
            while (*at) {
                if (*at == me) { *at = e->next; break; }
                at = &s_pipes[*at - 1].next;
            }
            garbage_add_pipe(e->pipe);               /* may still be in flight */
            e->pipe = VK_NULL_HANDLE;
        }
    }
}

static VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

/* vkpipes.bin: the program by what generated it, and the pipeline's own
 * state. */
#define PIPE_REC_MAGIC 0x4C504B56u              /* "VKPL" */
typedef struct {
    uint32_t   magic, nat;                      /* nat: was zero padding */
    uint64_t   vkey;
    Nv2aPshKey pk;
    uint32_t   blend, bsrc, bdst, beq, cmask, depth, topo;
} PipeRec;
static uint32_t s_pipes_unsaved;

static void pipe_rec_append(const PipeKey *k)
{
    char path[256];
    PipeRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "vkpipes.bin"))
        return;
    memset(&rec, 0, sizeof rec);
    rec.magic = PIPE_REC_MAGIC;
    rec.nat = k->nat;
    rec.vkey = s_prog[k->prog].vkey;
    rec.pk = s_prog[k->prog].pkey;
    rec.blend = k->blend; rec.bsrc = k->bsrc; rec.bdst = k->bdst; rec.beq = k->beq;
    rec.cmask = k->cmask; rec.depth = k->depth; rec.topo = k->topo;
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

static VkPipeline pipe_get(const PipeKey *k)
{
    const uint32_t *w = (const uint32_t *)k;
    uint32_t h = 2166136261u, i;
    int e;
    PipeEnt *pe;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkProg *p = &s_prog[k->prog];

    for (i = 0; i < sizeof *k / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    h &= PIPE_BUCKETS - 1;
    for (e = s_pipe_head[h]; e; e = s_pipes[e - 1].next)
        if (s_pipes[e - 1].pipe && !memcmp(&s_pipes[e - 1].key, k, sizeof *k))
            return s_pipes[e - 1].pipe;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = p->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = p->fs;
    if (k->nat) {
        if (!p->fs_nat) {
            static char fsrc[64 * 1024];
            if (nv2a_gl_psh_no_alpha_test(&p->pkey, fsrc, sizeof fsrc) >= 0)
                p->fs_nat = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
        }
        if (p->fs_nat)
            st[1].module = p->fs_nat;
    }
    st[1].pName = "main";
    ia.topology = k->topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    rs.depthBiasEnable = VK_TRUE;       /* the values are dynamic: 0, 0 = off */
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(k->bsrc);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(k->bdst);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(k->beq);
    ba.colorWriteMask = k->cmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (k->depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_layout;
    VT("vkCreateGraphicsPipelines prog %d blend %u cmask %X depth %u topo %u\n", k->prog, k->blend,
       k->cmask, k->depth, k->topo);
    {
        uint64_t t0 = hz_now();
        VkResult r = vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe);
        s_hz_pipes++;                     /* itemised in the [hitch] line */
        s_hz_pipe_ns += hz_now() - t0;
        if (r != VK_SUCCESS) {
            LOGE("pipeline creation failed\n");
            return VK_NULL_HANDLE;
        }
    }
    if (s_npipes == VK_MAX_PIPE) {
        LOGE("pipeline table full\n");
        return pipe;                          /* leaks; never seen in practice */
    }
    pe = &s_pipes[s_npipes];
    pe->key = *k;
    pe->pipe = pipe;
    pe->hash = h;
    pe->next = s_pipe_head[h];
    s_pipe_head[h] = ++s_npipes;
    s_pipes_unsaved++;
    pipe_rec_append(k);
    return pipe;
}

static VkProg *prog_find(uint64_t vkey, const Nv2aPshKey *pk)
{
    int i;
    for (i = s_prog_head[prog_bucket(vkey, pk)]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk))
            return c;
    }
    return NULL;
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *d = NULL;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
            *size = (size_t)n;
        else { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

/* The file write runs on a thread of its own: on the Switch the cache grows
 * to several MB and its write to the SD card held the recording thread (and
 * so the frame) for 450-650 ms every 20 s (issue 17, items 4 and 5). The
 * .tmp + rename keeps the file whole if the process ends mid-write. */
struct pcache_job { char path[256]; void *d; size_t n; };
static volatile int s_pcache_writing;

static void pcache_write(struct pcache_job *j)
{
    char tmp[272];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    if ((f = fopen(tmp, "wb"))) {
        int ok = fwrite(j->d, 1, j->n, f) == j->n;
        fclose(f);
        if (ok) {
            remove(j->path);               /* FAT: rename does not replace */
            rename(tmp, j->path);
        }
    }
    free(j->d);
    free(j);
}

static void *pcache_write_thread(void *arg)
{
    pcache_write(arg);
    s_pcache_writing = 0;
    return NULL;
}

/* sync: write before returning (at exit); else hand the bytes to a thread. */
static void pcache_save(int sync)
{
    size_t n = 0;
    struct pcache_job *j;
    if (!s_pcache || s_pcache_writing)     /* a write in flight: try again next time */
        return;
    j = calloc(1, sizeof *j);
    if (!j || !cache_path(j->path, sizeof j->path, "vkpipecache.bin")) { free(j); return; }
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n) { free(j); return; }
    j->d = malloc(n);
    j->n = n;
    if (!j->d || vkGetPipelineCacheData(s_dev, s_pcache, &n, j->d) != VK_SUCCESS) {
        free(j->d); free(j); return;
    }
    s_pipes_unsaved = 0;
    if (!sync) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        s_pcache_writing = 1;
        if (pthread_create(&t, &at, pcache_write_thread, j) == 0) {
            pthread_attr_destroy(&at);
            return;
        }
        pthread_attr_destroy(&at);
        s_pcache_writing = 0;
    }
    pcache_write(j);
}

static void prewarm(void)
{
    char path[256];
    FILE *f;
    uint64_t t0 = hz_now();
    uint32_t nprog = 0, npipe = 0, bad = 0;
    ProgRec rec;
    PipeRec pr;

    spv_load();
    s_prewarming = 1;
    if (cache_path(path, sizeof path, "vkprog.bin") && (f = fopen(path, "rb"))) {
        while (fread(&rec, sizeof rec, 1, f) == 1) {
            Nv2aRawBatch bb;
            if (rec.magic != PROG_REC_MAGIC || rec.slots > 136) { bad++; break; }
            prog_rec_batch(&rec, &bb, nprog);
            if (prog_get(&bb, &rec.pk))
                nprog++;
        }
        fclose(f);
    }
    if (cache_path(path, sizeof path, "vkpipes.bin") && (f = fopen(path, "rb"))) {
        while (fread(&pr, sizeof pr, 1, f) == 1) {
            VkProg *p;
            PipeKey k;
            if (pr.magic != PIPE_REC_MAGIC) { bad++; break; }
            if (!(p = prog_find(pr.vkey, &pr.pk)))
                continue;
            memset(&k, 0, sizeof k);
            k.prog = (int)(p - s_prog);
            k.blend = pr.blend; k.bsrc = pr.bsrc; k.bdst = pr.bdst; k.beq = pr.beq;
            k.cmask = pr.cmask; k.depth = pr.depth; k.topo = pr.topo;
            k.nat = pr.nat ? 1 : 0;
            if (pipe_get(&k))
                npipe++;
        }
        fclose(f);
    }
    s_prewarming = 0;
    fprintf(stderr, "  [VK] prewarm: %u programs (%u SPIR-V from cache), %u pipelines in %.0f ms%s\n",
            nprog, s_spv_hits, npipe, (hz_now() - t0) / 1e6, bad ? " (a file ends in a bad record)" : "");
    if (s_pipes_unsaved)
        pcache_save(1);
}

/* ── Context ──────────────────────────────────────────────────────── */

/* ── Frame pacing (RECOMP_VK_PACE, Switch) ────────────────────────────
 *
 * Presenting immediately shows each frame when it is ready: a title running
 * at 30 shows frames for one, two or three vblanks in turn (judder), worse
 * with the uncapped flip interval. Paced, the swapchain is FIFO and the
 * display's swap interval follows how often the title flips: over the last
 * eight flips, more than five two vblanks or more apart -> 2 (an even 30),
 * two or fewer -> 1, in between it stays. Interval 2 only with the title's
 * 30 fps cap (NFSU2_FPS_CAP=30): uncapped, a race at 30-50 fps chose 2,
 * and at 2 no flip comes sooner than two vblanks, so it never went back --
 * locked at 30. On unless RECOMP_VK_PACE=0; never headless. */
static int s_pace = -1;
static uint32_t s_pace_interval;

static int pace_on(void)
{
    if (s_pace < 0) {
#if defined(__SWITCH__)
        const char *e = getenv("RECOMP_VK_PACE");
        s_pace = !(e && *e == '0') && !s_headless;
#else
        s_pace = 0;
#endif
    }
    return s_pace;
}

static void pace_flip(void)
{
#if defined(__SWITCH__)
    extern uint32_t xbox_vblank_count(void);
    static uint32_t last_vb, hist[8], nhist;
    static int told;
    uint32_t vb = xbox_vblank_count(), d = vb - last_vb, slow = 0, want, i;
    static int cap30 = -1;

    if (cap30 < 0) {
        const char *e = getenv("NFSU2_FPS_CAP");
        cap30 = e && atoi(e) == 30;
    }

    last_vb = vb;
    if (!pace_on() || d < 1 || d > 8)
        return;
    hist[nhist++ & 7] = d;
    if (nhist < 8)
        return;
    for (i = 0; i < 8; i++)
        slow += hist[i] > 1;
    if (slow > 5 && cap30)
        want = 2;
    else if (slow <= 2)
        want = 1;
    else
        want = s_pace_interval ? s_pace_interval : 1;
    if (want == s_pace_interval)
        return;
    if (R_SUCCEEDED(nwindowSetSwapInterval(nwindowGetDefault(), want))) {
        if (told++ < 6)
            LOGE("swap interval %u (title flips every %s vblank)\n", want,
                 want == 2 ? "second" : "");
        s_pace_interval = want;
    }
#endif
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nf = 16, nm = 8, i;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pd, s_surface, &caps) != VK_SUCCESS)
        return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_pd, s_surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_pd, s_surface, &nm, modes);
    s_sc_format = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_sc_format = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    /* The game paces itself (vblank emulation): presenting must not block
     * it. Horizon's immediate mode does not tear (nvnflinger composes). */
    for (i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    for (i = 0; i < nm && mode == VK_PRESENT_MODE_FIFO_KHR; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    if (pace_on()) {
        mode = VK_PRESENT_MODE_FIFO_KHR;   /* the swap interval paces it */
        s_pace_interval = 0;               /* a new swapchain: set it again */
    }
    s_sc_extent = caps.currentExtent;
    if (s_sc_extent.width == 0xFFFFFFFFu) {
        s_sc_extent.width = 1280;
        s_sc_extent.height = 720;
    }
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s_sc_format;
    ci.imageExtent = s_sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    /* Android reports the panel's rotation (a phone held landscape is a
     * portrait panel turned 90 degrees). The picture is drawn upright, so
     * the compositor rotates it: identity. currentExtent is already the
     * window's upright size -- swapping it is only for pre-rotated
     * rendering (preTransform = currentTransform), and doing it here made a
     * portrait swapchain the compositor squashed into the landscape window. */
    if ((caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                                  VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
            && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    s_sc_unrotated = ci.preTransform != caps.currentTransform;
    /* Android surfaces often offer only INHERIT. */
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s_swapchain;
    {
        VkSwapchainKHR sc;
        if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &sc) != VK_SUCCESS)
            return 0;
        if (s_swapchain)
            vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
        s_swapchain = sc;
    }
    for (i = 0; i < 8; i++)
        if (s_sc_views[i]) {
            vkDestroyImageView(s_dev, s_sc_views[i], NULL);
            s_sc_views[i] = VK_NULL_HANDLE;
        }
    s_sc_count = 8;
    vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    for (i = 0; i < s_sc_count; i++) {
        if (!s_sc_rendered[i]) {
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(s_dev, &sci, NULL, &s_sc_rendered[i]) != VK_SUCCESS)
                s_sc_rendered[i] = VK_NULL_HANDLE;
        }
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s_sc_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_sc_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &s_sc_views[i]) != VK_SUCCESS)
            s_sc_views[i] = VK_NULL_HANDLE;     /* that image falls back to the blit */
    }
    fprintf(stderr, "  [VK] swapchain %ux%u, %u images, format %d, present mode %d\n",
            s_sc_extent.width, s_sc_extent.height, s_sc_count, s_sc_format, mode);
    return 1;
}

#if defined(__ANDROID__)
/* Android: the system driver (libvulkan.so), or a custom one picked in the
 * launcher (Mesa Turnip for Adreno) that libadrenotools loads:
 *   RECOMP_VK_DRIVER_DIR  its folder in the app's own storage, ending in '/'
 *   RECOMP_VK_DRIVER_LIB  its .so in there (meta.json's libraryName)
 *   RECOMP_VK_HOOK_DIR    the app's nativeLibraryDir (libmain_hook.so)
 * A driver that does not load falls back to the system one. */
static int vk_load(void)
{
    const char *dir = getenv("RECOMP_VK_DRIVER_DIR"), *lib = getenv("RECOMP_VK_DRIVER_LIB");
    const char *hooks = getenv("RECOMP_VK_HOOK_DIR");

    if (dir && *dir && lib && *lib && hooks && *hooks) {
        void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks,
                                             dir, lib, NULL, NULL);
        void *gipa = h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
        if (gipa) {
            volkInitializeCustom((PFN_vkGetInstanceProcAddr)gipa);
            LOGE("custom driver: %s%s\n", dir, lib);
            return 1;
        }
        {
            const char *why = dlerror();
            LOGE("custom driver %s%s did not load (%s): the system driver instead\n",
                 dir, lib, why ? why : "no vkGetInstanceProcAddr");
        }
    }
    return volkInitialize() == VK_SUCCESS;
}
#endif

#if !defined(__SWITCH__)
/* The window's surface. Android makes its own: SDL's would go through the
 * system driver, not the one vk_load picked. */
static int make_surface(void)
{
#if defined(__ANDROID__)
    VkAndroidSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    SDL_SysWMinfo wi;
    SDL_VERSION(&wi.version);
    if (!SDL_GetWindowWMInfo(s_win, &wi) || !wi.info.android.window)
        return 0;
    sci.window = wi.info.android.window;
    return vkCreateAndroidSurfaceKHR(s_inst, &sci, NULL, &s_surface) == VK_SUCCESS;
#else
    return SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surface);
#endif
}
#endif

static int ready(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[8];
    uint32_t niext = 0, n, i;
    VkPhysicalDevice pds[8];
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[8];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceFeatures have;
    const char *dext[4];
    uint32_t ndext = 0;
    const char *layers[1];
    uint32_t nlayers = 0;

    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    {
        const char *e = getenv("RECOMP_VK_TRACE");
        s_vtrace = e ? atoi(e) : 0;
    }
    nv2a_shader_vk = 1;
    glslang_initialize_process();

    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(__SWITCH__)
    iext[niext++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
    {
        const char *e = getenv("RECOMP_VK_HEADLESS");
        const char *drv = getenv("SDL_VIDEODRIVER");
        s_headless = (e && *e == '1') || (drv && !strcmp(drv, "offscreen"));
        if (!s_headless) {
            /* Pads with the window: SDL finds game controllers on the thread
             * that pumps events (Windows lists none otherwise). */
            if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
                s_headless = 1;
            } else {
                s_win = SDL_CreateWindow("NV2A (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                if (!s_win) {
                    s_headless = 1;
                } else {
                    unsigned int ne = 0;
                    const char *names[8];
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, NULL);
                    if (ne > 6) ne = 6;
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, names);
                    niext = 0;
                    for (i = 0; i < ne; i++)
                        iext[niext++] = names[i];
                }
            }
        }
        if (s_headless)
            niext = 0;
    }
    {
        const char *v = getenv("RECOMP_VK_VALIDATION");
        if (v && *v == '1')
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    }
#endif
#if defined(__ANDROID__)
    if (!vk_load()) {
        LOGE("no Vulkan driver\n");
        return 0;
    }
#endif
    ai.pApplicationName = "nfsu2x";
    ai.pEngineName = "xboxrecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    ici.enabledLayerCount = nlayers;
    ici.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed\n");
        return 0;
    }
#if defined(__ANDROID__)
    volkLoadInstance(s_inst);
#endif
    n = 8;
    if (vkEnumeratePhysicalDevices(s_inst, &n, pds) != VK_SUCCESS || !n) {
        LOGE("no Vulkan device\n");
        return 0;
    }
    s_pd = pds[0];
    vkGetPhysicalDeviceProperties(s_pd, &pp);
    fprintf(stderr, "  [VK] %s, Vulkan %u.%u.%u\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    s_ubo_align = pp.limits.minUniformBufferOffsetAlignment;
    s_ubo_range = pp.limits.maxUniformBufferRange;
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        /* Unset: the port's option (nv2a_vk_scale_pct, set before the
         * renderer starts), so the first frames have it already -- this
         * line said "1 x 1" and 1.5 came only at the first flip. */
        double k = e ? strtod(e, NULL)
                 : nv2a_vk_scale_pct >= 50 ? nv2a_vk_scale_pct / 100.0 : 1.0;
        uint32_t m = pp.limits.maxImageDimension2D;
        if (pp.limits.maxFramebufferWidth < m) m = pp.limits.maxFramebufferWidth;
        if (pp.limits.maxFramebufferHeight < m) m = pp.limits.maxFramebufferHeight;
        s_max_size = m ? m : 4096;
        if (!(k > 0.0)) k = 1.0;                    /* unset, 0 or garbage */
        s_scale = k < 0.5 ? 0.5 : k > 4.0 ? 4.0 : k;
        e = getenv("RECOMP_VK_SQUARE");
        s_square_env = !(e && *e == '0');      /* allowed; widescreen decides per flip */
        e = getenv("RECOMP_GL_SCALE_X");
        k = e ? strtod(e, NULL) : 0.0;
        if (k > 0.0)
            s_square_env = 0;
        s_square = s_square_env && xbox_video_widescreen() && nv2a_vk_square;
        s_wide = xbox_wide_ratio;
        if (k > 0.0)
            s_xratio = (k < 0.5 ? 0.5 : k > 4.0 * 4.0 / 3.0 ? 4.0 * 4.0 / 3.0 : k) / s_scale;
        s_scale_x = s_scale * s_xratio;
        if (s_scale != 1.0 || s_scale_x != 1.0 || s_square)
            fprintf(stderr, "  [VK] rendering at %g x %g (RECOMP_GL_SCALE[_X] or options)%s, surfaces up to %u\n",
                    s_scale_x, s_scale, s_square ? ", square pixels" : "", s_max_size);
    }
    if (s_ubo_align < 16) s_ubo_align = 16;
    vkGetPhysicalDeviceMemoryProperties(s_pd, &s_memprops);
    vkGetPhysicalDeviceFeatures(s_pd, &have);
    s_have_bc = have.textureCompressionBC;
    s_have_depth_clamp = have.depthClamp;
    {
        const char *e = getenv("RECOMP_VK_MIPS");
        s_mips = !(e && *e == '0');
        e = getenv("RECOMP_VK_ANISO");
        if (e)
            s_aniso = (float)atof(e);
        e = getenv("RECOMP_VK_LOD_BIAS");
        if (e)
            s_lod_bias = (float)atof(e);
        if (s_lod_bias < -pp.limits.maxSamplerLodBias) s_lod_bias = -pp.limits.maxSamplerLodBias;
        if (s_lod_bias > pp.limits.maxSamplerLodBias) s_lod_bias = pp.limits.maxSamplerLodBias;
        if (!have.samplerAnisotropy || !s_mips || !(s_aniso > 1.0f))
            s_aniso = 1.0f;
        if (s_aniso > pp.limits.maxSamplerAnisotropy)
            s_aniso = pp.limits.maxSamplerAnisotropy;
    }
    nv2a_shader_depth_clamp = s_have_depth_clamp;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            s_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        {
            const char *e = getenv("RECOMP_GL_DXT");
            if (e && *e == '0')
                s_have_bc = 0;
        }
    }

    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &n, qf);
    for (s_qfam = 0; s_qfam < n; s_qfam++)
        if (qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (s_qfam == n) {
        LOGE("no graphics queue\n");
        return 0;
    }

#if defined(__SWITCH__)
    {
        NWindow *win = nwindowGetDefault();
        VkViSurfaceCreateInfoNN sci = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
        nwindowSetDimensions(win, 1280, 720);
        sci.window = win;
        if (vkCreateViSurfaceNN(s_inst, &sci, NULL, &s_surface) != VK_SUCCESS) {
            LOGE("vkCreateViSurfaceNN failed\n");
            return 0;
        }
    }
#else
    if (!s_headless && !make_surface()) {
        LOGE("no window surface: %s\n", SDL_GetError());
        s_headless = 1;
    }
#endif

    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    f13.dynamicRendering = VK_TRUE;
    fvi.vertexInputDynamicState = VK_TRUE;
    feds.extendedDynamicState = VK_TRUE;
    f13.pNext = &fvi;
    f2.pNext = &f13;
    f2.features.textureCompressionBC = s_have_bc ? VK_TRUE : VK_FALSE;
    f2.features.depthClamp = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = s_aniso > 1.0f ? VK_TRUE : VK_FALSE;
    if (!s_headless)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    (void)feds;
    {
        /* Name what is missing: on a phone this is the likely failure. */
        VkExtensionProperties *ep;
        uint32_t ne = 0, k, j;
        if (VK_API_VERSION_MINOR(pp.apiVersion) < 3 && VK_API_VERSION_MAJOR(pp.apiVersion) == 1)
            LOGE("the driver has Vulkan 1.%u; 1.3 is needed\n", VK_API_VERSION_MINOR(pp.apiVersion));
        vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, NULL);
        ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
        if (ep && vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ep) == VK_SUCCESS)
            for (k = 0; k < ndext; k++) {
                for (j = 0; j < ne && strcmp(ep[j].extensionName, dext[k]); j++)
                    ;
                if (j == ne)
                    LOGE("device extension %s missing\n", dext[k]);
            }
        free(ep);
    }
    if (vkCreateDevice(s_pd, &dci, NULL, &s_dev) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed (needs push descriptors, dynamic vertex input, Vulkan 1.3)\n");
        return 0;
    }
#if defined(__ANDROID__)
    volkLoadDevice(s_dev);
#endif
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
#define GET(p, name) p = (void *)vkGetDeviceProcAddr(s_dev, name); if (!p) { LOGE("missing %s\n", name); return 0; }
    GET(p_push_desc, "vkCmdPushDescriptorSetKHR");
    GET(p_vertex_input, "vkCmdSetVertexInputEXT");
    GET(p_begin_rendering, "vkCmdBeginRendering");
    GET(p_end_rendering, "vkCmdEndRendering");
    GET(p_cull_mode, "vkCmdSetCullMode");
    GET(p_front_face, "vkCmdSetFrontFace");
    GET(p_topology, "vkCmdSetPrimitiveTopology");
    GET(p_depth_test, "vkCmdSetDepthTestEnable");
    GET(p_depth_write, "vkCmdSetDepthWriteEnable");
    GET(p_depth_op, "vkCmdSetDepthCompareOp");
    GET(p_stencil_test, "vkCmdSetStencilTestEnable");
    GET(p_stencil_op, "vkCmdSetStencilOp");
#undef GET

    if (!s_headless && !create_swapchain()) {
        LOGE("swapchain creation failed; running headless\n");
        s_headless = 1;
    }

    {
        VkDescriptorSetLayoutBinding b[7];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        memset(b, 0, sizeof b);
        for (i = 0; i < 7; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].descriptorType = i < 2 || i == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = i == 0 || i == 6 ? VK_SHADER_STAGE_VERTEX_BIT
                                               : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 7;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS)
            return 0;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_dsl;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_layout) != VK_SUCCESS)
            return 0;
        {
            char path[256];
            size_t n = 0;
            void *d = cache_path(path, sizeof path, "vkpipecache.bin") ? read_file(path, &n) : NULL;
            pci.initialDataSize = n;
            pci.pInitialData = d;
            if (vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache) != VK_SUCCESS) {
                pci.initialDataSize = 0;       /* another driver's or a damaged file */
                pci.pInitialData = NULL;
                vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache);
            }
            if (n)
                fprintf(stderr, "  [VK] pipeline cache: %u KB from %s\n", (unsigned)(n >> 10), path);
            free(d);
        }
    }

    for (i = 0; i < VK_FRAMES; i++) {
        Frame *f = &s_fr[i];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements rq;
        cpi.queueFamilyIndex = s_qfam;
        vkCreateCommandPool(s_dev, &cpi, NULL, &f->pool);
        cai.commandPool = f->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = VK_CBS;
        vkAllocateCommandBuffers(s_dev, &cai, f->cbs);
        vkCreateFence(s_dev, &fci, NULL, &f->fence);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->acquired);
        bci.size = RING_BYTES;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &f->ring) != VK_SUCCESS)
            return 0;
        vkGetBufferMemoryRequirements(s_dev, f->ring, &rq);
        f->ring_mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!f->ring_mem || vkBindBufferMemory(s_dev, f->ring, f->ring_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s_dev, f->ring_mem, 0, RING_BYTES, 0, (void **)&f->ring_ptr) != VK_SUCCESS) {
            LOGE("frame ring allocation failed\n");
            return 0;
        }
    }
    s_fi = 0;
    frame_begin();
    if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_img, &s_dummy_view, &s_dummy_mem))
        return 0;
    clear_image_color(s_dummy_img, 0, 0, 0, 0);
    if (!make_image_n(1, 1, 6, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_cube_img, &s_dummy_cube_view,
                      &s_dummy_cube_mem))
        return 0;
    clear_image_color(s_dummy_cube_img, 0, 0, 0, 0);
    s_state = 1;
    fprintf(stderr, "  [VK] ready%s: BC %s, depth %s, depth clamp %s, UBO align %u,"
            " mips %s (bias %g), aniso %gx, FXAA %s\n",
            s_headless ? " (headless)" : "", s_have_bc ? "yes" : "no",
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            s_have_depth_clamp ? "yes" : "no", (unsigned)s_ubo_align,
            s_mips ? "yes" : "no", s_lod_bias, s_aniso, fxaa_on() ? "on" : "off");
    prewarm();
    return 1;
}

/* Submit what is recorded and wait for it, then keep recording in the same
 * frame (ring full). */
static void present_drain(void);
static void flush_frame_wait(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    present_drain();
    end_rendering();
    s_rt = NULL;
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    vkQueueSubmit(s_queue, 1, &si, s_f->fence);
    vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s_dev, 1, &s_f->fence);
    garbage_free(s_f);
    s_f->ring_off = 0;
    s_ring_gen++;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s_f->ncb = 0;
    s_cb = s_f->cbs[0];
    vkBeginCommandBuffer(s_cb, &bi);
    s_cb_gen++;
    s_dyn_valid = 0;
    s_transfer_pending = 1;
}

/* ── Early submits (RECOMP_VK_KICK, nfsu1-sw) ─────────────────────────
 *
 * A frame used to be one command buffer, submitted at the flip: the GPU sat
 * idle while the frame was recorded, then the next frame's recording waited
 * on its fence. Now the recorded part goes to the queue every N draws
 * (default 300; 0 = off) -- at the next change of render target, or past
 * 2N draws in the middle of a pass -- without a fence, and recording goes on
 * in the frame's next command buffer. The flip's submit carries the fence,
 * which covers everything submitted before it. */
static int s_kick_n = -1;

static int kick_due(int mid_pass)
{
    if (s_kick_n < 0) {
        const char *e = getenv("RECOMP_VK_KICK");
        s_kick_n = e ? atoi(e) : 300;
        if (s_kick_n < 0) s_kick_n = 0;
        fprintf(stderr, "  [VK] early submits every %d draws%s\n", s_kick_n,
                s_kick_n ? "" : " (off)");
    }
    if (!s_kick_n || s_f->ncb + 1 >= VK_CBS)
        return 0;
    return s_kick_draws >= (uint32_t)s_kick_n * (mid_pass ? 2u : 1u);
}

static void kick_submit(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE);
    s_cb = s_f->cbs[++s_f->ncb];
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cb, &bi);
    s_cb_gen++;
    s_dyn_valid = 0;
    s_kick_draws = 0;
    s_st_kicks++;
    s_transfer_pending = 1;
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }
static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static VkCompareOp cmp_op(uint32_t v, uint32_t dflt)
{
    if (!v) v = dflt;
    if (v < 0x200 || v > 0x207) v = dflt;
    return (VkCompareOp)(v - 0x200);
}

static VkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;         /* KEEP, and 0 (never set) */
    }
}

/* Dynamic state for a draw, set only when it changed. */
typedef struct {
    uint32_t cull, cullmode, front, topo;
    uint32_t dtest, dwrite, dop;
    uint32_t stest, sfail, szfail, spass, sop, smask, swrite, sref;
    uint32_t bcolor;
    float bias, slope;                  /* polygon offset (0 when off) */
} DynState;
static DynState s_dyn;

static void apply_dynamic(const uint32_t *r, int has_depth, VkPrimitiveTopology topo)
{
    DynState d;
    memset(&d, 0, sizeof d);
    d.cull = r[0x308 / 4] != 0;
    d.cullmode = r[0x39C / 4];
    d.front = r[0x3A0 / 4];
    d.topo = (uint32_t)topo;
    d.dtest = has_depth && r[0x30C / 4];
    d.dwrite = has_depth && r[0x35C / 4];
    d.dop = r[0x354 / 4];
    d.stest = has_depth && r[0x32C / 4];
    if (d.stest) {
        d.sfail = r[0x370 / 4]; d.szfail = r[0x374 / 4]; d.spass = r[0x378 / 4];
        d.sop = r[0x364 / 4]; d.smask = r[0x36C / 4] & 0xFF; d.swrite = r[0x360 / 4] & 0xFF;
        d.sref = r[0x368 / 4] & 0xFF;
    }
    d.bcolor = r[0x34C / 4];
    /* SET_POLY_OFFSET_FILL_ENABLE (0x338), SET_POLYGON_OFFSET_SCALE_FACTOR
     * (0x384) and _BIAS (0x388), floats as glPolygonOffset takes them (xemu
     * does the same). The title's decals -- footprints in mud, scorch
     * marks -- are coplanar with the ground and rely on it; without it they
     * z-fought (flickered). One unit is one step of the depth buffer: a Z16
     * surface's step is 256 of our D24's. */
    if (has_depth && r[0x338 / 4]) {
        memcpy(&d.slope, &r[0x384 / 4], 4);
        memcpy(&d.bias, &r[0x388 / 4], 4);
        if (((r[0x208 / 4] >> 4) & 0xF) == 1)
            d.bias *= 256.0f;
        if (!isfinite(d.bias)) d.bias = 0.0f;
        if (!isfinite(d.slope)) d.slope = 0.0f;
        {
            static int told;
            if (told < 3 && (d.bias != s_dyn.bias || d.slope != s_dyn.slope)) {
                told++;
                LOGE("polygon offset: factor %g, units %g (surface format 0x%X)\n",
                     d.slope, d.bias, r[0x208 / 4]);
            }
        }
    }
    if (s_dyn_valid && !memcmp(&d, &s_dyn, sizeof d))
        return;
    if (!s_dyn_valid || d.cull != s_dyn.cull || d.cullmode != s_dyn.cullmode) {
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        if (d.cull)
            cm = d.cullmode == 0x404 ? VK_CULL_MODE_FRONT_BIT
               : d.cullmode == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        p_cull_mode(s_cb, cm);
    }
    if (!s_dyn_valid || d.front != s_dyn.front)
        /* NV2A winding is as seen on screen, rows top-first -- Vulkan's
         * framebuffer orientation, so CW is CW. nv2a_gl swaps because its
         * window y runs the other way. Swapped here too, every culled draw
         * (29 a frame in the main menu: headlight glass, grille) lost its
         * front faces and kept its back ones. */
        p_front_face(s_cb, d.front == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    if (!s_dyn_valid || d.topo != s_dyn.topo)
        p_topology(s_cb, topo);
    if (!s_dyn_valid || d.dtest != s_dyn.dtest) p_depth_test(s_cb, d.dtest);
    if (!s_dyn_valid || d.dwrite != s_dyn.dwrite) p_depth_write(s_cb, d.dwrite);
    if (!s_dyn_valid || d.dop != s_dyn.dop) p_depth_op(s_cb, cmp_op(d.dop, 0x203));
    if (!s_dyn_valid || d.stest != s_dyn.stest) p_stencil_test(s_cb, d.stest);
    if (!s_dyn_valid || d.bias != s_dyn.bias || d.slope != s_dyn.slope)
        vkCmdSetDepthBias(s_cb, d.bias, 0.0f, d.slope);
    if (!s_dyn_valid || memcmp(&d.sfail, &s_dyn.sfail, 7 * sizeof(uint32_t))) {
        p_stencil_op(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(d.sfail), stencil_op(d.spass),
                     stencil_op(d.szfail), cmp_op(d.sop, 0x207));
        vkCmdSetStencilCompareMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.smask);
        vkCmdSetStencilWriteMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.swrite);
        vkCmdSetStencilReference(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.sref);
    }
    if (!s_dyn_valid || d.bcolor != s_dyn.bcolor) {
        float bc[4];
        argb_vec4(d.bcolor, bc);
        vkCmdSetBlendConstants(s_cb, bc);
    }
    s_dyn = d;
    s_dyn_valid = 1;
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
}

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* NV097 primitive -> Vulkan topology and (possibly rewritten) indices. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static int prim_indices(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n,
                        VkPrimitiveTopology *topo, uint32_t *cls)
{
    uint32_t i, m = 0;
    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return 1;
    case 2: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return 1;
    case 4: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return 1;
    case 5: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return 1;
    case 6: case 9: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return 1;
    case 7: case 10: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; *cls = 2; return 1;
    case 3:                                      /* LINE_LOOP: strip + first */
    case 8: {                                    /* QUADS -> triangles */
        size_t need = b->prim == 3 ? (size_t)b->index_count + 1 : (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return 0; }
        if (b->prim == 3) {
            if (!b->index_count) { *n = 0; return 0; }
            memcpy(s_prim_idx, b->indices, (size_t)b->index_count * 4);
            s_prim_idx[b->index_count] = b->indices[0];
            m = b->index_count + 1;
            *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1;
        } else {
            for (i = 0; i + 3 < b->index_count; i += 4) {
                const uint32_t *q = b->indices + i;
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
            }
            *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2;
        }
        *idx = s_prim_idx;
        *n = m;
        return 1;
    }
    default:
        *n = 0;
        return 0;
    }
}

/* ── Vertices ─────────────────────────────────────────────────────── */

typedef struct { const uint8_t *lo, *hi; VkDeviceSize at; } VRun;

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;
    case 2: return 4 * size;
    case 4: return size;
    case 6: return 4;
    default: return 2 * size;
    }
}

static uint32_t vertex_runs(const Nv2aRawBatch *b, uint16_t direct, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + elem_bytes(b->direct[a].type, b->direct[a].size);
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j) of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1)) of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

/* The Vulkan vertex format of a direct attribute, or UNDEFINED when the
 * device cannot fetch it (then it is converted to float4 here). */
static VkFormat s_vfmt_cache[8][5];
static int      s_vfmt_known[8][5];

static VkFormat vertex_format(uint32_t type, uint32_t size)
{
    static const VkFormat f_float[5] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                         VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f_snorm16[5] = { 0, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                           VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    static const VkFormat f_unorm8[5] = { 0, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                          VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat f_sscaled16[5] = { 0, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                             VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    VkFormat f;
    if (size < 1 || size > 4 || type > 7)
        return VK_FORMAT_UNDEFINED;
    if (s_vfmt_known[type][size])
        return s_vfmt_cache[type][size];
    switch (type) {
    case 0: f = VK_FORMAT_B8G8R8A8_UNORM; break;
    case 1: f = f_snorm16[size]; break;
    case 2: f = f_float[size]; break;
    case 4: f = f_unorm8[size]; break;
    case 5: f = f_sscaled16[size]; break;
    case 6: f = size == 1 ? VK_FORMAT_R32_UINT : VK_FORMAT_UNDEFINED; break;   /* CMP */
    default: f = VK_FORMAT_UNDEFINED; break;
    }
    if (f != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            if (s_trace)
                LOGE("vertex format %d (type %u size %u) not supported: converted\n", f, type, size);
            f = VK_FORMAT_UNDEFINED;
        }
    }
    s_vfmt_known[type][size] = 1;
    s_vfmt_cache[type][size] = f;
    return f;
}

/* One attribute of vertex v, as the NV2A reads it, into float4. */
static void attr_to_float4(const Nv2aRawBatch *b, uint32_t a, uint32_t v, float out[4])
{
    const uint8_t *p = b->direct[a].ptr + (size_t)v * b->direct[a].stride;
    uint32_t size = b->direct[a].size, c;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (b->direct[a].type) {
    case 0:                                          /* D3DCOLOR: B,G,R,A */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1:
        for (c = 0; c < size && c < 4; c++) {
            int16_t s; memcpy(&s, p + 2 * c, 2);
            out[c] = s / 32767.0f < -1.0f ? -1.0f : s / 32767.0f;
        }
        break;
    case 2:
        for (c = 0; c < size && c < 4; c++) memcpy(&out[c], p + 4 * c, 4);
        break;
    case 4:
        for (c = 0; c < size && c < 4; c++) out[c] = p[c] / 255.0f;
        break;
    case 6: {                                        /* CMP: 11:11:10 normal */
        int32_t v;
        memcpy(&v, p, 4);
        out[0] = (float)((int32_t)((uint32_t)v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)((uint32_t)v << 10) >> 21) / 1023.0f;
        out[2] = (float)(v >> 22) / 511.0f;
        break;
    }
    default:
        for (c = 0; c < size && c < 4; c++) { int16_t s; memcpy(&s, p + 2 * c, 2); out[c] = (float)s; }
        break;
    }
}

static int upload_vertices(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun, a, i, v;
    uint32_t nv = b->vertex_count;
    uint16_t direct = 0, conv, cdirect = 0;
    VkDeviceSize bytes = 0, pack_at, const_at, at;
    VkFormat fmt[NV2A_RAW_ATTRS];
    uint8_t *dst;
    VkVertexInputBindingDescription2EXT bind[NV2A_RAW_ATTRS];
    VkVertexInputAttributeDescription2EXT attr[NV2A_RAW_ATTRS];
    VkBuffer bufs[NV2A_RAW_ATTRS];
    VkDeviceSize offs[NV2A_RAW_ATTRS];
    void *map;

    if (!nv)
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        fmt[a] = VK_FORMAT_UNDEFINED;
        if (!(b->attr_direct & (1u << a)))
            continue;
        fmt[a] = b->direct[a].type == 6 && b->xform != 2 ? VK_FORMAT_UNDEFINED
               : vertex_format(b->direct[a].type, b->direct[a].size);
        if (fmt[a] != VK_FORMAT_UNDEFINED)
            direct |= (uint16_t)(1u << a);
        else
            cdirect |= (uint16_t)(1u << a);          /* direct data, converted here */
    }
    conv = b->attr_present & (uint16_t)~direct;
    nrun = vertex_runs(b, direct, run, of_run);
    for (i = 0; i < nrun; i++) {
        run[i].at = bytes;
        bytes += ((VkDeviceSize)(run[i].hi - run[i].lo) + 15) & ~(VkDeviceSize)15;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += (VkDeviceSize)nv * np * 16;
    const_at = bytes;
    bytes += NV2A_RAW_ATTRS * 16;
    at = ring_alloc(bytes, 16, &map);
    dst = (uint8_t *)map;
    if (!dst)
        return 0;
    for (i = 0; i < nrun; i++)
        memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
    if (np) {
        float *out = (float *)(dst + pack_at);
        for (v = 0; v < nv; v++)
            for (i = 0; i < np; i++) {
                float *o = out + ((size_t)v * np + i) * 4;
                if (cdirect & (1u << slot[i]))
                    attr_to_float4(b, slot[i], v, o);
                else
                    memcpy(o, b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4, 16);
            }
    }
    memcpy(dst + const_at, b->attr_const, NV2A_RAW_ATTRS * 16);

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        memset(&bind[a], 0, sizeof bind[a]);
        memset(&attr[a], 0, sizeof attr[a]);
        bind[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        bind[a].binding = a;
        bind[a].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind[a].divisor = 1;
        attr[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        attr[a].location = a;
        attr[a].binding = a;
        bufs[a] = s_f->ring;
        if (direct & (1u << a)) {
            bind[a].stride = b->direct[a].stride;
            attr[a].format = fmt[a];
            offs[a] = at + run[of_run[a]].at + (VkDeviceSize)(b->direct[a].ptr - run[of_run[a]].lo);
        } else if (conv & (1u << a)) {
            for (i = 0; i < np && slot[i] != a; i++) { }
            bind[a].stride = np * 16;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + pack_at + i * 16;
        } else {
            /* Absent: the title's SET_VERTEX_DATA* value, stride 0. */
            bind[a].stride = 0;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + const_at + a * 16;
        }
    }
    {
        /* Vertex input is dynamic state: re-send it only when it changed
         * (or the command buffer is new) -- consecutive draws of a mesh type
         * repeat it, and NVK re-validates all 16 bindings each time. */
        static VkVertexInputBindingDescription2EXT last_bind[NV2A_RAW_ATTRS];
        static VkVertexInputAttributeDescription2EXT last_attr[NV2A_RAW_ATTRS];
        static uint32_t last_gen;
        if (last_gen != s_cb_gen || memcmp(bind, last_bind, sizeof bind)
            || memcmp(attr, last_attr, sizeof attr)) {
            p_vertex_input(s_cb, NV2A_RAW_ATTRS, bind, NV2A_RAW_ATTRS, attr);
            memcpy(last_bind, bind, sizeof bind);
            memcpy(last_attr, attr, sizeof attr);
            last_gen = s_cb_gen;
        }
    }
    /* Eight at a time: Mesa's common entry point copies the arrays through
     * STACK_ARRAY, which mallocs above 8 elements (~3.6% of the executor
     * on the console in malloc/free). */
    vkCmdBindVertexBuffers(s_cb, 0, NV2A_RAW_ATTRS / 2, bufs, offs);
    vkCmdBindVertexBuffers(s_cb, NV2A_RAW_ATTRS / 2, NV2A_RAW_ATTRS - NV2A_RAW_ATTRS / 2,
                           bufs + NV2A_RAW_ATTRS / 2, offs + NV2A_RAW_ATTRS / 2);
    return 1;
}

/* ── Back end entry points ────────────────────────────────────────── */

/* std140 blocks (gl_vsh.c / gl_psh.c, Vulkan dialect). */
typedef struct {
    float surf[4];
    float m[16];
    float vpoff[4];
    float aa[2];
    int32_t xform;
    float snap_y;              /* nv2a_snap's offset for y; surf[3] is x's */
} VsBlock;

typedef struct {
    float tscale[4][4];          /* vec2 array: 16-byte elements */
    float c0[8][4], c1[8][4];
    float fc0[4], fc1[4];
    float fog[4];
    int32_t afunc;
    float aref;
    float pad[2];
    float fogp[4];               /* nv2a_fog_params */
} FsBlock;

/* ── Instancing ───────────────────────────────────────────────────────
 *
 * A quarter of a race frame's draws repeat the previous draw with only the
 * transform constants changed (NFSU2's drag start line: ~480 of ~1870) --
 * the same mesh at another matrix. Such a run becomes one instanced draw:
 * the first draw is recorded up to its vkCmdDrawIndexed, which is held
 * back (s_pend); every repeat only appends its 192 constants right behind
 * the previous set in the ring (binding 6, which the vertex shader indexes
 * by gl_InstanceIndex * 192); anything else -- another draw, a clear, a
 * flip -- first issues the held draw with the instance count. Instances
 * are drawn in order, so blending and depth come out as separate draws.
 *
 * "Repeat" means: no register changed since the held draw except the
 * constant and program upload windows and the per-draw methods (executor
 * dirty blocks, compared to a copy), the same program, vertex pointers,
 * formats and count, the same indices, absent-attribute values and
 * texture addresses, and no semaphore release or trap in between
 * (vtx_epoch: vertex bytes the title could have rewritten). Only vertex
 * programs (xform 2) whose attributes are all uploaded as stored.
 * RECOMP_VK_INSTANCE=0 turns it off. */
#define VK_INST_MAX   16                    /* gl_vsh.c: c[192 * 16] */
#define VK_CONST_SIZE (192 * 16)

/* ── Reusing uniforms and descriptors within a command buffer ─────────
 *
 * A draw's VsBlock/FsBlock and 192 constants often equal an earlier draw's
 * (the HUD, post-processing, passes over one object), so they are looked up
 * by their exact bytes among this command buffer's uploads and the earlier
 * ring offset is used again; descriptors are pushed only for the bindings
 * whose buffer, offset, view or sampler changed (push descriptors keep the
 * rest until another layout's push disturbs them: present_fxaa). Less ring,
 * fewer NVK descriptor writes. RECOMP_VK_STATE_CACHE=0 off. */
#define UCACHE_BYTES 512
#define UCACHE_SLOTS 64
typedef struct {
    uint32_t     cb_gen, hash, bytes;
    VkDeviceSize at;
    uint8_t      v[UCACHE_BYTES];
} UCacheEnt;
static UCacheEnt s_ucache[UCACHE_SLOTS];
static int s_cache_on = -1;
static uint32_t s_st_uhit, s_st_umiss, s_st_chit, s_st_desc, s_st_desc_saved;
static uint32_t s_const_gen, s_const_cb_gen;
static VkDeviceSize s_const_at;
static uint8_t s_const_last[VK_CONST_SIZE];
static uint32_t s_desc_cb_gen;              /* 0: nothing pushed for s_layout yet */

static int cache_on(void)
{
    if (s_cache_on < 0) {
        const char *e = getenv("RECOMP_VK_STATE_CACHE");
        s_cache_on = !(e && *e == '0');
    }
    return s_cache_on;
}

/* `bytes` (<= UCACHE_BYTES) as a uniform block in this frame's ring. */
static int uniform_upload(const void *v, uint32_t bytes, VkDeviceSize *at)
{
    UCacheEnt *e = NULL;
    void *map;
    if (cache_on()) {
        const uint8_t *b = (const uint8_t *)v;
        uint32_t h = 2166136261u ^ bytes, i, w;
        for (i = 0; i + 4 <= bytes; i += 4) {
            memcpy(&w, b + i, 4);
            h = (h ^ w) * 16777619u;
        }
        e = &s_ucache[(h ^ (h >> 16)) & (UCACHE_SLOTS - 1)];
        if (e->cb_gen == s_cb_gen && e->hash == h && e->bytes == bytes && !memcmp(e->v, v, bytes)) {
            *at = e->at;
            s_st_uhit++;
            return 1;
        }
        e->hash = h;
    }
    *at = ring_alloc(bytes, s_ubo_align, &map);
    if (!map)
        return 0;
    memcpy(map, v, bytes);
    if (e) {
        memcpy(e->v, v, bytes);
        e->bytes = bytes;
        e->at = *at;
        e->cb_gen = s_cb_gen;               /* after ring_alloc: it may flush */
    }
    s_st_umiss++;
    return 1;
}

static void push_draw_descriptors(const VkWriteDescriptorSet wr[7])
{
    static VkDescriptorBufferInfo lb[7];
    static VkDescriptorImageInfo  li[7];
    VkWriteDescriptorSet out[7];
    uint32_t i, n = 0;
    if (!cache_on() || s_desc_cb_gen != s_cb_gen) {
        p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 7, wr);
        for (i = 0; i < 7; i++) {
            if (wr[i].pBufferInfo) lb[i] = *wr[i].pBufferInfo;
            if (wr[i].pImageInfo) li[i] = *wr[i].pImageInfo;
        }
        s_desc_cb_gen = s_cb_gen;
        s_st_desc += 7;
        return;
    }
    for (i = 0; i < 7; i++) {
        if (wr[i].pBufferInfo) {
            const VkDescriptorBufferInfo *b = wr[i].pBufferInfo;
            if (lb[i].buffer == b->buffer && lb[i].offset == b->offset && lb[i].range == b->range)
                continue;
            lb[i] = *b;
        } else {
            const VkDescriptorImageInfo *m = wr[i].pImageInfo;
            if (li[i].sampler == m->sampler && li[i].imageView == m->imageView
                && li[i].imageLayout == m->imageLayout)
                continue;
            li[i] = *m;
        }
        out[n++] = wr[i];
    }
    if (n)
        p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, n, out);
    s_st_desc += n;
    s_st_desc_saved += 7 - n;
}

extern uint8_t *nv2a_pb_reg_dirty(void);
static uint32_t s_regs_seen[0x2000 / 4];    /* registers as of the last draw */

/* Recording on a thread of its own (RECOMP_VK_THREAD, below): the executor's
 * dirty blocks are taken by the queue, which marks here the blocks a record
 * brought; and a queued draw's vertex pointers point into the record, so
 * instancing compares the title's pointers, which the record keeps too. */
static int s_q_on;
static uint8_t s_q_dirty[0x2000 / 64];
static const uint8_t *const *s_q_src;

static uint8_t *reg_dirty(void)
{
    return s_q_on ? s_q_dirty : nv2a_pb_reg_dirty();
}

static const uint8_t *direct_src(const Nv2aRawBatch *b, uint32_t a)
{
    return s_q_src ? s_q_src[a] : b->direct[a].ptr;
}

/* Did a register a draw depends on change since the last draw? */
static int s_inst_why_on;
static uint32_t s_reg_why[0x800];
static int regs_changed(const uint32_t *r)
{
    uint8_t *dirty = reg_dirty();
    uint32_t blk, w;
    int changed = 0;

    for (blk = 0; blk < 0x2000 / 64; blk++) {
        if (!dirty[blk])
            continue;
        dirty[blk] = 0;
        if (blk >= 0xB00 / 64 && blk < 0xC00 / 64)
            continue;                           /* program / constant uploads */
        for (w = blk * 16; w < blk * 16 + 16; w++) {
            uint32_t m = w * 4;
            if (r[w] == s_regs_seen[w])
                continue;
            s_regs_seen[w] = r[w];
            if (m == 0x0100 || m == 0x17FC || (m >= 0x1800 && m < 0x1820)
             || m == 0x1D6C || m == 0x1D70 || m == 0x1E9C || m == 0x1EA4)
                continue;                       /* NOP, BEGIN_END, draw data, semaphore, load slots */
            if (s_inst_why_on)                  /* RECOMP_DRAW_STATS: which register */
                s_reg_why[w & 0x7FF]++;
            changed = 1;
        }
    }
    return changed;
}

static struct {
    int          on;                        /* a draw is held */
    uint32_t     n, count, max;             /* index count, instances so far, room */
    VkDeviceSize cat;                       /* first instance's constants */
    uint32_t     cb_gen;
    uint32_t     prim, vertex_count, index_count;
    uint16_t     attr_present, attr_direct;
    uint32_t     vp_prog_gen, attr_const_gen, vtx_epoch, color_va, zeta_va;
    uint32_t     tex_va[4], pal_va[4];
    float        aa_sx, aa_sy;
    Nv2aSurface  surface;
    struct { const uint8_t *ptr; uint32_t type, size, stride; } direct[NV2A_RAW_ATTRS];
    uint32_t    *idx;
    size_t       idx_cap;
} s_pend;
static int s_inst_on = -1, s_inst_alt;
static uint32_t s_inst_why[9];          /* 0 appended or held, 1 regs, 2 full, 3 cb, 4 shape, 5 tex, 6 gen, 7 vertices, 8 other */
static uint32_t s_st_draws, s_st_calls, s_st_inst, s_st_creuse;

static void pend_flush(void)
{
    if (!s_pend.on)
        return;
    s_pend.on = 0;
    if (s_pend.cb_gen != s_cb_gen)
        return;                                 /* cannot happen: flushes go first */
    vkCmdDrawIndexed(s_cb, s_pend.n, s_pend.count, 0, 0, 0);
    s_st_calls++;
}

static int pend_matches(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (b->xform != 2 || b->prim != s_pend.prim || b->vertex_count != s_pend.vertex_count
     || b->index_count != s_pend.index_count || b->attr_present != s_pend.attr_present
     || b->attr_direct != s_pend.attr_direct || b->vp_prog_gen != s_pend.vp_prog_gen
     || b->attr_const_gen != s_pend.attr_const_gen || b->vtx_epoch != s_pend.vtx_epoch
     || b->color_va != s_pend.color_va || b->zeta_va != s_pend.zeta_va
     || b->aa_sx != s_pend.aa_sx || b->aa_sy != s_pend.aa_sy
     || memcmp(b->tex_va, s_pend.tex_va, sizeof s_pend.tex_va)
     || memcmp(b->pal_va, s_pend.pal_va, sizeof s_pend.pal_va)
     || memcmp(&b->surface, &s_pend.surface, sizeof s_pend.surface))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && (direct_src(b, a) != s_pend.direct[a].ptr || b->direct[a].type != s_pend.direct[a].type
          || b->direct[a].size != s_pend.direct[a].size || b->direct[a].stride != s_pend.direct[a].stride))
            return 0;
    return !memcmp(b->indices, s_pend.idx, (size_t)b->index_count * 4);
}

/* Can b be held as the first instance of a run? */
static int pend_eligible(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (!s_inst_on || b->xform != 2 || (b->attr_present & ~b->attr_direct))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && vertex_format(b->direct[a].type, b->direct[a].size) == VK_FORMAT_UNDEFINED)
            return 0;
    return 1;
}

static void pend_hold(const Nv2aRawBatch *b, uint32_t n, VkDeviceSize cat, uint32_t max)
{
    uint32_t a;
    s_pend.on = 1;
    s_pend.n = n;
    s_pend.count = 1;
    s_pend.max = max;
    s_pend.cat = cat;
    s_pend.cb_gen = s_cb_gen;
    s_pend.prim = b->prim;
    s_pend.vertex_count = b->vertex_count;
    s_pend.index_count = b->index_count;
    s_pend.attr_present = b->attr_present;
    s_pend.attr_direct = b->attr_direct;
    s_pend.vp_prog_gen = b->vp_prog_gen;
    s_pend.attr_const_gen = b->attr_const_gen;
    s_pend.vtx_epoch = b->vtx_epoch;
    s_pend.color_va = b->color_va;
    s_pend.zeta_va = b->zeta_va;
    s_pend.aa_sx = b->aa_sx;
    s_pend.aa_sy = b->aa_sy;
    s_pend.surface = b->surface;
    memcpy(s_pend.tex_va, b->tex_va, sizeof s_pend.tex_va);
    memcpy(s_pend.pal_va, b->pal_va, sizeof s_pend.pal_va);
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        s_pend.direct[a].ptr = direct_src(b, a);
        s_pend.direct[a].type = b->direct[a].type;
        s_pend.direct[a].size = b->direct[a].size;
        s_pend.direct[a].stride = b->direct[a].stride;
    }
    if (b->index_count > s_pend.idx_cap) {
        free(s_pend.idx);
        s_pend.idx_cap = b->index_count + 1024;
        s_pend.idx = (uint32_t *)malloc(s_pend.idx_cap * 4);
        if (!s_pend.idx) {
            s_pend.idx_cap = 0;
            s_pend.on = 0;
            vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
            s_st_calls++;
            return;
        }
    }
    memcpy(s_pend.idx, b->indices, (size_t)b->index_count * 4);
}

/* One more instance of the held draw, when b is one; 0: draw it normally. */
static int pend_append(const Nv2aRawBatch *b, int changed)
{
    VkDeviceSize at;
    void *map;

    if (!s_pend.on)
        return 0;
    if (s_inst_why_on) {            /* RECOMP_DRAW_STATS: why a draw was not appended */
        uint32_t a, why = 0;
        if (changed) why = 1;
        else if (s_pend.count >= s_pend.max) why = 2;
        else if (s_pend.cb_gen != s_cb_gen) why = 3;
        else if (b->vertex_count != s_pend.vertex_count || b->index_count != s_pend.index_count
                 || b->prim != s_pend.prim) why = 4;
        else if (memcmp(b->tex_va, s_pend.tex_va, sizeof s_pend.tex_va)) why = 5;
        else if (b->vp_prog_gen != s_pend.vp_prog_gen || b->attr_const_gen != s_pend.attr_const_gen
                 || b->vtx_epoch != s_pend.vtx_epoch) why = 6;
        else {
            for (a = 0; a < NV2A_RAW_ATTRS; a++)
                if ((b->attr_direct & (1u << a)) && b->direct[a].ptr != s_pend.direct[a].ptr)
                    why = 7;
            if (!why && !pend_matches(b)) why = 8;
        }
        s_inst_why[why]++;
    }
    if (changed || s_pend.count >= s_pend.max || s_pend.cb_gen != s_cb_gen || !pend_matches(b))
        return 0;
    at = (s_f->ring_off + s_ubo_align - 1) & ~(s_ubo_align - 1);
    if (at != s_pend.cat + (VkDeviceSize)s_pend.count * VK_CONST_SIZE
     || at + VK_CONST_SIZE > RING_BYTES)
        return 0;
    at = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map);
    if (!map)
        return 0;
    memcpy(map, b->vp_consts, VK_CONST_SIZE);
    s_const_gen = 0;                            /* the last constants are these now */
    s_pend.count++;
    s_st_inst++;
    return 1;
}

static void vk_draw_raw(const Nv2aRawBatch *b)
{
    VkSurf *s, *sampled[4];
    VkDepthBuf *d;
    VkProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, i, cls;
    VkPrimitiveTopology topo;
    VkDescriptorImageInfo img[4];
    float tscale[4][2];
    PipeKey key;
    VkPipeline pipe;
    VkDescriptorBufferInfo ub[3];
    VkWriteDescriptorSet wr[7];
    uint32_t inst_room = 0;
    void *map;
    {
        /* RECOMP_VK_SKIP_BLEND=<hex dfactor>: drop blended draws with that
         * destination factor (bring-up: which pass makes a colour). */
        static int skip = -2;
        if (skip == -2) {
            const char *e = getenv("RECOMP_VK_SKIP_BLEND");
            skip = e && *e ? (int)strtol(e, NULL, 16) : -1;
        }
        if (skip >= 0 && r[0x304 / 4] && r[0x348 / 4] == (uint32_t)skip)
            return;
    }
    static VkPipeline cur_pipe;
    static uint32_t cur_pipe_gen;

    if (!ready())
        return;
    s_regs = r;
    s_batch = b;
    if (s_lat_slot >= 0)
        lat_poll();
    if (s_inst_on < 0) {
        const char *e = getenv("RECOMP_VK_INSTANCE");
        s_inst_on = !(e && *e == '0') && s_ubo_range >= VK_CONST_SIZE
                 && VK_CONST_SIZE % s_ubo_align == 0;
        if (s_inst_on && e && *e == '2')
            s_inst_alt = 1;                     /* debug: every other frame */
        fprintf(stderr, "  [VK] instancing %s (uniform range %u, alignment %u)\n",
                s_inst_on ? "on" : "off", s_ubo_range, (unsigned)s_ubo_align);
    }
    s_st_draws++;
    {
        int changed = regs_changed(r);
        if (pend_append(b, changed))
            return;
    }
    pend_flush();
    s_kick_draws++;
    if (kick_due(1))
        kick_submit();
    if (!prim_indices(b, &idx, &n, &topo, &cls) || !n)
        return;
    VT("draw: prim %u idx %u verts %u attrs %04X direct %04X xform %u surf %08X zeta %08X\n", b->prim, n,
       b->vertex_count, b->attr_present, b->attr_direct, b->xform, b->surface.color_va, b->zeta_va);
    s = target(&b->surface, b->zeta_va, &d);
    if (!s)
        return;
    s_drew_any = 1;
    psh_key(r, &pk);
    {
        uint32_t made = s_prog_made;
        uint64_t t0 = hz_now();
        p = prog_get(b, &pk);
        if (s_prog_made != made) {
            s_hz_progs += s_prog_made - made;
            s_hz_prog_ns += hz_now() - t0;
        }
    }
    if (!p)
        return;
    s_hz_draws++;

    /* Textures first: uploads end the rendering pass. */
    for (i = 0; i < 4; i++)
        bind_stage(r, (int)i, tscale[i], &img[i], &sampled[i]);
    /* A stage that samples the surface being drawn into (FlatOut 2's bloom
     * composite reads the scene while it overwrites it) is a feedback loop
     * in Vulkan: the GPU reads texels other fragments of the same draw are
     * writing, and the result moved with the tiles -- a wavy shimmer over
     * every bright car body. The NV2A reads what was there before the draw,
     * so it samples a copy of that. */
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i] == s)
            img[i].imageView = self_snapshot(s);
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i]->dirty && sampled[i] != s) {
            end_rendering();                 /* next begin_rendering barriers */
            s_read_pending = 1;
            break;
        }

    memset(&key, 0, sizeof key);
    key.prog = (int)(p - s_prog);
    if (r[0x304 / 4]) {
        key.blend = 1; key.bsrc = r[0x344 / 4]; key.bdst = r[0x348 / 4]; key.beq = r[0x350 / 4];
    }
    {
        uint32_t cm = r[0x358 / 4], m = 0;
        if (cm & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (cm & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (cm & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
        if (cm & 0x01000000u) m |= VK_COLOR_COMPONENT_A_BIT;
        key.cmask = m;
    }
    key.depth = d != NULL;
    key.topo = cls;
    /* ALPHA_TEST_ENABLE off or ALPHA_FUNC ALWAYS (RECOMP_VK_NAT=0: never). */
    {
        static int nat_env = -1;
        if (nat_env < 0) {
            const char *e = getenv("RECOMP_VK_NAT");
            nat_env = !(e && *e == '0');
        }
        key.nat = nat_env && (!r[0x300 / 4] || r[0x33C / 4] == 0x207);
    }
    pipe = pipe_get(&key);
    if (!pipe)
        return;

    {
        /* Everything this draw puts in the ring, reserved now: a flush
         * later in the draw would reuse the ring under its own uniforms. */
        VkDeviceSize need = 2 * 4096 + sizeof(VsBlock) + sizeof(FsBlock) + VK_CONST_SIZE + (VkDeviceSize)n * 4
                          + NV2A_RAW_ATTRS * 16 + 16 * 16;
        uint32_t a;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (b->attr_present & (1u << a)) {
                uint32_t st = (b->attr_direct & (1u << a)) ? b->direct[a].stride : 0;
                need += (VkDeviceSize)b->vertex_count * (st > 16 ? st : 16) + 16;
            }
        if (s_f->ring_off + need > RING_BYTES) {
            void *tmp;
            if (need > RING_BYTES) return;
            ring_alloc(RING_BYTES, 16, &tmp);       /* forces the flush */
            s_f->ring_off = 0;
            s_ring_gen++;
        }
    }
    begin_rendering(s, d);
    s->dirty = 1;
    s->gen++;
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i] != s)
            sampled[i]->sampled = 1;
    if (pipe != cur_pipe || cur_pipe_gen != s_cb_gen || s_pipe_foreign) {
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        cur_pipe = pipe;
        cur_pipe_gen = s_cb_gen;
        s_pipe_foreign = 0;
    }
    apply_dynamic(r, d != NULL, topo);
    set_surface_scissor(r, s);

    /* Uniforms. */
    {
        VsBlock vblock, *vb = &vblock;
        FsBlock fblock, *fb = &fblock;
        VkDeviceSize vat;
        memset(vb, 0, sizeof *vb);
        vb->surf[0] = 2.0f / (float)s->w;
        vb->surf[1] = 2.0f / (float)s->h;
        vb->surf[2] = 1.0f / (float)zmax_of(r);
        vb->surf[3] = 0.5f - 0.5f * (float)s->w / (float)s->pw; /* see nv2a_snap */
        vb->snap_y = 0.5f - 0.5f * (float)s->h / (float)s->ph;
        memcpy(vb->m, b->composite, sizeof vb->m);
        memcpy(vb->vpoff, b->vp_offset, sizeof vb->vpoff);
        vb->aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
        vb->aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
        vb->xform = b->xform == 1 ? 1 : 0;
        if (!uniform_upload(vb, sizeof *vb, &vat)) return;
        ub[0].buffer = s_f->ring;
        ub[0].offset = vat;
        ub[0].range = sizeof(VsBlock);

        {
            VkDeviceSize fat;
            memset(fb, 0, sizeof *fb);
            for (i = 0; i < 4; i++) {
                fb->tscale[i][0] = tscale[i][0];
                fb->tscale[i][1] = tscale[i][1];
            }
            for (i = 0; i < 8; i++) {
                argb_vec4(r[0xA60 / 4 + i], fb->c0[i]);
                argb_vec4(r[0xA80 / 4 + i], fb->c1[i]);
            }
            argb_vec4(r[0x1E20 / 4], fb->fc0);
            argb_vec4(r[0x1E24 / 4], fb->fc1);
            {
                uint32_t fc = r[0x2A8 / 4];
                fb->fog[0] = byte_f(fc, 0); fb->fog[1] = byte_f(fc, 8);
                fb->fog[2] = byte_f(fc, 16); fb->fog[3] = byte_f(fc, 24);
            }
            fb->afunc = r[0x300 / 4] ? (int32_t)r[0x33C / 4] : 0x207;
            fb->aref = (float)(r[0x340 / 4] & 0xFF);
            if (!uniform_upload(fb, sizeof *fb, &fat)) return;
            nv2a_fog_params(r, b->xform, fb->fogp);
            ub[1].buffer = s_f->ring;
            ub[1].offset = fat;
            ub[1].range = sizeof(FsBlock);
        }
    }
    if (!upload_vertices(b))
        return;
    {
        VkDeviceSize iat;
        if (b->vertex_count <= 65536) {
            uint16_t *o;
            iat = ring_alloc((VkDeviceSize)n * 2, 4, &map);
            o = (uint16_t *)map;
            if (!o) return;
            for (i = 0; i < n; i++)
                o[i] = (uint16_t)idx[i];
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT16);
        } else {
            iat = ring_alloc((VkDeviceSize)n * 4, 4, &map);
            if (!map) return;
            memcpy(map, idx, (size_t)n * 4);
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT32);
        }
    }
    {
        /* Constants last: repeats append theirs right behind (pend_append). */
        VkDeviceSize cat, range;
        /* The same constants as the last upload in this command buffer:
         * its offset again (a run held for instancing then cannot append
         * behind it -- pend_append checks -- and draws one by one). */
        if (cache_on() && s_const_gen && s_const_gen == b->vp_const_gen && s_const_cb_gen == s_cb_gen
            && !memcmp(s_const_last, b->vp_consts, VK_CONST_SIZE)) {
            cat = s_const_at;
            s_st_chit++;
        } else {
            cat = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map);
            if (!map) return;
            memcpy(map, b->vp_consts, VK_CONST_SIZE);
            if (cache_on()) {
                memcpy(s_const_last, b->vp_consts, VK_CONST_SIZE);
                s_const_gen = b->vp_const_gen;
                s_const_cb_gen = s_cb_gen;
                s_const_at = cat;
            }
        }
        range = (VkDeviceSize)VK_CONST_SIZE * VK_INST_MAX;
        if (range > s_ubo_range) range = s_ubo_range;
        if (cat + range > RING_BYTES) range = RING_BYTES - cat;
        range = range / VK_CONST_SIZE * VK_CONST_SIZE;
        inst_room = (uint32_t)(range / VK_CONST_SIZE);
        ub[2].buffer = s_f->ring;
        ub[2].offset = cat;
        ub[2].range = range;
    }
    memset(wr, 0, sizeof wr);
    for (i = 0; i < 7; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i < 2 || i == 6) {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &ub[i == 6 ? 2 : i];
        } else {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &img[i - 2];
        }
    }
    push_draw_descriptors(wr);

    VT("  vkCmdDrawIndexed %u\n", n);
    /* The constants were the ring's last allocation, so a repeat's set
     * lands right behind them (pend_append). */
    if (pend_eligible(b) && inst_room > 1) {
        pend_hold(b, n, ub[2].offset, inst_room);
    } else {
        vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
        s_st_calls++;
    }
    if (s_vtrace > 0)
        s_vtrace--;
}

/* A clear of some colour channels only. vkCmdClearAttachments has no write
 * mask, so this draws one triangle over the rect with the channels as the
 * pipeline's colour write mask. Burnout 3 clears its reflection cube faces
 * with A alone (0x83): the alpha is a highlight mask that only the light
 * fittings write, and without the clear it stayed 1 over every wall. */
static VkPipelineLayout s_clr_layout;
static VkShaderModule s_clr_vs, s_clr_fs;
static VkPipeline s_clr_pipe[16][2];            /* [write mask][depth attached] */

static VkPipeline clear_pipe(uint32_t vkmask, int depth)
{
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipeline *pp = &s_clr_pipe[vkmask & 15][depth != 0];

    if (*pp)
        return *pp;
    if (!s_clr_layout) {
        VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_clr_layout) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        s_clr_vs = compile(GLSLANG_STAGE_VERTEX,
            "#version 450\n"
            "void main() {\n"
            "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n");
        s_clr_fs = compile(GLSLANG_STAGE_FRAGMENT,
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 c; };\n"
            "layout(location = 0) out vec4 o;\n"
            "void main() { o = c; }\n");
    }
    if (!s_clr_vs || !s_clr_fs)
        return VK_NULL_HANDLE;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = s_clr_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = s_clr_fs;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = vkmask & 15;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_clr_layout;
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, pp) != VK_SUCCESS) {
        LOGE("clear pipeline creation failed\n");
        *pp = VK_NULL_HANDLE;
    }
    return *pp;
}

static void vk_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkSurf *s;
    VkDepthBuf *d;
    VkClearAttachment ca[2];
    VkClearRect rect;
    uint32_t nca = 0;

    if (!ready())
        return;
    pend_flush();
    s = target(sf, rs ? rs->zeta_va : 0, &d);
    if (!s)
        return;
    memset(&rect, 0, sizeof rect);
    rect.layerCount = 1;
    rect.rect.extent.width = s->pw;
    rect.rect.extent.height = s->ph;
    if (s_regs) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            uint32_t px0 = to_stored(x0, s->pw, s->w), py0 = to_stored(y0, s->ph, s->h);
            rect.rect.offset.x = (int32_t)px0;
            rect.rect.offset.y = (int32_t)py0;
            rect.rect.extent.width = to_stored(x1, s->pw, s->w) - px0;
            rect.rect.extent.height = to_stored(y1, s->ph, s->h) - py0;
        }
    }
    if (d && rect.rect.extent.width > d->pw - (uint32_t)rect.rect.offset.x)
        rect.rect.extent.width = d->pw - (uint32_t)rect.rect.offset.x;
    if (d && rect.rect.extent.height > d->ph - (uint32_t)rect.rect.offset.y)
        rect.rect.extent.height = d->ph - (uint32_t)rect.rect.offset.y;
    memset(ca, 0, sizeof ca);
    if (flags & 0xF0) {
        /* The clear value is in the surface's own format (as xemu reads
         * it): a R5G6B5 target's white is 0xFFFF. Taken as A8R8G8B8 that is
         * A=0 R=0 G=B=255 -- FlatOut 2 clears its car-shadow target white,
         * multiplies the ground by it, and every shadow came out teal. Both
         * the full and the masked clear below take it. */
        uint32_t cf = s_regs ? s_regs[0x0208 / 4] & 0xF : 0;
        if (cf == 3) {                                  /* R5G6B5 */
            uint32_t r5 = (argb >> 11) & 0x1F, g6 = (argb >> 5) & 0x3F, b5 = argb & 0x1F;
            argb = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16) | ((g6 << 2 | g6 >> 4) << 8)
                 | (b5 << 3 | b5 >> 2);
        } else if (cf == 1 || cf == 2) {                /* X1R5G5B5 */
            uint32_t r5 = (argb >> 10) & 0x1F, g5 = (argb >> 5) & 0x1F, b5 = argb & 0x1F;
            argb = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16) | ((g5 << 3 | g5 >> 2) << 8)
                 | (b5 << 3 | b5 >> 2);
        }
    }
    if ((flags & 0xF0) == 0xF0) {
        ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[nca].colorAttachment = 0;
        argb_vec4(argb, ca[nca].clearValue.color.float32);
        nca++;
    }
    if (d && (flags & 0x3)) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        ca[nca].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                             ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[nca].clearValue.depthStencil.depth = (float)((double)z / (double)zmax);
        ca[nca].clearValue.depthStencil.stencil = zstencil & 0xFF;
        nca++;
    }
    if ((flags & 0xF0) && (flags & 0xF0) != 0xF0) {
        /* CLEAR_SURFACE R 0x10, G 0x20, B 0x40, A 0x80: the same order as
         * VkColorComponentFlags. */
        VkPipeline cp = clear_pipe((flags >> 4) & 0xF, d != NULL);
        if (cp) {
            float c[4];
            argb_vec4(argb, c);
            begin_rendering(s, d);
            vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cp);
            vkCmdSetScissor(s_cb, 0, 1, &rect.rect);
            s_sc_cur = rect.rect;
            vkCmdPushConstants(s_cb, s_clr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
            vkCmdDraw(s_cb, 3, 1, 0, 0);
            s_pipe_foreign = 1;                 /* the next draw rebinds its own */
            s_dyn_valid = 0;                    /* and resets its dynamic state */
            s_cb_gen++;                         /* incl. vertex input (cached per gen) */
            s->dirty = 1;
            s->gen++;
        }
    }
    if (!nca)
        return;
    VT("clear %08X %ux%u flags %X rect %d,%d %ux%u depth %p\n", s->va, s->pw, s->ph, flags,
       rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height, (void *)d);
    begin_rendering(s, d);
    vkCmdClearAttachments(s_cb, nca, ca, 1, &rect);
    VT("  cleared\n");
    s->dirty = 1;
    s->gen++;
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;
    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(p + x * 4, 1, 3, f);
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* RECOMP_GL_DUMP: read the surface back (waits for the GPU). */
static void dump_surface(VkSurf *s, const char *path)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements rq;
    VkBufferImageCopy rg;
    void *ptr;

    bci.size = (VkDeviceSize)s->pw * s->ph * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mem) { vkDestroyBuffer(s_dev, buf, NULL); return; }
    vkBindBufferMemory(s_dev, buf, mem, 0);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = s->pw;
    rg.imageExtent.height = s->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    flush_frame_wait();
    if (vkMapMemory(s_dev, mem, 0, bci.size, 0, &ptr) == VK_SUCCESS) {
        dump_bmp(path, (const uint8_t *)ptr, s->pw, s->ph);
        /* RECOMP_VK_DUMP_RAW=1: also <path>.raw, the stored BGRA bytes with
         * alpha (the BMP drops it). */
        if (getenv("RECOMP_VK_DUMP_RAW")) {
            char rp[300];
            FILE *f;
            snprintf(rp, sizeof rp, "%s.raw", path);
            if ((f = fopen(rp, "wb")) != NULL) {
                fwrite(ptr, 4, (size_t)s->pw * s->ph, f);
                fclose(f);
            }
        }
        vkUnmapMemory(s_dev, mem);
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

/* ── Present pass (FXAA) ──────────────────────────────────────────────
 * RECOMP_VK_FXAA=0: the presented surface is blitted to the swapchain as
 * before. Otherwise one full-screen draw scales it with FXAA 3.11 (quality
 * preset, 8 search steps) worked out in the surface's texels: a fixed GPU
 * cost per frame, no draws or CPU work added. */
static int s_fxaa = -1;
static VkDescriptorSetLayout s_pp_dsl;
static VkPipelineLayout      s_pp_layout;
static VkSampler             s_pp_sampler;
static struct { VkFormat fmt; VkPipeline pipe; } s_pp[2];

static const char s_pp_vs[] =
    "#version 450\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_pp_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(set = 0, binding = 1) uniform sampler2D ovl;\n"
    "layout(push_constant) uniform P { vec4 rect; vec4 rcp; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "float L(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "float Lat(vec2 uv) { return L(textureLod(src, uv, 0.0).rgb); }\n"
    "#define LO(x, y) L(textureLodOffset(src, uv, 0.0, ivec2(x, y)).rgb)\n"
    "const float Q[8] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0);\n"
    "vec4 fx() {\n"
    "    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;\n"
    "    vec2 r = pc.rcp.xy;\n"
    "    vec4 cM = textureLod(src, uv, 0.0);\n"
    "    float lM = L(cM.rgb), lD = LO(0, 1), lU = LO(0, -1), lL = LO(-1, 0), lR = LO(1, 0);\n"
    "    float mx = max(lM, max(max(lD, lU), max(lL, lR)));\n"
    "    float mn = min(lM, min(min(lD, lU), min(lL, lR)));\n"
    "    float range = mx - mn;\n"
    "    if (range < max(0.0312, mx * 0.125)) return vec4(cM.rgb, 1.0);\n"
    "    float lDL = LO(-1, 1), lUR = LO(1, -1), lUL = LO(-1, -1), lDR = LO(1, 1);\n"
    "    float lDU = lD + lU, lLR = lL + lR;\n"
    "    float lLc = lDL + lUL, lDc = lDL + lDR, lRc = lDR + lUR, lUc = lUR + lUL;\n"
    "    float eH = abs(-2.0 * lL + lLc) + abs(-2.0 * lM + lDU) * 2.0 + abs(-2.0 * lR + lRc);\n"
    "    float eV = abs(-2.0 * lU + lUc) + abs(-2.0 * lM + lLR) * 2.0 + abs(-2.0 * lD + lDc);\n"
    "    bool hor = eH >= eV;\n"
    "    float l1 = hor ? lU : lL, l2 = hor ? lD : lR;\n"
    "    float g1 = l1 - lM, g2 = l2 - lM;\n"
    "    bool steep1 = abs(g1) >= abs(g2);\n"
    "    float gs = 0.25 * max(abs(g1), abs(g2));\n"
    "    float step = hor ? r.y : r.x, avg;\n"
    "    if (steep1) { step = -step; avg = 0.5 * (l1 + lM); } else avg = 0.5 * (l2 + lM);\n"
    "    vec2 cur = uv;\n"
    "    if (hor) cur.y += step * 0.5; else cur.x += step * 0.5;\n"
    "    vec2 off = hor ? vec2(r.x, 0.0) : vec2(0.0, r.y);\n"
    "    vec2 uv1 = cur - off, uv2 = cur + off;\n"
    "    float e1 = Lat(uv1) - avg, e2 = Lat(uv2) - avg;\n"
    "    bool r1 = abs(e1) >= gs, r2 = abs(e2) >= gs;\n"
    "    if (!r1) uv1 -= off;\n"
    "    if (!r2) uv2 += off;\n"
    "    for (int i = 2; i < 8 && !(r1 && r2); i++) {\n"
    "        if (!r1) e1 = Lat(uv1) - avg;\n"
    "        if (!r2) e2 = Lat(uv2) - avg;\n"
    "        r1 = abs(e1) >= gs; r2 = abs(e2) >= gs;\n"
    "        if (!r1) uv1 -= off * Q[i];\n"
    "        if (!r2) uv2 += off * Q[i];\n"
    "    }\n"
    "    float d1 = hor ? uv.x - uv1.x : uv.y - uv1.y;\n"
    "    float d2 = hor ? uv2.x - uv.x : uv2.y - uv.y;\n"
    "    bool dir1 = d1 < d2;\n"
    "    float px = -min(d1, d2) / (d1 + d2) + 0.5;\n"
    "    bool ok = ((dir1 ? e1 : e2) < 0.0) != (lM < avg);\n"
    "    float fin = ok ? px : 0.0;\n"
    "    float la = (1.0 / 12.0) * (2.0 * (lDU + lLR) + lLc + lRc);\n"
    "    float s1 = clamp(abs(la - lM) / range, 0.0, 1.0);\n"
    "    float s2 = (-2.0 * s1 + 3.0) * s1 * s1;\n"
    "    fin = max(fin, s2 * s2 * 0.75);\n"
    "    if (hor) uv.y += fin * step; else uv.x += fin * step;\n"
    "    return vec4(textureLod(src, uv, 0.0).rgb, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;\n"
    "    vec4 c = pc.rcp.w > 0.5 ? fx() : vec4(textureLod(src, uv, 0.0).rgb, 1.0);\n"
    "    if (pc.rcp.z > 0.5) { vec4 v = textureLod(ovl, uv, 0.0); c.rgb = mix(c.rgb, v.rgb, v.a); }\n"
    "    o = c;\n"
    "}\n";

static int fxaa_on(void)
{
    if (s_fxaa < 0) {
        const char *e = getenv("RECOMP_VK_FXAA");
        s_fxaa = !(e && *e == '0');
    }
    return s_fxaa && nv2a_vk_fxaa;
}

static VkPipeline pp_pipe(VkFormat fmt)
{
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkShaderModule vs, fs;
    VkPipeline pipe = VK_NULL_HANDLE;
    int i;

    for (i = 0; i < 2; i++)
        if (s_pp[i].pipe && s_pp[i].fmt == fmt)
            return s_pp[i].pipe;
    if (!s_pp_layout) {
        VkDescriptorSetLayoutBinding b[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 2;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_pp_dsl) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_pp_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_pp_layout) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(s_dev, &sci, NULL, &s_pp_sampler) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    vs = compile(GLSLANG_STAGE_VERTEX, s_pp_vs);
    fs = compile(GLSLANG_STAGE_FRAGMENT, s_pp_fs);
    if (vs && fs) {
        memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&ba, 0, sizeof ba);
        ba.colorWriteMask = 0xF;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &fmt;
        ci.pNext = &rci;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = s_pp_layout;
        if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS)
            pipe = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
    if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
    if (!pipe) {
        LOGE("FXAA pipeline failed; presenting with a blit\n");
        s_fxaa = 0;
        return VK_NULL_HANDLE;
    }
    i = s_pp[0].pipe ? 1 : 0;
    if (s_pp[i].pipe)
        vkDestroyPipeline(s_dev, s_pp[i].pipe, NULL);   /* a third format: never in practice */
    s_pp[i].fmt = fmt;
    s_pp[i].pipe = pipe;
    return pipe;
}

/* RECOMP_FPS_OSD=1, as nv2a_gl.c's osd_fps: presented frames per second
 * as seven-segment digits top-left (vkCmdClearAttachments rectangles inside
 * the present pass, so FXAA must be on), "[fps1] N" in the log every second,
 * and RECOMP_OSD_TAG's digits small in grey under them. */
static void osd_rects(VkClearRect *r, uint32_t *n, uint32_t max, const char *str,
                      int x0, int top, int u, int H)
{
    static const unsigned char seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                           0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int dw = 4 * u, dh = 7 * u, i = 0;
    const char *c;
    /* (x, y, w, h) with y up from the bottom, as the GL version */
#define OSD_RECT(RX, RY, RW, RH) do { if (*n < max) { \
        r[*n].rect.offset.x = (RX); r[*n].rect.offset.y = H - ((RY) + (RH)); \
        r[*n].rect.extent.width = (uint32_t)(RW); r[*n].rect.extent.height = (uint32_t)(RH); \
        r[*n].baseArrayLayer = 0; r[*n].layerCount = 1; (*n)++; } } while (0)
    for (c = str; *c; c++) {
        unsigned m;
        int x;
        if (*c < '0' || *c > '9')
            continue;
        m = seg[*c - '0'];
        x = x0 + i++ * (dw + 2 * u);
        if (m & 0x01) OSD_RECT(x, top - u, dw, u);                  /* a */
        if (m & 0x02) OSD_RECT(x + dw - u, top - dh / 2, u, dh / 2); /* b */
        if (m & 0x04) OSD_RECT(x + dw - u, top - dh, u, dh / 2 + 1); /* c */
        if (m & 0x08) OSD_RECT(x, top - dh, dw, u);                  /* d */
        if (m & 0x10) OSD_RECT(x, top - dh, u, dh / 2 + 1);          /* e */
        if (m & 0x20) OSD_RECT(x, top - dh / 2, u, dh / 2);          /* f */
        if (m & 0x40) OSD_RECT(x, top - dh / 2 - u / 2, dw, u);      /* g */
    }
#undef OSD_RECT
}

static void osd_clear(VkClearRect *r, uint32_t n, float cr, float cg, float cb)
{
    VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0 };
    if (!n)
        return;
    ca.clearValue.color.float32[0] = cr;
    ca.clearValue.color.float32[1] = cg;
    ca.clearValue.color.float32[2] = cb;
    ca.clearValue.color.float32[3] = 1.0f;
    vkCmdClearAttachments(s_cb, 1, &ca, n, r);
}

/* A digit block's black background: n digits of size u, top edge `top`. */
static void osd_back(VkClearRect *r, int n, int x0, int top, int u, int H)
{
    int dh = 7 * u;
    r->rect.offset.x = x0 - u;
    r->rect.offset.y = H - top - u;
    r->rect.extent.width = (uint32_t)(n * (4 * u + 2 * u) + u);
    r->rect.extent.height = (uint32_t)(dh + 2 * u);
    r->baseArrayLayer = 0;
    r->layerCount = 1;
}

static int digits_in(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s >= '0' && *s <= '9';
    return n;
}

static void osd_fps(VkExtent2D ext)
{
    static int on = -1;
    static unsigned shown, frames;
    static uint64_t t0;
    static const char *tag;
    VkClearRect back[2], fg[64], tg[96];
    uint32_t nb = 0, nf = 0, nt = 0;
    uint64_t now = hz_now();
    int H = (int)ext.height, u, ut;
    char buf[12];

    if (on < 0) {
        const char *e = getenv("RECOMP_FPS_OSD");
        on = e && *e == '1';
        if (ovl_fps_osd < 0)
            ovl_fps_osd = on;
        tag = getenv("RECOMP_OSD_TAG");
        if (tag && !digits_in(tag))
            tag = NULL;
        t0 = now;
    }
    if (!ovl_fps_osd)
        return;
    frames++;
    if (now - t0 >= 1000000000ull) {
        shown = (unsigned)(frames * 1e9 / (double)(now - t0) + 0.5);
        fprintf(stderr, "[fps1] %u\n", shown);
        frames = 0;
        t0 = now;
    }
    u = H / 180 > 2 ? H / 180 : 2;
    ut = u / 2 > 1 ? u / 2 : 1;
    snprintf(buf, sizeof buf, "%u", shown);
    osd_back(&back[nb++], digits_in(buf), 3 * u, H - 3 * u, u, H);
    osd_rects(fg, &nf, 64, buf, 3 * u, H - 3 * u, u, H);
    if (tag) {
        osd_back(&back[nb++], digits_in(tag), 3 * u, H - 13 * u, ut, H);
        osd_rects(tg, &nt, 96, tag, 3 * u, H - 13 * u, ut, H);
    }
    osd_clear(back, nb, 0, 0, 0);
    osd_clear(fg, nf, 1, 1, 0);
    osd_clear(tg, nt, 0.6f, 0.6f, 0.6f);
}

/* Surface s, FXAA'd and scaled into the rectangle (dx, dy, dw, dh) of dst
 * (cleared to black around it), left in layout `final`. Recorded outside
 * rendering; 0 = not recorded (use the blit). */
/* The port's overlay (platform/overlay.h): its image, uploaded through the
 * frame ring when it changed. Returns its view, or VK_NULL_HANDLE when hidden.
 * Recorded outside rendering. */
static VkImageView ovl_view(void)
{
    static VkImage img;
    static VkImageView view;
    static VkDeviceMemory mem;
    static unsigned gen;
    static uint8_t *px;
    static int failed;
    VkBufferImageCopy rg;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkDeviceSize at;
    void *dst;
    int st;

    if (failed || (!px && !(px = (uint8_t *)malloc(OVL_W * OVL_H * 4))))
        return VK_NULL_HANDLE;
    st = ovl_take(&gen, px);
    if (!st)
        return VK_NULL_HANDLE;
    if (!img) {
        if (!make_image(OVL_W, OVL_H, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &img, &view, &mem)) {
            failed = 1;
            return VK_NULL_HANDLE;
        }
        st = 2;
    }
    if (st == 2) {
        at = ring_alloc(OVL_W * OVL_H * 4, 16, &dst);
        if (!dst)
            return VK_NULL_HANDLE;
        memcpy(dst, px, OVL_W * OVL_H * 4);
        memset(&rg, 0, sizeof rg);
        rg.bufferOffset = at;
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = OVL_W;
        rg.imageExtent.height = OVL_H;
        rg.imageExtent.depth = 1;
        end_rendering();
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdCopyBufferToImage(s_cb, s_f->ring, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
    return view;
}

static int present_fxaa(VkSurf *s, VkImage dst, VkImageView dview, VkFormat fmt, VkExtent2D ext,
                        int dx, int dy, int dw, int dh, VkImageLayout final,
                        VkPipelineStageFlags final_stage)
{
    VkPipeline pipe = pp_pipe(fmt);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp = { (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f };
    VkRect2D sc = { { dx, dy }, { (uint32_t)dw, (uint32_t)dh } };
    VkDescriptorImageInfo ii, oi;
    VkWriteDescriptorSet wr[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                   { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    VkImageView ov;
    float pc[8];

    if (!pipe || !dview)
        return 0;
    end_rendering();
    ov = ovl_view();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    ca.imageView = dview;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea.extent = ext;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_pp_sampler;
    ii.imageView = s->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    oi = ii;
    oi.imageView = ov ? ov : s_dummy_view;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &ii;
    wr[1] = wr[0];
    wr[1].dstBinding = 1;
    wr[1].pImageInfo = &oi;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp_layout, 0, 2, wr);
    s_desc_cb_gen = 0;                  /* s_layout's pushed set is disturbed */
    pc[0] = (float)dx; pc[1] = (float)dy; pc[2] = (float)dw; pc[3] = (float)dh;
    pc[4] = 1.0f / (float)s->pw; pc[5] = 1.0f / (float)s->ph;
    pc[6] = ov ? 1.0f : 0.0f;           /* overlay */
    pc[7] = nv2a_vk_fxaa ? 1.0f : 0.0f; /* FXAA, or a plain copy */
    vkCmdPushConstants(s_cb, s_pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    if (final == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        osd_fps(ext);                   /* the swapchain only, not dumps */
    p_end_rendering(s_cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = final == VK_IMAGE_LAYOUT_GENERAL ? VK_ACCESS_MEMORY_READ_BIT : 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = final;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, final_stage,
                         0, 0, NULL, 0, NULL, 1, &b);
    s_dyn_valid = 0;
    return 1;
}

/* RECOMP_GL_DUMP while headless: the FXAA'd frame too, at the surface's
 * size (<prefix>NNNNN_fxaa.bmp), to check the pass without a window. */
static void dump_fxaa(VkSurf *s, const char *path)
{
    static VkSurf t;
    if (t.image && (t.pw != s->pw || t.ph != s->ph)) {
        garbage_add(t.image, t.view, t.mem);
        memset(&t, 0, sizeof t);
    }
    if (!t.image) {
        t.pw = s->pw; t.ph = s->ph;
        if (!make_image(t.pw, t.ph, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &t.image, &t.view, &t.mem)) {
            memset(&t, 0, sizeof t);
            return;
        }
        pace_flip();
    }
    {
        VkExtent2D ext = { t.pw, t.ph };
        if (present_fxaa(s, t.image, t.view, VK_FORMAT_B8G8R8A8_UNORM, ext, 0, 0,
                         (int)t.pw, (int)t.ph, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))
            dump_surface(&t, path);
    }
}

static uint64_t s_guest_flip_ns;          /* last flip the title made (overlay below) */
static int s_overlay_flip;                 /* vk_flip called for the overlay */
static int s_burst;                        /* RECOMP_VK_DUMP_BURST frames left */

/* The host's per-frame hook (switch_saltynx.c: the frame rate for SaltyNX
 * overlays), with the rendered picture's size. Weak and undefined
 * elsewhere: the call is skipped. */
extern void nv2a_host_frame_presented(uint32_t width, uint32_t height) __attribute__((weak));

/* Asynchronous present (RECOMP_VK_ASYNC_PRESENT=0/1; on by default on the
 * Switch): NVK's Switch WSI waits for the GPU to finish the frame inside
 * vkQueuePresentKHR, which held the executor -- the title's pushbuffer --
 * for that long every frame. A worker thread presents instead while the
 * next frame is recorded. One present at a time; the queue and the
 * swapchain are externally synchronized, so present_drain() waits for it
 * before any other queue or swapchain call (acquire, submit, wait idle). */
static struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    pthread_t       t;
    int             on, pending, done;
    uint32_t        idx, pw, ph;
    VkResult        result;
} s_pres = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void *present_thread(void *arg)
{
    (void)arg;
    fs_thread(FS_T_PRES);
    pthread_mutex_lock(&s_pres.m);
    for (;;) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        uint32_t idx;
        VkResult r;
        while (!s_pres.pending || s_pres.done)
            pthread_cond_wait(&s_pres.c, &s_pres.m);
        idx = s_pres.idx;
        pthread_mutex_unlock(&s_pres.m);
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_sc_rendered[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(s_queue, &pi);
        pthread_mutex_lock(&s_pres.m);
        s_pres.result = r;
        s_pres.done = 1;
        pthread_cond_broadcast(&s_pres.c);
    }
    return NULL;
}

static void present_result(VkResult r, uint32_t pw, uint32_t ph)
{
    if (nv2a_host_frame_presented)
        nv2a_host_frame_presented(pw, ph);
    /* Rebuilding on that SUBOPTIMAL waited for the GPU and made a new
     * swapchain every frame: 20 fps on an Adreno 740 that does 60. */
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !s_sc_unrotated)) {
        vkDeviceWaitIdle(s_dev);
        create_swapchain();
    }
}

static void present_drain(void)
{
    VkResult r;
    uint32_t pw, ph;
    if (!s_pres.on)
        return;
    pthread_mutex_lock(&s_pres.m);
    if (!s_pres.pending) {
        pthread_mutex_unlock(&s_pres.m);
        return;
    }
    while (!s_pres.done)
        pthread_cond_wait(&s_pres.c, &s_pres.m);
    r = s_pres.result;
    pw = s_pres.pw; ph = s_pres.ph;
    s_pres.pending = s_pres.done = 0;
    pthread_mutex_unlock(&s_pres.m);
    present_result(r, pw, ph);
}

static void present_image(uint32_t idx, uint32_t pw, uint32_t ph)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_VK_ASYNC_PRESENT");
#if defined(__SWITCH__)
        int want = !(e && *e == '0');
#else
        int want = e && *e == '1';
#endif
        pthread_attr_t at;
        init = 1;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1u << 20);
        s_pres.on = want && pthread_create(&s_pres.t, &at, present_thread, NULL) == 0;
        pthread_attr_destroy(&at);
        fprintf(stderr, "  [VK] present on %s\n", s_pres.on ? "a worker thread" : "the executor");
    }
    if (s_pres.on) {
        pthread_mutex_lock(&s_pres.m);
        s_pres.idx = idx; s_pres.pw = pw; s_pres.ph = ph;
        s_pres.pending = 1;
        s_pres.done = 0;
        pthread_cond_broadcast(&s_pres.c);
        pthread_mutex_unlock(&s_pres.m);
    } else {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_sc_rendered[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &idx;
        present_result(vkQueuePresentKHR(s_queue, &pi), pw, ph);
    }
}

#if defined(__ANDROID__)
/* Android takes the window's surface away while the app is in the
 * background. The renderer, and the game behind it, waits until the app is
 * back, then presents to the new surface (SDL has it by then: its event
 * pump blocks while the activity is paused). */
static void android_background(void)
{
    SDL_Event e;
    uint32_t i;

    fprintf(stderr, "  [VK] app in the background: rendering paused\n");
    present_drain();
    vkDeviceWaitIdle(s_dev);
    while (SDL_WaitEvent(&e) && e.type != SDL_APP_DIDENTERFOREGROUND)
        if (e.type == SDL_QUIT)
            recomp_host_quit("app closed in the background");
    for (i = 0; i < 8; i++)
        if (s_sc_views[i]) {
            vkDestroyImageView(s_dev, s_sc_views[i], NULL);
            s_sc_views[i] = VK_NULL_HANDLE;
        }
    if (s_swapchain)
        vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
    s_swapchain = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(s_inst, s_surface, NULL);
    s_surface = VK_NULL_HANDLE;
    if (!make_surface() || !create_swapchain()) {
        LOGE("no surface after the app came back (%s); running headless\n", SDL_GetError());
        s_headless = 1;
        return;
    }
    fprintf(stderr, "  [VK] app back in the foreground: %ux%u\n",
            s_sc_extent.width, s_sc_extent.height);
}
#endif

static void vk_flip(void)
{
    static int dump_every = -1;
    static char dump_prefix[256];
    VkSurf *s = s_last;
    {   /* The executor knows which buffer is scanned out (g_nv2a_present_va,
         * nv2a_pb_exec.c): the last surface drawn can be an offscreen pass. */
        extern uint32_t g_nv2a_present_va;
        VkSurf *p = g_nv2a_present_va ? surf_find(g_nv2a_present_va) : NULL;
        if (p)
            s = p;
    }
    uint32_t idx = 0;
    int have_image = 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (!ready())
        return;
    if (!s_overlay_flip)
        s_guest_flip_ns = hz_now();
    pend_flush();
    s_frame++;
    if (s_inst_alt)
        s_inst_on = s_frame & 1;
    {
        static int st = -1;
        static uint32_t st_frame;
        if (st < 0) {
            const char *e = getenv("RECOMP_DRAW_STATS");
            st = e && *e == '1';
            s_inst_why_on = st;
        }
        if (st && s_frame - st_frame >= 300) {
            {
                uint32_t nf = s_frame - st_frame;
                fprintf(stderr, "[vk] per frame: %u draws -> %u draw calls (%u instances appended);"
                        " barriers %u full, %u gfx, %u skipped; uniforms %u reused, %u new;"
                        " constants %u reused; descriptors %u pushed, %u kept\n",
                        s_st_draws / nf, s_st_calls / nf, s_st_inst / nf,
                        s_st_bar_full / nf, s_st_bar_gfx / nf, s_st_bar_skip / nf,
                        s_st_uhit / nf, s_st_umiss / nf, s_st_chit / nf,
                        s_st_desc / nf, s_st_desc_saved / nf);
                s_st_bar_full = s_st_bar_gfx = s_st_bar_skip = 0;
                s_st_uhit = s_st_umiss = s_st_chit = s_st_desc = s_st_desc_saved = 0;
            }
            {
                uint32_t k, nf = s_frame - st_frame;
                fprintf(stderr, "[vk] not appended per frame: regs %u, full %u, cb %u, shape %u,"
                        " textures %u, program/epoch %u, vertices %u, other %u\n",
                        s_inst_why[1] / nf, s_inst_why[2] / nf, s_inst_why[3] / nf, s_inst_why[4] / nf,
                        s_inst_why[5] / nf, s_inst_why[6] / nf, s_inst_why[7] / nf, s_inst_why[8] / nf);
                for (k = 0; k < 9; k++) s_inst_why[k] = 0;
                {
                    uint32_t best[6] = {0}, w2, j;
                    for (w2 = 0; w2 < 0x800; w2++)
                        for (j = 0; j < 6; j++)
                            if (!best[j] || s_reg_why[w2] > s_reg_why[best[j]]) {
                                memmove(best + j + 1, best + j, (5 - j) * sizeof best[0]);
                                best[j] = w2;
                                break;
                            }
                    fprintf(stderr, "[vk] registers that broke a run:");
                    for (j = 0; j < 6; j++)
                        if (s_reg_why[best[j]])
                            fprintf(stderr, " %04X x%u", best[j] * 4, s_reg_why[best[j]] / nf);
                    fprintf(stderr, "\n");
                    memset(s_reg_why, 0, sizeof s_reg_why);
                }
            }
            s_st_draws = s_st_calls = s_st_inst = 0;
            st_frame = s_frame;
        }
    }
    {
        static uint64_t last_ns, window_ns;
        static int lines;
        uint64_t now = hz_now();
        if (last_ns) {
            uint64_t dt = now - last_ns;
            if (now - window_ns > 10000000000ull) { window_ns = now; lines = 0; }
            if (dt > 50000000ull && lines++ < 30)
                fprintf(stderr, "[hitch] frame %u: %.0f ms -- %u programs compiled (%.0f ms),"
                        " %u pipelines (%.0f ms), %u textures %.1f MB (%.0f ms), %u draws\n",
                        s_frame, dt / 1e6, s_hz_progs, s_hz_prog_ns / 1e6,
                        s_hz_pipes, s_hz_pipe_ns / 1e6, s_hz_texs,
                        s_hz_tex_bytes / 1048576.0, s_hz_tex_ns / 1e6, s_hz_draws);
        }
        last_ns = now;
        fs_frame(s_hz_draws, s_hz_progs, s_hz_texs);
        s_hz_progs = s_hz_texs = s_hz_draws = s_hz_pipes = 0;
        s_hz_prog_ns = s_hz_tex_ns = s_hz_tex_bytes = s_hz_pipe_ns = 0;
    }
    {
        static int on = -1;
        static uint32_t last_frame;
        static struct timespec last;
        struct timespec now;
        if (on < 0) {
            const char *e = getenv("RECOMP_FPS_LOG");
            on = e && *e == '1';
            clock_gettime(CLOCK_MONOTONIC, &last);
        }
        if (on) {
            double dt;
            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
            if (dt >= 10.0) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128];
                xbox_vblank_report(dt, vb, sizeof vb);
                fprintf(stderr, "[fps] %.1f fps (frame %u), %s\n",
                        (s_frame - last_frame) / dt, s_frame, vb);
                last_frame = s_frame;
                last = now;
            }
        }
    }
    if (dump_every < 0) {
        const char *dd = getenv("RECOMP_GL_DUMP");
        dump_every = 0;
        if (dd && *dd) {
            const char *comma = strchr(dd, ',');
            size_t nn = comma ? (size_t)(comma - dd) : strlen(dd);
            if (nn >= sizeof dump_prefix) nn = sizeof dump_prefix - 1;
            memcpy(dump_prefix, dd, nn);
            dump_prefix[nn] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
        }
    }
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    {
        int pct = nv2a_vk_scale_pct;
        double k = pct < 50 ? 0.0 : pct > 400 ? 4.0 : pct / 100.0;
        int sq = s_square_env && xbox_video_widescreen() && nv2a_vk_square;
        float wide = xbox_wide_ratio;
        if (!(k > 0.0))
            k = s_scale;
        if (k != s_scale || sq != s_square || (sq && wide != s_wide)) {
            s_square = sq;
            s_wide = wide;
            rescale_surfaces(k);
        }
    }
    {   /* RECOMP_VK_DUMP_BURST=<flag file>: once it exists, it is removed and
         * the next 30 frames are all dumped (flicker between frames). */
        static const char *burst_flag;
        static int burst_init;
        if (!burst_init) {
            burst_init = 1;
            burst_flag = getenv("RECOMP_VK_DUMP_BURST");
        }
        if (burst_flag && dump_every && !s_burst && remove(burst_flag) == 0)
            s_burst = 30;
    }
    if (s && s_drew_any && dump_every && ((s_burst > 0 && s_burst--) || (s_frame % (uint32_t)dump_every) == 0
                                          || (s_inst_alt && s_frame % (uint32_t)dump_every == 1))) {
        char path[320];
        snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
        dump_surface(s, path);
        if (s_headless && fxaa_on()) {
            snprintf(path, sizeof path, "%s%05u_fxaa.bmp", dump_prefix, s_frame);
            barrier_all();
            dump_fxaa(s, path);
        }
        {
            /* RECOMP_VK_DUMP_SURF=<hex va>[,<hex va>...]: those surfaces too,
             * as <prefix>NNNNN_s.bmp, _s1.bmp, ... (off-screen targets: the
             * scene a title composites, the faces of a reflection cube). */
            static int init, n_extra;
            static uint32_t extra[8];
            VkSurf *x;
            int k;
            if (!init) {
                const char *e = getenv("RECOMP_VK_DUMP_SURF");
                char *end;
                init = 1;
                while (e && *e && n_extra < 8) {
                    extra[n_extra++] = (uint32_t)strtoul(e, &end, 16);
                    e = *end == ',' ? end + 1 : NULL;
                }
            }
            for (k = 0; k < n_extra; k++) {
                if (!extra[k] || (x = surf_find(extra[k])) == NULL)
                    continue;
                if (k)
                    snprintf(path, sizeof path, "%s%05u_s%d.bmp", dump_prefix, s_frame, k);
                else
                    snprintf(path, sizeof path, "%s%05u_s.bmp", dump_prefix, s_frame);
                dump_surface(x, path);
            }
        }
    }
    barrier_all();

    VT("flip %u: surface %08X drew %d, acquiring\n", s_frame, s ? s->va : 0, s_drew_any);
    {
        uint64_t t0 = hz_now();
        present_drain();
        fs_add(FS_ACQUIRE, hz_now() - t0);
    }
    if (!s_headless) {
        uint64_t t0 = hz_now();
        VkResult rr = vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX, s_f->acquired,
                                            VK_NULL_HANDLE, &idx);
        fs_add(FS_ACQUIRE, hz_now() - t0);
        if (rr == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        } else if (rr == VK_SUCCESS || rr == VK_SUBOPTIMAL_KHR) {
            VkImage dst = s_sc_images[idx];
            VT("flip %u: acquired image %u\n", s_frame, idx);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue black = { { 0, 0, 0, 1 } };
            int dw = 0, dh = 0, dx = 0, dy = 0;
            have_image = 1;
            if (s && s_drew_any) {
                /* The logical aspect (16:9 for a widescreen title),
                 * letterboxed. Rows are top-first on both sides. */
                float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
                float ww = (float)s_sc_extent.width, wh = (float)s_sc_extent.height, scale;
                if (xbox_video_widescreen() && lw < lh * 1.5f)
                    lw = lh * xbox_wide_ratio;
                scale = ww / lw < wh / lh ? ww / lw : wh / lh;
                dw = (int)(lw * scale); dh = (int)(lh * scale);
                dx = ((int)ww - dw) / 2; dy = ((int)wh - dh) / 2;
            }
            (void)fxaa_on();                    /* RECOMP_VK_FXAA=0: the blit, no overlay */
            if (dw > 0 && dh > 0 && s_fxaa
                && present_fxaa(s, dst, s_sc_views[idx], s_sc_format, s_sc_extent, dx, dy, dw, dh,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT))
                goto presented;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = rg;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            vkCmdClearColorImage(s_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
            if (dw > 0 && dh > 0) {
                VkImageBlit bl;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 1, &mb, 0, NULL, 0, NULL);
                memset(&bl, 0, sizeof bl);
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = 1;
                bl.srcOffsets[1].x = (int32_t)s->pw;
                bl.srcOffsets[1].y = (int32_t)s->ph;
                bl.srcOffsets[1].z = 1;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstOffsets[0].x = dx;
                bl.dstOffsets[0].y = dy;
                bl.dstOffsets[1].x = dx + dw;
                bl.dstOffsets[1].y = dy + dh;
                bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            }
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        presented:;
        }
    }
    VT("flip %u: submit (image %d)\n", s_frame, have_image);
    if (s_ts_began[s_fi]) {
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tsq, 2 * s_fi + 1);
        s_ts_began[s_fi] = 0;
        s_ts_done[s_fi] = 1;
    }
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_f->acquired;
        si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_sc_rendered[idx];
    }
    if (vkQueueSubmit(s_queue, 1, &si, s_f->fence) != VK_SUCCESS)
        LOGE("vkQueueSubmit failed\n");
    s_f->submitted = 1;
    if (recomp_lat_on()) {
        lat_poll();
        if (recomp_lat_flip())
            s_lat_slot = (int)s_fi;
    }
    if (have_image) {
        VT("flip %u: vkQueuePresentKHR\n", s_frame);
        present_image(idx, s && s_drew_any ? s->pw : 0, s && s_drew_any ? s->ph : 0);
    }
    {
        static uint64_t last_save;
        uint64_t now = hz_now();
        if (s_pipes_unsaved && now - last_save > 20000000000ull) {
            last_save = now;
            pcache_save(0);
        }
    }
    {
        /* Textures nobody sampled for RECOMP_VK_TEX_IDLE frames (default
         * 900; 0 = keep until the budget evicts them) go, checked every 60
         * frames: a race leaves the front end's and the last track's
         * behind, and every tex_get walks past them. */
        static int idle = -1;
        if (idle < 0) {
            const char *e = getenv("RECOMP_VK_TEX_IDLE");
            idle = e ? atoi(e) : 900;
            if (idle < 0) idle = 0;
        }
        if (idle && s_frame % 60 == 0) {
            uint32_t i;
            for (i = 0; i < VK_MAX_TEX; i++)
                if (s_tex[i].image && s_frame - s_tex[i].used > (uint32_t)idle)
                    tex_free(&s_tex[i]);
        }
    }
    VT("flip %u: presented, waiting for frame slot\n", s_frame);
    s_fi = (s_fi + 1) % VK_FRAMES;
    frame_begin();
    VT("flip %u: next frame recording\n", s_frame);
    s_dyn_valid = 0;
#if !defined(__SWITCH__)
    if (s_win) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {     /* the window's close button, Alt+F4 */
            if (e.type == SDL_QUIT
                || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE))
                recomp_host_quit("window closed");
#if defined(__ANDROID__)
            if (e.type == SDL_APP_WILLENTERBACKGROUND)
                android_background();
#endif
        }
    }
#endif
}


/* ── PVIDEO overlay: movies the title shows without drawing ──────────
 *
 * D3D's UpdateOverlay points the NV2A's video overlay at a YUY2 surface
 * (PVIDEO buffer 0: base + offset, pitch in FORMAT, source step DS_DX/DT_DY
 * in 1/2^20 pixels, destination POINT_OUT/SIZE_OUT on the 640x480 screen,
 * colour key in FORMAT bit 20 / COLOR_KEY) and sets PVIDEO_BUFFER bit 0; the
 * scan-out shows it until PVIDEO_STOP. FlatOut 2 plays its intro movies (XMV)
 * this way and flips nothing meanwhile, so they were black with sound.
 *
 * Called from the executor's thread (nv2a_ack_thread), where every other
 * call into this back end comes from. While the overlay is on and the title
 * has not flipped for 100 ms, the overlay frame is converted (BT.601) into a
 * surface of its own and presented through vk_flip, at most once per 16 ms.
 * A title that keeps flipping over an overlay (colour-keyed) is not handled. */
#define OVL_SURF_VA 0xFE0F0000u

void nv2a_vk_overlay_tick(void)
{
    static uint64_t last_ns;
    static uint8_t *px;
    static size_t px_cap;
    const uint8_t *mem;
    const volatile uint32_t *pv;
    uint32_t src, pitch, ox, oy, ow, oh, dsdx, dtdy, s0, x, y;
    uint64_t now;
    VkSurf *sf;
    VkDeviceSize at;
    void *dst;
    VkBufferImageCopy rg;

    if (s_state <= 0)
        return;
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    pv = (const volatile uint32_t *)(mem + 0xFD008000u);
    if (!(pv[0x700 / 4] & 1u))
        return;
    now = hz_now();
    if (now - s_guest_flip_ns < 100000000ull || now - last_ns < 16000000ull)
        return;
    src   = (pv[0x900 / 4] + pv[0x920 / 4]) & 0x07FFFFFFu;
    {
        /* Only a new frame is converted: UpdateOverlay points buffer 0 at
         * the next surface for each movie frame. The same offset again is
         * the frame already on screen (or, during a load after a movie
         * whose overlay was never stopped, a stale one): converting it at
         * 60 Hz was 10% of this thread on the console. */
        static uint32_t last_src, last_vb;
        uint32_t vb = pv[0x928 / 4] ^ pv[0x950 / 4];
        if (src == last_src && vb == last_vb)
            return;
        last_src = src;
        last_vb = vb;
    }
    last_ns = now;

    pitch = pv[0x958 / 4] & 0x1FFFu;
    ox = pv[0x948 / 4] & 0xFFFFu; oy = pv[0x948 / 4] >> 16;
    ow = pv[0x950 / 4] & 0xFFFFu; oh = pv[0x950 / 4] >> 16;
    dsdx = pv[0x938 / 4]; dtdy = pv[0x940 / 4];
    s0 = (pv[0x930 / 4] & 0xFFFFu) >> 4;           /* 12.4: the 64-byte alignment */
    if (!pitch || !ow || !oh || ow > 4096 || oh > 4096)
        return;

    sf = surf_get(OVL_SURF_VA, 640, 480, 1, 1);
    if (!sf)
        return;
    if (px_cap < (size_t)sf->pw * sf->ph * 4) {
        free(px);
        px_cap = (size_t)sf->pw * sf->ph * 4;
        px = malloc(px_cap);
        if (!px) { px_cap = 0; return; }
    }
    for (y = 0; y < sf->ph; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * 480 / sf->ph);
        uint32_t *row = (uint32_t *)(px + (size_t)y * sf->pw * 4);
        const uint8_t *line = NULL;
        if (sy >= oy && sy < oy + oh) {
            uint32_t ty = (uint32_t)(((uint64_t)(sy - oy) * dtdy) >> 20);
            line = mem + 0x80000000u + src + (size_t)ty * pitch;
        }
        for (x = 0; x < sf->pw; x++) {
            uint32_t sx = (uint32_t)((uint64_t)x * 640 / sf->pw), tx;
            const uint8_t *q;
            int Y, U, V, r, g, b;
            if (!line || sx < ox || sx >= ox + ow) { row[x] = 0xFF000000u; continue; }
            tx = s0 + (uint32_t)(((uint64_t)(sx - ox) * dsdx) >> 20);
            if (tx * 2 + 4 > pitch) tx = pitch / 2 - 2;
            q = line + (tx & ~1u) * 2;                  /* Y0 U Y1 V */
            Y = (tx & 1) ? q[2] : q[0];
            U = q[1] - 128; V = q[3] - 128;
            Y = (Y - 16) * 298;
            r = (Y + 409 * V + 128) >> 8;
            g = (Y - 100 * U - 208 * V + 128) >> 8;
            b = (Y + 516 * U + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    end_rendering();
    barrier_all();
    at = ring_alloc((VkDeviceSize)sf->pw * sf->ph * 4, 16, &dst);
    if (!dst)
        return;
    memcpy(dst, px, (size_t)sf->pw * sf->ph * 4);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = sf->pw;
    rg.imageExtent.height = sf->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, sf->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    sf->dirty = 1;
    s_last = sf;
    s_drew_any = 1;
    s_overlay_flip = 1;
    vk_flip();
    s_overlay_flip = 0;
}

/* ── Recording on a thread of its own (RECOMP_VK_THREAD=1) ────────────
 *
 * The console profile of a race: one thread (the executor) at 95-100% of a
 * core, decoding the pushbuffer and recording Vulkan one after the other,
 * while the game thread waits on it. With this on, the executor only decodes
 * and queues; a thread of its own records and submits, so the two halves
 * run on two cores (core 1 and core 2 with RECOMP_NX_PIN).
 *
 * The queue is the GL renderer's (nv2a_gl.c, RECOMP_GL_THREAD), records
 * and all: a queued draw must not point at anything the executor or the
 * title may change once it moves on, so the executor copies the method
 * shadow, transform program and constants (as the 64/16-byte blocks that
 * differ from what was last queued), the vertex byte ranges a draw reads,
 * converted attributes and indices. Texture images are still read from
 * guest memory when the recording thread gets to them, at most two flips
 * behind; their addresses come resolved in the batch. Besides the pointers,
 * a draw record keeps the title's vertex pointers (instancing compares
 * them), and the recording thread marks the shadow blocks a record brought
 * (regs_changed reads those in place of the executor's). */
enum { Q_WRAP = 0, Q_DRAW = 1, Q_CLEAR = 2, Q_FLIP = 3 };
typedef struct { uint32_t type, size, nreg, nprog, nconst, pad[3]; } QHdr;
typedef struct { uint32_t idx, pad[3]; uint32_t w[16]; } QRegChunk;   /* 64 B of shadow */
typedef struct { uint32_t idx, pad[3]; uint32_t w[4]; } QRow;         /* 16 B row */
typedef struct {
    Nv2aSurface s;
    Nv2aRenderState rs;
    int has_rs;
    uint32_t flags, argb, zs;
} QClear;

#define Q_BYTES    (32u << 20)
#define Q_ALIGN(x) (((x) + 15u) & ~(size_t)15u)
#define REG_WORDS  (0x2000 / 4)
#define PROG_ROWS  136
#define CONST_ROWS 192

static uint8_t *s_q;
static size_t s_q_head, s_q_tail;               /* bytes written / read, ever */
static int s_q_consumer_waits, s_q_producer_waits, s_q_flips;
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cv = PTHREAD_COND_INITIALIZER;
static uint64_t s_q_dropped;
/* For the perf report: time the recording thread sat with an empty queue,
 * and the executor was held back (queue full, or two flips queued). ns. */
static uint64_t s_q_idle_ns, s_q_exec_wait_ns;

/* Executor thread: what was last queued, and where the live shadow is. */
static uint32_t s_q_regs[REG_WORDS], s_q_prog[PROG_ROWS][4];
static float s_q_consts[CONST_ROWS][4];
static const uint32_t *s_q_live_regs;
static uint32_t s_q_prog_gen, s_q_const_gen;
/* Recording thread: its own copies, rebuilt from the records. */
static uint32_t g_regs[REG_WORDS], g_prog[PROG_ROWS][4];
static float g_consts[CONST_ROWS][4];

static void q_timed_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s_q_cv, &s_q_lock, &ts);
}

/* Room for `size` contiguous bytes; returns where to write them. */
static uint8_t *q_reserve(size_t size)
{
    size_t off = s_q_head % Q_BYTES, need = size;
    if (off + size > Q_BYTES)
        need += Q_BYTES - off;                  /* the wrap filler */
    if (Q_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need) {
        uint64_t t0 = hz_now();
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (Q_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, hz_now() - t0, __ATOMIC_RELAXED);
    }
    if (off + size > Q_BYTES) {
        QHdr *w = (QHdr *)(s_q + off);
        w->type = Q_WRAP;                        /* ring has slack past the end */
        w->size = (uint32_t)(Q_BYTES - off);
        __atomic_store_n(&s_q_head, s_q_head + (Q_BYTES - off), __ATOMIC_RELEASE);
        off = 0;
    }
    return s_q + off;
}

static void q_publish(size_t size)
{
    __atomic_store_n(&s_q_head, s_q_head + size, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_q_consumer_waits, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&s_q_lock);
        pthread_cond_broadcast(&s_q_cv);
        pthread_mutex_unlock(&s_q_lock);
    }
}

/* The shadow blocks that changed since the last record: the executor's
 * dirty blocks that really differ, kept until q_reg_emit. */
static uint16_t s_q_pend[REG_WORDS / 16];
static uint32_t s_q_npend;

static uint32_t q_reg_collect(void)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t c;
    if (!s_q_live_regs)
        return 0;
    for (c = 0; c < REG_WORDS / 16; c++) {
        if (!dirty[c])
            continue;
        dirty[c] = 0;
        if (memcmp(s_q_live_regs + c * 16, s_q_regs + c * 16, 64) != 0) {
            uint32_t k;
            for (k = 0; k < s_q_npend && s_q_pend[k] != c; k++) { }
            if (k == s_q_npend)
                s_q_pend[s_q_npend++] = (uint16_t)c;
        }
    }
    return s_q_npend;
}

static uint32_t q_reg_emit(QRegChunk *out)
{
    uint32_t k, n = s_q_npend;
    for (k = 0; k < n; k++) {
        uint32_t c = s_q_pend[k];
        out[k].idx = c;
        memcpy(out[k].w, s_q_live_regs + c * 16, 64);
        memcpy(s_q_regs + c * 16, s_q_live_regs + c * 16, 64);
    }
    s_q_npend = 0;
    return n;
}

static uint32_t q_rows_diff(const uint32_t (*live)[4], uint32_t (*sent)[4], uint32_t rows,
                            QRow *out)
{
    uint32_t r, n = 0;
    for (r = 0; r < rows; r++)
        if (memcmp(live[r], sent[r], 16) != 0) {
            if (out) {
                out[n].idx = r;
                memcpy(out[n].w, live[r], 16);
                memcpy(sent[r], live[r], 16);
            }
            n++;
        }
    return n;
}

/* Header plus the shadow changes; returns the offset after them. */
static size_t q_write_state(uint8_t *rec, uint32_t type, uint32_t nprog, uint32_t nconst,
                            const Nv2aRawBatch *b)
{
    QHdr *h = (QHdr *)rec;
    size_t at = sizeof(QHdr);
    memset(h, 0, sizeof *h);
    h->type = type;
    h->nreg = q_reg_emit((QRegChunk *)(rec + at));
    at += (size_t)h->nreg * sizeof(QRegChunk);
    if (b && nprog) {
        h->nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nprog * sizeof(QRow);
    }
    if (b && nconst) {
        h->nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                                (uint32_t (*)[4])s_q_consts, CONST_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nconst * sizeof(QRow);
    }
    return at;
}

/* The converted attributes of b->attrs into a record, in the same [v][a][c]
 * layout: only the rows vk_draw_raw reads (attr_present & ~attr_direct).
 * Copying the whole array cost 256 bytes a vertex; on the Switch that was
 * 46% of the executor in Conker's field (prof_vk.bin, 2026-10-21). */
static void q_copy_conv(uint8_t *dst, const Nv2aRawBatch *b, uint16_t conv)
{
    float *o = (float *)dst;
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, a, v, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    for (v = 0; v < b->vertex_count; v++)
        for (i = 0; i < np; i++) {
            size_t k = ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4;
            memcpy(o + k, b->attrs + k, 16);
        }
}

static void q_draw(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun = vertex_runs(b, b->attr_direct, run, of_run), i, a, nreg, nprog = 0, nconst = 0;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    size_t size, at, runs_at[NV2A_RAW_ATTRS], attrs_at = 0, idx_at, batch_at, src_at;
    Nv2aRawBatch *qb;
    const uint8_t **src;
    uint8_t *rec;

    s_q_live_regs = b->regs;
    nreg = q_reg_collect();
    if (b->vp_program && b->vp_prog_gen != s_q_prog_gen && b->vp_slots <= PROG_ROWS)
        nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, NULL);
    if (b->vp_consts && b->vp_const_gen != s_q_const_gen && b->vp_const_count <= CONST_ROWS)
        nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                             (uint32_t (*)[4])s_q_consts, CONST_ROWS, NULL);
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk)
         + (size_t)(nprog + nconst) * sizeof(QRow);
    batch_at = size;
    size += Q_ALIGN(sizeof(Nv2aRawBatch));
    src_at = size;
    size += Q_ALIGN(sizeof(const uint8_t *) * NV2A_RAW_ATTRS);
    for (i = 0; i < nrun; i++) {
        runs_at[i] = size;
        size += Q_ALIGN((size_t)(run[i].hi - run[i].lo));
    }
    if (conv) {
        attrs_at = size;
        size += Q_ALIGN((size_t)b->vertex_count * NV2A_RAW_ATTRS * 16);
    }
    idx_at = size;
    size += Q_ALIGN((size_t)b->index_count * 4);
    if (size > Q_BYTES / 4) {
        if ((s_q_dropped++ & 1023) == 0)
            LOGE("queued draw of %zu bytes dropped (%llu so far)\n",
                 size, (unsigned long long)s_q_dropped);
        return;
    }

    rec = q_reserve(size);
    at = q_write_state(rec, Q_DRAW, nprog, nconst, b);
    s_q_prog_gen = b->vp_prog_gen;
    s_q_const_gen = b->vp_const_gen;
    if (at != batch_at) {                       /* cannot happen: same diffs */
        LOGE("queue record mismatch %zu/%zu\n", at, batch_at);
        abort();
    }
    qb = (Nv2aRawBatch *)(rec + batch_at);
    *qb = *b;
    src = (const uint8_t **)(rec + src_at);
    /* Pointers become offsets into the record; the recording thread fixes
     * them. */
    for (i = 0; i < nrun; i++)
        memcpy(rec + runs_at[i], run[i].lo, (size_t)(run[i].hi - run[i].lo));
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        src[a] = (b->attr_direct & (1u << a)) ? b->direct[a].ptr : NULL;
        if (b->attr_direct & (1u << a))
            qb->direct[a].ptr = (const uint8_t *)(uintptr_t)
                (runs_at[of_run[a]] + (size_t)(b->direct[a].ptr - run[of_run[a]].lo));
    }
    if (conv)
        q_copy_conv(rec + attrs_at, b, conv);
    qb->attrs = (const float *)(uintptr_t)attrs_at;
    memcpy(rec + idx_at, b->indices, (size_t)b->index_count * 4);
    qb->indices = (const uint32_t *)(uintptr_t)idx_at;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                    uint32_t flags, uint32_t argb, uint32_t zs)
{
    uint32_t nreg = q_reg_collect();
    size_t size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk) + Q_ALIGN(sizeof(QClear));
    uint8_t *rec = q_reserve(size);
    size_t at = q_write_state(rec, Q_CLEAR, 0, 0, NULL);
    QClear *c = (QClear *)(rec + at);
    c->s = *sf;
    c->has_rs = rs != NULL;
    if (rs)
        c->rs = *rs;
    c->flags = flags;
    c->argb = argb;
    c->zs = zs;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_flip(void)
{
    uint32_t nreg;
    size_t size;
    uint8_t *rec;

    /* No more than two frames behind the executor. */
    if (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= 2) {
        uint64_t t0 = hz_now();
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= 2)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, hz_now() - t0, __ATOMIC_RELAXED);
    }
    nreg = q_reg_collect();
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk);
    rec = q_reserve(size);
    q_write_state(rec, Q_FLIP, 0, 0, NULL);
    ((QHdr *)rec)->size = (uint32_t)size;
    __atomic_add_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
    q_publish(size);
}

static void *q_vk_thread(void *arg)
{
    extern void xbox_nx_pin_renderer(void);
    extern void xbox_nx_track_thread(void *entry);
    (void)arg;
    xbox_nx_pin_renderer();
    xbox_nx_track_thread((void *)q_vk_thread);
    fs_thread(FS_T_REC);
    for (;;) {
        size_t avail = __atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) - s_q_tail;
        uint8_t *rec;
        const QHdr *h;
        size_t at;
        uint32_t i;

        if (!avail) {
            uint64_t t0 = hz_now();
            pthread_mutex_lock(&s_q_lock);
            __atomic_store_n(&s_q_consumer_waits, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) == s_q_tail)
                q_timed_wait();
            __atomic_store_n(&s_q_consumer_waits, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_q_lock);
            __atomic_add_fetch(&s_q_idle_ns, hz_now() - t0, __ATOMIC_RELAXED);
            continue;
        }
        rec = s_q + s_q_tail % Q_BYTES;
        h = (const QHdr *)rec;
        if (h->type != Q_WRAP) {
            at = sizeof(QHdr);
            for (i = 0; i < h->nreg; i++, at += sizeof(QRegChunk)) {
                const QRegChunk *c = (const QRegChunk *)(rec + at);
                memcpy(g_regs + c->idx * 16, c->w, 64);
                s_q_dirty[c->idx] = 1;
            }
            for (i = 0; i < h->nprog; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_prog[r->idx], r->w, 16);
            }
            for (i = 0; i < h->nconst; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_consts[r->idx], r->w, 16);
            }
            s_regs = g_regs;                    /* what clears/presents read */
            if (h->type == Q_DRAW) {
                Nv2aRawBatch bb = *(const Nv2aRawBatch *)(rec + at);
                uint32_t a;
                bb.regs = g_regs;
                bb.vp_program = (const uint32_t (*)[4])g_prog;
                bb.vp_consts = (const float (*)[4])g_consts;
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if (bb.attr_direct & (1u << a))
                        bb.direct[a].ptr = rec + (uintptr_t)bb.direct[a].ptr;
                bb.attrs = (const float *)(rec + (uintptr_t)bb.attrs);
                bb.indices = (const uint32_t *)(rec + (uintptr_t)bb.indices);
                s_q_src = (const uint8_t *const *)(rec + at + Q_ALIGN(sizeof(Nv2aRawBatch)));
                vk_draw_raw(&bb);
                s_q_src = NULL;
            } else if (h->type == Q_CLEAR) {
                const QClear *c = (const QClear *)(rec + at);
                vk_clear(&c->s, c->has_rs ? &c->rs : NULL, c->flags, c->argb, c->zs);
            } else if (h->type == Q_FLIP) {
                vk_flip();
                __atomic_sub_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
            }
        }
        __atomic_store_n(&s_q_tail, s_q_tail + h->size, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s_q_producer_waits, __ATOMIC_SEQ_CST)) {
            pthread_mutex_lock(&s_q_lock);
            pthread_cond_broadcast(&s_q_cv);
            pthread_mutex_unlock(&s_q_lock);
        }
    }
    return NULL;
}

/* Share of the last `secs` the recording thread sat idle and the executor
 * was held back by the queue; -1 when the thread is off. */
void nv2a_gl_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    if (!s_q_on) {
        *gl_idle_pct = *exec_wait_pct = -1;
        return;
    }
    *gl_idle_pct = (int)(__atomic_exchange_n(&s_q_idle_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
    *exec_wait_pct = (int)(__atomic_exchange_n(&s_q_exec_wait_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
}

static int q_start(void)
{
    pthread_t t;
    pthread_attr_t at;
    s_q = (uint8_t *)malloc(Q_BYTES + 256);
    if (!s_q)
        return 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&t, &at, q_vk_thread, NULL) != 0) {
        free(s_q);
        s_q = NULL;
        return 0;
    }
    pthread_detach(t);
    return 1;
}

static const Nv2aBackend s_backend_q = {
    q_clear,
    NULL,
    q_flip,
    q_draw,
    /* RAW_CMP as the direct back end: the recording thread runs the same
     * vk_draw_raw, which unpacks CMP normals itself. Without it every
     * batch with CMP normals was converted on the executor (fetch_attr). */
    NV2A_BACKEND_RAW_DIRECT | NV2A_BACKEND_RAW_CMP,
};

static void vk_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s; (void)b;                        /* draw_raw handles every batch */
}

static const Nv2aBackend s_backend = {
    vk_clear,
    vk_draw,
    vk_flip,
    vk_draw_raw,
    NV2A_BACKEND_RAW_DIRECT | NV2A_BACKEND_RAW_CMP,
};

void nv2a_vk_install(void)
{
    {
        const char *e = getenv("RECOMP_VK_THREAD");
        /* On by default on the Switch: Conker on lavapipe, executor 14.6 ->
         * 8.8 ms per frame with recording on this thread. */
#if defined(__SWITCH__)
        s_q_on = !(e && *e == '0') && q_start();
#else
        s_q_on = e && *e == '1' && q_start();
#endif
    }
    if (s_q_on) {
        nv2a_backend_register(&s_backend_q);
        fprintf(stderr, "[BOOT] NV2A Vulkan renderer registered, recording on a thread of its own\n");
        return;
    }
    extern void (*g_nv2a_overlay_tick)(void);
    g_nv2a_overlay_tick = nv2a_vk_overlay_tick;
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[BOOT] NV2A Vulkan renderer registered\n");
}
