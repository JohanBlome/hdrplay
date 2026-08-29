#include "renderer.h"
#include "hud.h"
#include "log.h"
#include "probe.h"
#include "stats.h"
#include "source.h"

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
    /* Clamp the initial window to fit the display, preserving aspect.
     *
     * Sizing the window to the source is fine for 1080p landscape, but a
     * 1728x2304 portrait clip produces a window taller than the screen —
     * and with HIGH_PIXEL_DENSITY that becomes a ~16 megapixel backing
     * surface. Every render pass, and every border clear, then covers 16
     * MP; with two panes that is enough to drop a fast GPU to single-
     * digit frame rates. Resizing afterwards is still free. */
    {
        SDL_DisplayID d = target_display ? target_display : SDL_GetPrimaryDisplay();
        SDL_Rect usable = {0};
        if (d && SDL_GetDisplayUsableBounds(d, &usable) &&
            usable.w > 0 && usable.h > 0 && width > 0 && height > 0)
        {
            /* Leave a little room so the window is not flush to the edges. */
            int max_w = (int)(usable.w * 0.9f);
            int max_h = (int)(usable.h * 0.9f);
            if (width > max_w || height > max_h) {
                float sx = (float)max_w / width;
                float sy = (float)max_h / height;
                float sc = sx < sy ? sx : sy;
                int nw = (int)(width * sc), nh = (int)(height * sc);
                if (nw < 320) nw = 320;
                if (nh < 240) nh = 240;
                LOG("GPU", "window %dx%d exceeds usable %dx%d — opening at %dx%d",
                    width, height, usable.w, usable.h, nw, nh);
                width = nw; height = nh;
            }
        }
    }

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

static void ensure_inter_tex(Renderer *r, int si, int w, int h, int mask_mode)
{
    typeof(r->slot[0]) *sl = &r->slot[si];
    bool size_changed = !sl->inter_tex || sl->inter_w != w || sl->inter_h != h;
    bool mask_changed = sl->inter_mask != mask_mode;
    if (!size_changed && !mask_changed) return;

    if (size_changed) {
        if (sl->inter_tex) pl_tex_destroy(r->vulkan->gpu, &sl->inter_tex);
        sl->inter_w = w;
        sl->inter_h = h;
        pl_fmt fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba16hf");
        if (!fmt) fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba8");
        sl->inter_tex = pl_tex_create(r->vulkan->gpu, pl_tex_params(
            .w              = w,
            .h              = h,
            .format         = fmt,
            .sampleable     = true,
            .renderable     = true,
            .host_writable  = true,
            .blit_dst       = true,
        ));
        if (!sl->inter_tex) { LOG("REND", "ensure_inter_tex: alloc failed"); return; }
    }
    sl->inter_mask = mask_mode;

    pl_fmt fmt = sl->inter_tex->params.format;
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
        .tex = sl->inter_tex,
        .ptr = buf,
    ));
    free(buf);
    const char *mname =
        mask_mode == ALPHA_MASK_FULL ? "FULL" :
        mask_mode == ALPHA_MASK_LR   ? "LR"   :
        mask_mode == ALPHA_MASK_TB   ? "TB"   :
        mask_mode == ALPHA_MASK_DIAG ? "DIAG" : "?";
    LOG("REND", "inter_tex[%d] (%dx%d, %s) mask=%s uploaded",
        si, w, h, fmt ? fmt->name : "?", mname);
}

/* ------------------------------------------------------------------ */
/* Helpers to apply a colorspace to the target frame.                 */
/* The swapchain is always HDR-capable; we synthesize SDR by clamping */
/* the target's peak luminance, so libplacebo's tone-mapper does the  */
/* heavy lifting.                                                     */
/* ------------------------------------------------------------------ */
static void apply_hdr_target(struct pl_frame *t, float headroom)
{
    t->color.primaries  = PL_COLOR_PRIM_BT_2020;
    t->color.transfer   = PL_COLOR_TRC_PQ;
    t->color.hdr.max_luma = 203.0f * headroom;
    t->color.hdr.min_luma = 0.005f;
}

static float compute_sdr_peak(const Renderer *r)
{
    if (r->sdr_peak_override > 0.0f) return r->sdr_peak_override;
    /* Track the OS SDR-white reference. SDL reports it normalized
     * (1.0 = SDR baseline), so scale by the BT.2100 100-nit reference
     * and by the headroom the compositor is currently granting. */
    float white = r->display_sdr_white > 0.0f ? r->display_sdr_white : 1.0f;
    float peak  = white * 100.0f * (r->display_hdr_headroom > 0.0f
                                    ? r->display_hdr_headroom : 1.0f);
    if (peak < 100.0f)  peak = 100.0f;
    if (peak > 1000.0f) peak = 1000.0f;
    return peak;
}

