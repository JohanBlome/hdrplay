#include "renderer.h"
#include "hud.h"
#include "log.h"
#include "probe.h"

#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <libavutil/frame.h>

/* ------------------------------------------------------------------ */
/* libplacebo logger bridge                                           */
/* ------------------------------------------------------------------ */
static void pl_log_cb(void *priv, enum pl_log_level level, const char *msg)
{
    const char *lvl = "?";
    switch (level) {
        case PL_LOG_FATAL: case PL_LOG_ERR:   lvl = "ERR";  break;
        case PL_LOG_WARN:                     lvl = "WARN"; break;
        case PL_LOG_INFO:                     lvl = "INFO"; break;
        case PL_LOG_DEBUG: case PL_LOG_TRACE: lvl = "DBG";  break;
        default: break;
    }
    LOG("GPU", "placebo %s: %s", lvl, msg);
}

/* ------------------------------------------------------------------ */
/* libplacebo info callback — fires for every render pass / decision  */
/* ------------------------------------------------------------------ */
static void pl_info_cb(void *priv, const struct pl_render_info *info)
{
    Renderer *r = (Renderer *)priv;
    /* info->stage tells us which conceptual pass this is.
     * info->pass->desc is a short human description from libplacebo. */
    if (info->stage == PL_RENDER_STAGE_FRAME) {
        r->last_num_passes++;
        /* libplacebo 7.x: pass->shader->description is a comma-separated
         * list of semantic steps performed by this shader pass, e.g.
         * "color decoding, tone mapping, debanding, dithering". */
        if (info->pass && info->pass->shader && info->pass->shader->description) {
            const char *desc = info->pass->shader->description;
            if (strstr(desc, "tone") || strstr(desc, "Tone")) {
                snprintf(r->last_tonemap, sizeof(r->last_tonemap), "%s", desc);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* SDL3 + Vulkan instance / surface plumbing                          */
/* ------------------------------------------------------------------ */
static VkSurfaceKHR create_surface(SDL_Window *win, VkInstance inst)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(win, inst, NULL, &surface)) {
        LOG("GPU", "ERROR: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

/* ------------------------------------------------------------------ */
/* Display HDR state — polled before each frame so we can react to    */
/* the user dragging the window between SDR and HDR monitors.         */
/* ------------------------------------------------------------------ */
void renderer_update_display_state(Renderer *r)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(r->window);
    /* These property names track SDL 3.2.x. If your SDL3 build is older,
     * compile-time symbol names may differ — grep SDL_video.h for HDR. */
    bool  hdr   = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    float white = SDL_GetFloatProperty (props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 203.0f);
    float head  = SDL_GetFloatProperty (props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT,    1.0f);

    if (hdr != r->display_hdr_capable ||
        fabsf(white - r->display_sdr_white)    > 0.5f ||
        fabsf(head  - r->display_hdr_headroom) > 0.01f)
    {
        LOG("HDR", "display state: hdr=%s, sdr_white=%.1f nits, headroom=%.2fx",
            hdr ? "ON" : "off", white, head);
    }
    r->display_hdr_capable  = hdr;
    r->display_sdr_white    = white;
    r->display_hdr_headroom = head;
}

/* ------------------------------------------------------------------ */
/* Display enumeration                                                 */
/* ------------------------------------------------------------------ */
static bool sdl_video_started = false;

static void ensure_sdl_video(void)
{
    if (sdl_video_started) return;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG("GPU", "SDL_Init: %s", SDL_GetError());
        return;
    }
    sdl_video_started = true;
}

void renderer_list_displays(void)
{
    ensure_sdl_video();

    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids || count == 0) {
        fprintf(stderr, "  (no displays detected)\n");
        return;
    }
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();

    for (int i = 0; i < count; i++) {
        SDL_DisplayID id   = ids[i];
        const char *name   = SDL_GetDisplayName(id);
        SDL_Rect bounds    = {0};
        SDL_GetDisplayBounds(id, &bounds);
        SDL_PropertiesID p = SDL_GetDisplayProperties(id);
        bool hdr           = SDL_GetBooleanProperty(p, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        /* SDR white and headroom are window-level in SDL3 (they depend on
         * which monitor a window is currently on), so they're only known
         * after the window has been placed. See renderer_update_display_state. */
        fprintf(stderr,
            "  [%d] %s%s  %dx%d @ (%d,%d)  hdr=%s\n",
            i, name ?: "(unnamed)", id == primary ? " *primary*" : "",
            bounds.w, bounds.h, bounds.x, bounds.y,
            hdr ? "ON" : "off");
    }
    SDL_free(ids);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
bool renderer_init(Renderer *r, int width, int height, const char *title, int display_index)
{
    memset(r, 0, sizeof(*r));

    ensure_sdl_video();
    if (!sdl_video_started) return false;

    /* Hint SDL to expose HDR-aware swapchain properties. On displays that
     * are currently in HDR mode, SDL will set the surface colorspace to
     * an HDR-capable Vulkan colorspace at swapchain create time. */
    SDL_SetHint("SDL_VIDEO_FORCE_EGL", "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    /* Resolve display_index → SDL_DisplayID, so we can place the window
     * on a specific monitor at creation time. */
    SDL_DisplayID target_display = 0;
    if (display_index >= 0) {
        int count = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&count);
        if (ids && display_index < count) {
            target_display = ids[display_index];
            LOG("GPU", "targeting display [%d]: %s",
                display_index, SDL_GetDisplayName(target_display));
        } else {
            LOG("GPU", "display index %d out of range (have %d) — using primary",
                display_index, count);
        }
        SDL_free(ids);
    }

    /* SDL3 window creation goes through a properties bag when we want to
     * set position. SDL_WINDOWPOS_CENTERED_DISPLAY(id) puts the window
     * in the middle of the target display. */
    SDL_PropertiesID wp = SDL_CreateProperties();
    SDL_SetStringProperty (wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    if (target_display) {
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
    }
    r->window = SDL_CreateWindowWithProperties(wp);
    SDL_DestroyProperties(wp);
    if (!r->window) {
        LOG("GPU", "SDL_CreateWindowWithProperties: %s", SDL_GetError());
        return false;
    }

    /* libplacebo log. PL_LOG_INFO is chatty during init (lists every
     * Vulkan extension considered). PL_LOG_WARN is the right default
     * for an insight-not-debug tool; -v promotes it. */
    r->pl_log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb     = pl_log_cb,
        .log_level  = g_verbose ? PL_LOG_INFO : PL_LOG_WARN,
    ));

    /* Get Vulkan extensions SDL needs for surface creation. */
    Uint32 num_ext = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&num_ext);
    if (!sdl_ext) {
        LOG("GPU", "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return false;
    }
    for (Uint32 i = 0; i < num_ext; i++)
        LOG("GPU", "vk instance ext: %s", sdl_ext[i]);

    /* libplacebo creates the Vulkan instance. On macOS we additionally
     * need VK_KHR_portability_enumeration so MoltenVK (a "portability
     * subset" driver) is allowed to enumerate. Without this, the loader
     * silently skips MoltenVK and we get the "no driver" error. The
     * VK_KHR_get_physical_device_properties2 extension is a dependency
     * for portability_subset on the device side. */
    const char *opt_ext[] = {
        "VK_KHR_portability_enumeration",
        "VK_KHR_get_physical_device_properties2",
    };

    r->vk_inst = pl_vk_inst_create(r->pl_log, pl_vk_inst_params(
        .extensions       = sdl_ext,
        .num_extensions   = num_ext,
        .opt_extensions   = opt_ext,
        .num_opt_extensions = sizeof(opt_ext) / sizeof(opt_ext[0]),
        .get_proc_addr    = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr(),
    ));
    if (!r->vk_inst) { LOG("GPU", "ERROR: pl_vk_inst_create"); return false; }

    VkSurfaceKHR surface = create_surface(r->window, r->vk_inst->instance);
    if (!surface) return false;

    r->vulkan = pl_vulkan_create(r->pl_log, pl_vulkan_params(
        .instance       = r->vk_inst->instance,
        .get_proc_addr  = r->vk_inst->get_proc_addr,
        .surface        = surface,
    ));
    if (!r->vulkan) { LOG("GPU", "ERROR: pl_vulkan_create"); return false; }

    /* libplacebo 7 doesn't expose a GPU name string directly; query
     * Vulkan for it. This is the "which physical adapter are we on?"
     * insight — on macOS it'll typically be "Apple M*" via MoltenVK. */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(r->vulkan->phys_device, &props);
    LOG("GPU", "vulkan device: %s (api %u.%u.%u)", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    /* Swapchain wrapped around the Vulkan surface. libplacebo will pick
     * an HDR-capable surface format (VK_COLOR_SPACE_HDR10_ST2084_EXT or
     * similar) if the surface advertises one — which it does iff the OS
     * compositor is currently in HDR mode for this window's display. */
    r->swapchain = pl_vulkan_create_swapchain(r->vulkan, pl_vulkan_swapchain_params(
        .surface          = surface,
        .present_mode     = VK_PRESENT_MODE_FIFO_KHR,
        .swapchain_depth  = 3,
    ));
    if (!r->swapchain) { LOG("GPU", "ERROR: pl_vulkan_create_swapchain"); return false; }

    /* Hint our preferred output colorspace. libplacebo will negotiate
     * against what the swapchain actually exposes; if HDR is unavailable
     * it falls back to BT.709 + sRGB and tone-maps for us. */
    pl_swapchain_colorspace_hint(r->swapchain, &(struct pl_color_space){
        .primaries  = PL_COLOR_PRIM_BT_2020,
        .transfer   = PL_COLOR_TRC_PQ,
        .hdr = {
            .max_luma = 1000.0f,
            .min_luma = 0.005f,
        },
    });

    r->renderer = pl_renderer_create(r->pl_log, r->vulkan->gpu);
    if (!r->renderer) { LOG("GPU", "ERROR: pl_renderer_create"); return false; }
    /* Separate renderer instance for the SDR-to-intermediate pass.
     * Each pl_renderer caches its compiled shaders against the specific
     * (source, target, params) signature. Reusing one renderer for both
     * HDR and SDR passes caused 50-100 ms of shader-LUT regeneration
     * every frame as it ping-ponged between the two configurations. */
    r->renderer_sdr = pl_renderer_create(r->pl_log, r->vulkan->gpu);
    if (!r->renderer_sdr) { LOG("GPU", "ERROR: pl_renderer_create (sdr)"); return false; }

    renderer_update_display_state(r);
    LOG("SWAP", "swapchain ready, HDR signaling %s",
        r->display_hdr_capable ? "ACTIVE" : "off (display in SDR mode)");
    return true;
}

/* ------------------------------------------------------------------ */
/* Diagonal-mask compositor.                                          */
/*                                                                    */
/* For the smooth diagonal split, we render the SDR pass into a       */
/* swapchain-sized RGBA texture whose alpha channel was pre-filled    */
/* by the CPU with an anti-aliased diagonal mask. libplacebo's        */
/* blend_params let us write RGB while preserving the dst alpha.      */
/* The texture then composites over the HDR pass as an overlay,       */
/* alpha-blended per pixel = smooth diagonal, zero stair-stepping.    */
/* ------------------------------------------------------------------ */
/* Per-pixel alpha value for a given mask mode at normalized (nx, ny). */
static inline float alpha_for_mask(int mask_mode, float nx, float ny, float aa)
{
    switch (mask_mode) {
    case ALPHA_MASK_FULL: return 1.0f;
    case ALPHA_MASK_LR:   return nx < 0.5f ? 0.0f : 1.0f;
    case ALPHA_MASK_TB:   return ny < 0.5f ? 0.0f : 1.0f;
    case ALPHA_MASK_DIAG: {
        float d = nx + ny - 1.0f;
        float t = (d + aa) / (2.0f * aa);
        if (t < 0) t = 0; if (t > 1) t = 1;
        return t * t * (3.0f - 2.0f * t);   /* smoothstep */
    }
    default: return 0.0f;
    }
}

static void ensure_diag_tex(Renderer *r, int w, int h, int mask_mode)
{
    bool size_changed = !r->diag_tex || r->diag_tex_w != w || r->diag_tex_h != h;
    bool mask_changed = r->diag_tex_mask_mode != mask_mode;
    if (!size_changed && !mask_changed) return;

    if (size_changed) {
        if (r->diag_tex) pl_tex_destroy(r->vulkan->gpu, &r->diag_tex);
        r->diag_tex_w = w;
        r->diag_tex_h = h;
        pl_fmt fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba16hf");
        if (!fmt) fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba8");
        r->diag_tex = pl_tex_create(r->vulkan->gpu, pl_tex_params(
            .w              = w,
            .h              = h,
            .format         = fmt,
            .sampleable     = true,
            .renderable     = true,
            .host_writable  = true,
            .blit_dst       = true,
        ));
        if (!r->diag_tex) { LOG("REND", "ensure_diag_tex: alloc failed"); return; }
    }
    r->diag_tex_mask_mode = mask_mode;

    pl_fmt fmt = r->diag_tex->params.format;
    bool is_half = (fmt && strcmp(fmt->name, "rgba16hf") == 0);
    size_t pixel_size = is_half ? 8 : 4;
    uint8_t *buf = calloc((size_t)w * h, pixel_size);
    if (!buf) return;

    float aa = 2.0f / (float)(w + h);
    for (int y = 0; y < h; y++) {
        float ny = (float)y / (float)h;
        for (int x = 0; x < w; x++) {
            float nx = (float)x / (float)w;
            float a = alpha_for_mask(mask_mode, nx, ny, aa);

            size_t idx = ((size_t)y * w + x) * 4;
            if (is_half) {
                uint16_t *p16 = (uint16_t *)&buf[idx * 2];
                union { float f; uint32_t u; } v;
                v.f = a;
                uint32_t sign = (v.u >> 31) & 0x1;
                int32_t  exp  = ((v.u >> 23) & 0xFF) - 127 + 15;
                uint32_t mant = v.u & 0x7FFFFF;
                uint16_t h16;
                if (exp <= 0)        h16 = (uint16_t)(sign << 15);
                else if (exp >= 31)  h16 = (uint16_t)((sign << 15) | (0x1F << 10));
                else                 h16 = (uint16_t)((sign << 15) | (exp << 10) | (mant >> 13));
                p16[0] = p16[1] = p16[2] = 0;
                p16[3] = h16;
            } else {
                buf[idx + 0] = 0;
                buf[idx + 1] = 0;
                buf[idx + 2] = 0;
                buf[idx + 3] = (uint8_t)(a * 255.0f + 0.5f);
            }
        }
    }

    pl_tex_upload(r->vulkan->gpu, pl_tex_transfer_params(
        .tex = r->diag_tex,
        .ptr = buf,
    ));
    free(buf);
    const char *mname =
        mask_mode == ALPHA_MASK_FULL ? "FULL" :
        mask_mode == ALPHA_MASK_LR   ? "LR"   :
        mask_mode == ALPHA_MASK_TB   ? "TB"   :
        mask_mode == ALPHA_MASK_DIAG ? "DIAG" : "?";
    LOG("REND", "diag_tex (%dx%d, %s) mask=%s uploaded",
        w, h, fmt ? fmt->name : "?", mname);
}

/* ------------------------------------------------------------------ */
/* Render SDR content into diag_tex (which already carries the mask). */
/* Used by the unified SDR/SPLIT pipeline below.                       */
/* ------------------------------------------------------------------ */
static bool render_sdr_to_intermediate(
    Renderer *r,
    const struct pl_frame *src_image,
    int tw, int th,
    int mask_mode,
    float sdr_peak,
    const struct pl_render_params *rp_base)
{
    ensure_diag_tex(r, tw, th, mask_mode);
    if (!r->diag_tex) return false;

    struct pl_frame sdr_target = {
        .num_planes = 1,
        .planes = {{
            .texture           = r->diag_tex,
            .components        = 4,
            .component_mapping = { 0, 1, 2, 3 },
        }},
        .crop  = { 0, 0, (float)tw, (float)th },
        .repr  = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    sdr_target.color.hdr.max_luma = sdr_peak;
    sdr_target.color.hdr.min_luma = 0.005f;

    struct pl_render_params rp_sdr = *rp_base;
    static const struct pl_blend_params keep_alpha = {
        .src_rgb   = PL_BLEND_ONE,
        .dst_rgb   = PL_BLEND_ZERO,
        .src_alpha = PL_BLEND_ZERO,
        .dst_alpha = PL_BLEND_ONE,
    };
    rp_sdr.blend_params = &keep_alpha;

    /* Enable inverse tone mapping so SDR-source content (peak ~100
     * nits) is expanded UP to our SDR target peak (~500 nits) instead
     * of being preserved at 100 nits. Without this, an SDR file in
     * SDR mode displays at ~100 absolute nits while QuickTime's same
     * file displays at ~500 nits (macOS SDR layer compositing applies
     * EDR boost). Inverse tone-mapping replicates that boost. For HDR
     * source content (peak 10000), this flag is a no-op — libplacebo
     * still tone-maps down to the target ceiling. */
    static struct pl_color_map_params sdr_color_map;
    sdr_color_map = pl_color_map_default_params;
    sdr_color_map.inverse_tone_mapping = true;
    rp_sdr.color_map_params = &sdr_color_map;

    LOGV("REND", "SDR→intermediate %dx%d (mask=%d): src.max=%.0fn  tgt.max=%.0fn  inverse=on",
         tw, th, mask_mode,
         src_image->color.hdr.max_luma, sdr_target.color.hdr.max_luma);
    return pl_render_image(r->renderer_sdr, src_image, &sdr_target, &rp_sdr);
}

/* Build an overlay pointing at diag_tex, sized to `dst_rect`. Caller
 * owns the storage for the returned overlay + part. */
static void make_sdr_overlay(
    Renderer *r,
    struct pl_overlay *out_overlay,
    struct pl_overlay_part *out_part,
    int tw, int th,
    struct pl_rect2df dst_rect)
{
    *out_part = (struct pl_overlay_part){
        .src = { 0, 0, (float)tw, (float)th },
        .dst = dst_rect,
    };
    *out_overlay = (struct pl_overlay){
        .tex   = r->diag_tex,
        .mode  = PL_OVERLAY_NORMAL,
        .parts = out_part,
        .num_parts = 1,
        .repr  = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
}

/* ------------------------------------------------------------------ */
/* Helpers to apply a colorspace + crop to the target frame.          */
/* The swapchain is always HDR-capable; we synthesize SDR by clamping */
/* the target's peak luminance and switching the transfer/primaries,  */
/* so libplacebo's tone-mapper does the heavy lifting for us.         */
/* ------------------------------------------------------------------ */
static void apply_hdr_target(struct pl_frame *t, float headroom)
{
    t->color.primaries  = PL_COLOR_PRIM_BT_2020;
    t->color.transfer   = PL_COLOR_TRC_PQ;
    t->color.hdr.max_luma = 203.0f * headroom;
    t->color.hdr.min_luma = 0.005f;
}

static void apply_sdr_target(struct pl_frame *t, float peak_nits)
{
    /* CRUCIAL: do NOT override primaries OR transfer. Both must match
     * the swapchain's actual colorspace, otherwise libplacebo's encoded
     * pixels get misinterpreted by the OS compositor.
     *
     * peak_nits is the SDR ceiling in absolute nits. 100 = strict
     * BT.2100 spec (very dim on most setups), 203 = Apple HDR Video
     * reference, 500 = Apple Display preset, etc. See renderer.h. */
    t->color.hdr.max_luma = peak_nits;
    t->color.hdr.min_luma = 0.005f;
}

/* Compute the SDR peak we'll use this frame. Caller responsibility
 * to record it in r->sdr_peak_effective for the HUD. */
static float compute_sdr_peak(const Renderer *r)
{
    if (r->sdr_peak_override > 0.0f) return r->sdr_peak_override;
    /* OS-tracked default: 500 nits × SDL3's SDR_WHITE_LEVEL multiplier.
     *
     * We default to ~500 nits — NOT the BT.2408 spec of 203 — because
     * macOS's own SDR layer composition (QuickTime, Safari, Finder)
     * applies an EDR brightness boost so SDR content sits at the
     * panel's "SDR reference white", which is typically 400–600 nits
     * on HDR-enabled Macs. Rendering spec-SDR (100 or 203 nits) on the
     * same panel looks distinctly dimmer than what users see in any
     * native app, which makes the HDR-vs-SDR comparison misleading.
     *
     * 500 nits matches macOS perceptual SDR roughly; override with
     *   --sdr-peak 100   strict BT.2100 spec
     *   --sdr-peak 203   BT.2408 HDR Video diffuse-white reference
     *   --sdr-peak 800   Apple Display preset / bright SDR
     */
    float white = r->display_sdr_white > 0.01f ? r->display_sdr_white : 1.0f;
    return 500.0f * white;
}

/* Narrow the target's destination rect to a sub-band along one axis.
 * (a0..a1) are normalized fractions [0,1] of the full extent on the
 * chosen axis. Used to put HDR and SDR renders into adjacent halves
 * of the same swapchain image. */
static void set_crop_axis(struct pl_frame *t, int axis_is_y, float a0, float a1)
{
    if (axis_is_y) {
        float h = t->crop.y1 - t->crop.y0;
        float base_y0 = t->crop.y0;
        t->crop.y0 = base_y0 + h * a0;
        t->crop.y1 = base_y0 + h * a1;
    } else {
        float w = t->crop.x1 - t->crop.x0;
        float base_x0 = t->crop.x0;
        t->crop.x0 = base_x0 + w * a0;
        t->crop.x1 = base_x0 + w * a1;
    }
}

/* ------------------------------------------------------------------ */
/* Per-frame render                                                    */
/* ------------------------------------------------------------------ */
bool renderer_render_avframe(Renderer *r, AVFrame *avframe)
{
    renderer_update_display_state(r);

    struct pl_swapchain_frame sf;
    if (!pl_swapchain_start_frame(r->swapchain, &sf)) {
        LOG("SWAP", "skip frame: swapchain not ready");
        return true; /* not fatal — try next frame */
    }

    struct pl_frame image = {0};
    if (!pl_map_avframe_ex(r->vulkan->gpu, &image, pl_avframe_params(
            .frame = avframe,
            .tex   = r->plane_tex,
    ))) {
        LOG("REND", "pl_map_avframe_ex failed");
        pl_swapchain_submit_frame(r->swapchain);
        return false;
    }

    /* Reset per-frame info before render. */
    r->last_num_passes = 0;
    r->last_tonemap[0] = 0;
    /* Source colorspace summary, for the on-screen HUD. */
    snprintf(r->last_source_csp, sizeof(r->last_source_csp),
             "src: prim=%d trc=%d peak=%.0fn",
             image.color.primaries, image.color.transfer,
             image.color.hdr.max_luma);

    /* Luminance probe — sample source pixel if the user has the probe
     * active. Done before render so HUD has fresh values to display. */
    if (r->probe_active && r->probe_x >= 0 && r->probe_y >= 0 &&
        r->probe_win_w > 0 && r->probe_win_h > 0 && avframe)
    {
        int sx = (int)((double)r->probe_x / r->probe_win_w  * avframe->width);
        int sy = (int)((double)r->probe_y / r->probe_win_h * avframe->height);
        ProbeResult pr;
        if (probe_sample(avframe, sx, sy, &pr)) {
            r->probe_nits   = pr.luma_nits;
            r->probe_y_norm = pr.y_norm;
            r->probe_r_nits = pr.r_nits;
            r->probe_g_nits = pr.g_nits;
            r->probe_b_nits = pr.b_nits;
        } else {
            r->probe_nits = NAN;
        }
    }

    struct pl_render_params rp = pl_render_default_params;
    rp.info_callback = pl_info_cb;
    rp.info_priv     = r;

    /* Build the target frame(s) based on current mode. SPLIT mode does
     * two pl_render_image calls into halves of the same swapchain image. */
    struct pl_frame target;
    pl_frame_from_swapchain(&target, &sf);
    struct pl_frame base_target = target;  /* save original crop */

    /* Prepare HUD overlays for THIS frame. Critical: overlays must be
     * attached to `target` BEFORE pl_render_image is called — libplacebo
     * composites them during the render pass. Attaching after = invisible. */
    int win_w = (int)(base_target.crop.x1 - base_target.crop.x0);
    int win_h = (int)(base_target.crop.y1 - base_target.crop.y0);
    HudOverlays hud_ov;
    hud_prepare(r, win_w, win_h, &hud_ov);

    /* Resolve effective SDR peak once per frame for HUD readback. */
    r->sdr_peak_effective = compute_sdr_peak(r);
    const float sdr_peak = r->sdr_peak_effective;

    const char *mode_name = "?";
    switch (r->mode) {
    case HDRPLAY_MODE_HDR:
        mode_name = "HDR";
        apply_hdr_target(&target, r->display_hdr_headroom);
        target.overlays     = &hud_ov.status;
        target.num_overlays = 1;
        if (!pl_render_image(r->renderer, &image, &target, &rp))
            LOG("REND", "pl_render_image (HDR) failed");
        break;

    case HDRPLAY_MODE_SDR:
    case HDRPLAY_MODE_SPLIT: {
        /* Unified path: render HDR full-frame to swapchain with the
         * SDR-intermediate attached as an overlay. The overlay's alpha
         * mask is selected per mode (FULL = solid SDR, LR/TB = hard
         * half-split, DIAG = smooth diagonal).
         *
         * WHY this is the same path for all SDR-related modes: libplacebo's
         * overlay compositor blends overlay pixels in the overlay's
         * declared color space WITHOUT going through the tone-mapping
         * pipeline. That preserves the absolute PQ-encoded brightness
         * (203 nits encoded as PQ ≈ 0.578) that we wrote into the
         * intermediate. Rendering the intermediate as a *source* via
         * pl_render_image, by contrast, runs the full tone-map pipeline
         * which renormalizes brightness back to panel peak — and that
         * was the reason SDR mode and LR/TB split looked identical to
         * HDR mode. Diagonal worked from day one only because it was
         * the first thing that happened to use the overlay path. */
        int sw = (int)(base_target.crop.x1 - base_target.crop.x0);
        int sh_ = (int)(base_target.crop.y1 - base_target.crop.y0);

        int mask_mode;
        if (r->mode == HDRPLAY_MODE_SDR) {
            mode_name = "SDR";   mask_mode = ALPHA_MASK_FULL;
        } else if (r->split_orient == HDRPLAY_SPLIT_LR) {
            mode_name = "SPLIT-LR";   mask_mode = ALPHA_MASK_LR;
        } else if (r->split_orient == HDRPLAY_SPLIT_TB) {
            mode_name = "SPLIT-TB";   mask_mode = ALPHA_MASK_TB;
        } else {
            mode_name = "SPLIT-DIAG"; mask_mode = ALPHA_MASK_DIAG;
        }

        /* 1. Render SDR into the intermediate (carries mode's alpha mask). */
        if (!render_sdr_to_intermediate(r, &image, sw, sh_, mask_mode, sdr_peak, &rp))
            LOG("REND", "render_sdr_to_intermediate failed");

        /* 2. Build the SDR overlay descriptor (covers full swapchain). */
        struct pl_overlay  sdr_ov;
        struct pl_overlay_part sdr_part;
        if (r->diag_tex)
            make_sdr_overlay(r, &sdr_ov, &sdr_part, sw, sh_, base_target.crop);

        /* 3. Render HDR full-frame to swapchain with the SDR overlay
         *    riding on top. Status + HDR + SDR badges all attach to
         *    this single render too — clipped to the swapchain crop. */
        target = base_target;
        apply_hdr_target(&target, r->display_hdr_headroom);

        struct pl_overlay ov_arr[4];
        int n = 0;
        ov_arr[n++] = hud_ov.status;
        if (r->mode == HDRPLAY_MODE_SPLIT) {
            ov_arr[n++] = hud_ov.hdr_label;
            ov_arr[n++] = hud_ov.sdr_label;
        }
        if (r->diag_tex) ov_arr[n++] = sdr_ov;

        target.overlays     = ov_arr;
        target.num_overlays = n;
        if (!pl_render_image(r->renderer, &image, &target, &rp))
            LOG("REND", "pl_render_image (composite) failed");
        break;
    }
    }

    /* Short form so the HUD doesn't crop. In HDR mode we override the
     * swapchain target's peak directly; in SDR / SPLIT modes the actual
     * peak control happens in the intermediate render, so we log the
     * effective sdr_peak alongside the panel/swapchain peak for clarity. */
    if (r->mode == HDRPLAY_MODE_HDR) {
        snprintf(r->last_output_csp, sizeof(r->last_output_csp),
                 "out: HDR peak=%.0fn", target.color.hdr.max_luma);
    } else if (r->mode == HDRPLAY_MODE_SDR) {
        snprintf(r->last_output_csp, sizeof(r->last_output_csp),
                 "out: SDR peak=%.0fn", r->sdr_peak_effective);
    } else {
        snprintf(r->last_output_csp, sizeof(r->last_output_csp),
                 "out: %s HDR=%.0fn SDR=%.0fn", mode_name,
                 203.0f * r->display_hdr_headroom, r->sdr_peak_effective);
    }

    pl_unmap_avframe(r->vulkan->gpu, &image);
    if (!pl_swapchain_submit_frame(r->swapchain))
        LOG("SWAP", "submit_frame failed");

    pl_swapchain_swap_buffers(r->swapchain);

    LOGV("REND", "frame: passes=%d mode=%s tonemap=\"%s\" target=%s",
         r->last_num_passes, mode_name, r->last_tonemap, r->last_output_csp);
    return true;
}

void renderer_close(Renderer *r)
{
    if (r->vulkan) {
        for (int i = 0; i < 4; i++)
            if (r->plane_tex[i]) pl_tex_destroy(r->vulkan->gpu, &r->plane_tex[i]);
        if (r->diag_tex) pl_tex_destroy(r->vulkan->gpu, &r->diag_tex);
    }
    if (r->renderer)     pl_renderer_destroy(&r->renderer);
    if (r->renderer_sdr) pl_renderer_destroy(&r->renderer_sdr);
    if (r->swapchain) pl_swapchain_destroy(&r->swapchain);
    if (r->vulkan)    pl_vulkan_destroy(&r->vulkan);
    if (r->vk_inst)   pl_vk_inst_destroy(&r->vk_inst);
    if (r->pl_log)    pl_log_destroy(&r->pl_log);
    if (r->window)    SDL_DestroyWindow(r->window);
    SDL_Quit();
}