static float inter_min_luma(const Renderer *r, float peak)
{
    float cap = r->sdr_dr_stops_cap > 0.0f ? r->sdr_dr_stops_cap : 12.0f;
    return peak / powf(2.0f, cap);
}

/* ------------------------------------------------------------------ */
/* Render a source into its intermediate (which already carries the   */
/* alpha mask).                                                        */
/*                                                                     */
/* `sdr` selects the treatment. SDR is the original use — gamut-map to */
/* BT.709 and clamp the peak — and is why the intermediate exists at   */
/* all: overlay composition bypasses tone-mapping, so absolute         */
/* PQ-encoded brightness survives to the panel instead of being        */
/* renormalized to swapchain peak (RENDERING.md §6.6).                 */
/*                                                                     */
/* The HDR variant is new, and only used for the second source in a    */
/* two-file DIAG wipe, where the pane boundary is not a rectangle and  */
/* therefore cannot be a target crop.                                  */
/* ------------------------------------------------------------------ */
static bool render_to_intermediate(
    Renderer *r,
    int si,
    const struct pl_frame *src_image,
    int tw, int th,
    int mask_mode,
    bool sdr,
    float sdr_peak,
    const struct pl_render_params *rp_base)
{
    ensure_inter_tex(r, si, tw, th, mask_mode);
    pl_tex tex = r->slot[si].inter_tex;
    if (!tex) return false;

    struct pl_frame target = {
        .num_planes = 1,
        .planes = {{
            .texture           = tex,
            .components        = 4,
            .component_mapping = { 0, 1, 2, 3 },
        }},
        .crop  = { 0, 0, (float)tw, (float)th },
        .repr  = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };

    if (sdr) {
        /* Target primaries = BT.709 so libplacebo gamut-maps wide-gamut
         * BT.2020 source colors into the BT.709 volume during render.
         * Still PQ-encoded with max_luma = sdr_peak so the overlay path
         * carries it to the HDR swapchain; only the gamut changes. */
        target.color.primaries    = PL_COLOR_PRIM_BT_709;
        target.color.hdr.max_luma = sdr_peak;
        /* SDR's contrast ceiling. libplacebo compresses sub-floor source
         * detail up to this floor, so the pane crushes shadows the way a
         * real SDR display has to. See --sdr-dr-stops. */
        target.color.hdr.min_luma = inter_min_luma(r, sdr_peak);
    } else {
        apply_hdr_target(&target, r->display_hdr_headroom);
    }

    struct pl_render_params rp = *rp_base;
    static const struct pl_blend_params keep_alpha = {
        .src_rgb   = PL_BLEND_ONE,
        .dst_rgb   = PL_BLEND_ZERO,
        .src_alpha = PL_BLEND_ZERO,
        .dst_alpha = PL_BLEND_ONE,
    };
    rp.blend_params = &keep_alpha;

    static struct pl_color_map_params sdr_color_map;
    static struct pl_color_adjustment  sdr_adj;
    if (sdr) {
        sdr_color_map = pl_color_map_default_params;
        /* Inverse tone-mapping expands an SDR source (peak ~100 nits)
         * UP to the SDR target peak, replicating the EDR boost macOS
         * applies when compositing an SDR layer. Without it an SDR file
         * shows at ~100 absolute nits while QuickTime shows the same
         * file at ~500. For HDR source content this is a no-op. It is
         * also what the HUD's SDR BOOST line reports on. */
        sdr_color_map.inverse_tone_mapping = true;
        /* Wide-gamut -> BT.709. Perceptual (BT.2407 rolloff) by default,
         * approximating what an SDR display would actually show for
         * BT.2020 source. See --sdr-gamut-map. */
        if (r->sdr_gamut_map)
            sdr_color_map.gamut_mapping = r->sdr_gamut_map;
        rp.color_map_params = &sdr_color_map;

        /* libplacebo's tone-map desaturates perceptually to keep hue
         * stable across brightness changes, which reads flatter than
         * macOS's SDR layer. Counter with a saturation adjustment. */
        sdr_adj = pl_color_adjustment_neutral;
        sdr_adj.saturation = r->sdr_saturation > 0.0f ? r->sdr_saturation : 1.0f;
        rp.color_adjustment = &sdr_adj;
    }

    LOGV("REND", "%s->intermediate[%d] %dx%d (mask=%d): src.max=%.0fn tgt.max=%.0fn",
         sdr ? "SDR" : "HDR", si, tw, th, mask_mode,
         src_image->color.hdr.max_luma, target.color.hdr.max_luma);
    return pl_render_image(sdr ? r->renderer_sdr : r->renderer,
                           src_image, &target, &rp);
}

/* Build an overlay pointing at a source's intermediate. */
static void make_inter_overlay(
    Renderer *r,
    int si,
    struct pl_overlay *out_overlay,
    struct pl_overlay_part *out_part,
    int tw, int th,
    struct pl_rect2df dst_rect,
    bool sdr,
    float sdr_peak)
{
    *out_part = (struct pl_overlay_part){
        .src = { 0, 0, (float)tw, (float)th },
        .dst = dst_rect,
    };
    *out_overlay = (struct pl_overlay){
        .tex   = r->slot[si].inter_tex,
        .mode  = PL_OVERLAY_NORMAL,
        .parts = out_part,
        .num_parts = 1,
        .repr  = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    /* CRITICAL: pl_color_space_hdr10 leaves max_luma=0, which kicks
     * libplacebo's overlay compositor into a heuristic that renormalizes
     * our intermediate's PQ codes against an assumed max (usually the
     * HDR10 spec 10000 nits). That decouples the encoded brightness from
     * what lands on the panel — --sdr-peak 500/800/1500 would all look
     * the same. Declare exactly the range the intermediate was rendered
     * against so the codes pass through 1:1. These MUST mirror
     * render_to_intermediate's target. See RENDERING.md §6.6. */
    if (sdr) {
        out_overlay->color.hdr.max_luma = sdr_peak;
        out_overlay->color.hdr.min_luma = inter_min_luma(r, sdr_peak);
        out_overlay->color.primaries    = PL_COLOR_PRIM_BT_709;
    } else {
        out_overlay->color.hdr.max_luma = 203.0f * r->display_hdr_headroom;
        out_overlay->color.hdr.min_luma = 0.005f;
        out_overlay->color.primaries    = PL_COLOR_PRIM_BT_2020;
    }
}

/* ------------------------------------------------------------------ */
static struct pl_rect2df to_pl_rect(LayoutRect r)
{
    return (struct pl_rect2df){ r.x0, r.y0, r.x1, r.y1 };
}

static bool rect_is_zero(LayoutRect r)
{
    return r.x0 == 0 && r.y0 == 0 && r.x1 == 0 && r.y1 == 0;
}

/* Which source the HUD, probe and statistics should describe. In a
 * two-file comparison that is the left/top pane; when soloed it is
 * whichever file is on screen. */
int renderer_focus_source(const Renderer *r)
{
    if (r->n_sources < 2) return 0;
    if (r->solo >= 0) return r->solo;
    return r->swapped ? 1 : 0;
}

bool renderer_render(Renderer *r, Source *sources, int n)
{
    if (n < 1) return false;
    if (n > 2) n = 2;
    renderer_update_display_state(r);

    struct pl_swapchain_frame sf;
    if (!pl_swapchain_start_frame(r->swapchain, &sf)) {
        LOG("SWAP", "skip frame: swapchain not ready");
        return true; /* not fatal — try next frame */
    }

    /* Map every source once up front. A pass and an intermediate can
     * both reference the same source, and DIAG maps both sources in one
     * swapchain frame, so per-pass mapping would either double-map or
     * unmap something still queued. */
    for (int i = 0; i < n; i++) {
        r->slot[i].mapped = false;
        if (!sources[i].shown) continue;
        if (!pl_map_avframe_ex(r->vulkan->gpu, &r->slot[i].image,
                pl_avframe_params(.frame = sources[i].shown,
                                  .tex   = r->slot[i].plane_tex))) {
            LOG("REND", "pl_map_avframe_ex failed for source %d", i);
            continue;
        }
        r->slot[i].mapped = true;
    }

    int focus = renderer_focus_source(r);
    if (!r->slot[focus].mapped) {
        /* Nothing to draw from the focused source; still present so the
         * window does not freeze. */
        for (int i = 0; i < n; i++)
            if (r->slot[i].mapped)
                pl_unmap_avframe(r->vulkan->gpu, &r->slot[i].image);
        pl_swapchain_submit_frame(r->swapchain);
        return true;
    }

    /* Reset per-frame info before render. */
    r->last_num_passes = 0;
    r->last_tonemap[0] = 0;
    snprintf(r->last_source_csp, sizeof(r->last_source_csp),
             "src: prim=%d trc=%d peak=%.0fn",
             r->slot[focus].image.color.primaries,
             r->slot[focus].image.color.transfer,
             r->slot[focus].image.color.hdr.max_luma);

    /* Frame statistics and the session accumulator are updated by
     * source_advance_to / source_step_*, which own the decode. Mirror
     * the focused source's into the renderer for the HUD. */
    r->frame_stats       = sources[focus].frame_stats;
    r->frame_stats_valid = sources[focus].frame_stats_valid;
    r->session           = &sources[focus].session;
    r->current_frame_no  = sources[focus].frame_no;

    struct pl_render_params rp = pl_render_default_params;
    rp.info_callback = pl_info_cb;
    rp.info_priv     = r;

    struct pl_frame base_target;
    pl_frame_from_swapchain(&base_target, &sf);
    int win_w = (int)(base_target.crop.x1 - base_target.crop.x0);
    int win_h = (int)(base_target.crop.y1 - base_target.crop.y0);

    r->sdr_peak_effective = compute_sdr_peak(r);
    const float sdr_peak = r->sdr_peak_effective;

    /* Every compositing decision comes from here — see layout.c. */
    LayoutInput li = {
        .mode = r->mode, .orient = r->split_orient,
        .n_sources = n, .solo = r->solo, .swapped = r->swapped,
        .win_w = win_w, .win_h = win_h,
        .src_w = { sources[0].shown ? sources[0].shown->width  : 0,
                   n > 1 && sources[1].shown ? sources[1].shown->width  : 0 },
        .src_h = { sources[0].shown ? sources[0].shown->height : 0,
                   n > 1 && sources[1].shown ? sources[1].shown->height : 0 },
        .zoom = r->zoom, .pan_x = r->pan_x, .pan_y = r->pan_y,
        .hud_hidden = r->hud_hidden,
        .session_panel = r->session_panel,
    };
    LayoutPlan plan;
    layout_plan(&li, &plan);

    /* Log the plan whenever it changes. A mis-planned layout otherwise
     * presents as a blank pane with no clue why. */
    static char last_plan[192];
    char plan_desc[192];
    int  pd = snprintf(plan_desc, sizeof(plan_desc), "%s: %d pass, %d inter",
                       plan.name, plan.n_pass, plan.n_inter);
    for (int i = 0; i < plan.n_pass && pd < (int)sizeof(plan_desc); i++)
        pd += snprintf(plan_desc + pd, sizeof(plan_desc) - pd,
                       " | src%d->[%.0f,%.0f %.0fx%.0f]", plan.pass[i].src,
                       plan.pass[i].target_crop.x0, plan.pass[i].target_crop.y0,
                       plan.pass[i].target_crop.x1 - plan.pass[i].target_crop.x0,
                       plan.pass[i].target_crop.y1 - plan.pass[i].target_crop.y0);
    if (strcmp(plan_desc, last_plan) != 0) {
        LOG("REND", "layout %s", plan_desc);
        snprintf(last_plan, sizeof(last_plan), "%s", plan_desc);
    }

    /* The probe maps a window coordinate back to a source pixel, so it
     * has to account for zoom/pan the same way the render does. */
    if (r->probe_active && r->probe_x >= 0 && r->probe_y >= 0 &&
        r->probe_win_w > 0 && r->probe_win_h > 0)
    {
        AVFrame *pf = sources[focus].shown;
        LayoutRect ic = plan.pass[0].image_crop;
        double fx = (double)r->probe_x / r->probe_win_w;
        double fy = (double)r->probe_y / r->probe_win_h;
        int sx, sy;
        if (rect_is_zero(ic)) {
            sx = (int)(fx * pf->width);
            sy = (int)(fy * pf->height);
        } else {
            sx = (int)(ic.x0 + fx * (ic.x1 - ic.x0));
            sy = (int)(ic.y0 + fy * (ic.y1 - ic.y0));
        }
        ProbeResult pr;
        if (probe_sample(pf, sx, sy, &pr)) {
            r->probe_nits   = pr.luma_nits;
            r->probe_y_norm = pr.y_norm;
            r->probe_r_nits = pr.r_nits;
            r->probe_g_nits = pr.g_nits;
            r->probe_b_nits = pr.b_nits;
        } else {
            r->probe_nits = NAN;
        }
    }

    HudOverlays hud_ov;
    hud_prepare(r, sources, n, &plan, win_w, win_h, &hud_ov);

    /* 1. Intermediates first — they are inputs to the passes below. */
    for (int i = 0; i < plan.n_inter; i++) {
        int si = plan.inter[i].src;
        if (si < 0 || si >= n || !r->slot[si].mapped) continue;
        if (!render_to_intermediate(r, si, &r->slot[si].image, win_w, win_h,
                                    plan.inter[i].mask, plan.inter[i].sdr,
                                    sdr_peak, &rp))
            LOG("REND", "render_to_intermediate(%d) failed", si);
    }

    /* 2. Swapchain passes.
     *
     * With more than one pass the border handling has to change. By
     * default libplacebo fills everything in the target OUTSIDE the
     * image with `border` (PL_CLEAR_COLOR), which for a half-window
     * target.crop means the second pass wipes the first one's half —
     * pane A renders, then pane B blanks it. Clear the frame once
     * ourselves and tell every pass to skip its own border fill.
     *
     * Single-pass rendering deliberately keeps the default, so
     * single-file playback still letterboxes exactly as before. */
    if (plan.n_pass > 1) {
        static const float black[3] = { 0.0f, 0.0f, 0.0f };
        pl_frame_clear(r->vulkan->gpu, &base_target, black);
        rp.border = PL_CLEAR_SKIP;
    }

    struct pl_overlay      ov_store[LAYOUT_MAX_OVERLAYS];
    struct pl_overlay_part ov_parts[LAYOUT_MAX_OVERLAYS];

    for (int pi = 0; pi < plan.n_pass; pi++) {
        const LayoutPass *lp = &plan.pass[pi];
        if (lp->src < 0 || lp->src >= n || !r->slot[lp->src].mapped) continue;

        struct pl_frame image  = r->slot[lp->src].image;
        struct pl_frame target = base_target;
        target.crop = to_pl_rect(lp->target_crop);
        apply_hdr_target(&target, r->display_hdr_headroom);

        if (!rect_is_zero(lp->image_crop))
            image.crop = to_pl_rect(lp->image_crop);

        int n_ov = 0;
        for (int oi = 0; oi < lp->n_ov && n_ov < LAYOUT_MAX_OVERLAYS; oi++) {
            const LayoutOverlay *ov = &lp->ov[oi];
            switch (ov->kind) {
            case LAYOUT_OV_INTERMEDIATE: {
                int si = ov->src;
                if (si < 0 || si >= n || !r->slot[si].inter_tex) break;
                bool sdr = false;
                for (int k = 0; k < plan.n_inter; k++)
                    if (plan.inter[k].src == si) sdr = plan.inter[k].sdr;
                make_inter_overlay(r, si, &ov_store[n_ov], &ov_parts[n_ov],
                                   win_w, win_h, to_pl_rect(ov->dst),
                                   sdr, sdr_peak);
                n_ov++;
                break;
            }
            case LAYOUT_OV_STATUS:
                if (hud_ov.has_status)  ov_store[n_ov++] = hud_ov.status;
                break;
            case LAYOUT_OV_SESSION:
                if (hud_ov.has_session) ov_store[n_ov++] = hud_ov.session;
                break;
            case LAYOUT_OV_LABEL_A:
                if (hud_ov.has_label_a) ov_store[n_ov++] = hud_ov.label_a;
                break;
            case LAYOUT_OV_LABEL_B:
                if (hud_ov.has_label_b) ov_store[n_ov++] = hud_ov.label_b;
                break;
            }
        }

        target.overlays     = n_ov ? ov_store : NULL;
        target.num_overlays = n_ov;

        if (!pl_render_image(r->renderer, &image, &target, &rp))
            LOG("REND", "pl_render_image (%s pass %d) failed", plan.name, pi);
    }

    for (int i = 0; i < n; i++)
        if (r->slot[i].mapped)
            pl_unmap_avframe(r->vulkan->gpu, &r->slot[i].image);

    snprintf(r->last_output_csp, sizeof(r->last_output_csp),
             "out: %s peak=%.0fn passes=%d",
             plan.name, r->mode == HDRPLAY_MODE_HDR
                        ? 203.0f * r->display_hdr_headroom : sdr_peak,
             r->last_num_passes);

    if (!pl_swapchain_submit_frame(r->swapchain)) {
        LOG("SWAP", "submit_frame failed");
        return false;
    }
    pl_swapchain_swap_buffers(r->swapchain);
    return true;
}

void renderer_close(Renderer *r)
{
    if (r->vulkan) {
        /* Per-slot: plane uploads and the compositing intermediate. */
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < 4; i++)
                if (r->slot[s].plane_tex[i])
                    pl_tex_destroy(r->vulkan->gpu, &r->slot[s].plane_tex[i]);
            if (r->slot[s].inter_tex)
                pl_tex_destroy(r->vulkan->gpu, &r->slot[s].inter_tex);
        }
        hud_close(r->vulkan->gpu);
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
