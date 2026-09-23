#include "renderer.h"
#include "clamp.h"
#include "hud.h"
#include "log.h"
#include "probe.h"
#include "stats.h"
#include "source.h"
#include "ambient.h"

#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libplacebo/shaders/custom.h>

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

static void renderer_update_ambient_sensor(Renderer *r)
{
    if (r->ambient_lux_override > 0.0f) {
        r->ambient_lux_current = r->ambient_lux_override;
        r->ambient_sensor_available = false;
        return;
    }

    uint64_t now = SDL_GetTicks();
    if (now < r->ambient_next_poll_ms) return;
    r->ambient_next_poll_ms = now + 1000;

    float lux = 0.0f;
    r->ambient_sensor_available = ambient_sensor_read_lux(&lux);
    if (r->ambient_sensor_available)
        r->ambient_lux_current = lux;
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
    /* Separate renderer instance for EVERY intermediate render, SDR or
     * HDR. Each pl_renderer caches its compiled shaders against the
     * specific (source, target, params) signature, so alternating one
     * renderer between two configurations regenerates shader LUTs — 50
     * to 100 ms per frame when this was first hit on the SDR pass.
     *
     * The split is by TARGET, not by colour treatment. Intermediates
     * render into an RGBA texture with blend_params set (keep_alpha, so
     * the pre-baked mask survives); swapchain passes render without
     * blending. blend_params is part of that cached signature, and it is
     * the only thing stopping the render from overwriting the alpha
     * mask — so a renderer that ping-pongs between the two is not just
     * slow, it loses the mask, and the pane composites fully opaque.
     *
     * That was the two-file diagonal wipe: it was the sole user of the
     * HDR intermediate, which went through r->renderer and so shared an
     * instance with the swapchain pass. Source B came out opaque
     * everywhere and covered A completely. Single-file DIAG was fine
     * because its intermediate is SDR and already had a dedicated
     * renderer. Keep every intermediate on this one. */
    r->renderer_inter = pl_renderer_create(r->pl_log, r->vulkan->gpu);
    if (!r->renderer_inter) { LOG("GPU", "ERROR: pl_renderer_create (inter)"); return false; }
    r->renderer_hlg = pl_renderer_create(r->pl_log, r->vulkan->gpu);
    if (!r->renderer_hlg) { LOG("GPU", "ERROR: pl_renderer_create (HLG)"); return false; }
    r->renderer_hlg_out = pl_renderer_create(r->pl_log, r->vulkan->gpu);
    if (!r->renderer_hlg_out) {
        LOG("GPU", "ERROR: pl_renderer_create (HLG output)");
        return false;
    }
    r->dispatch_diff = pl_dispatch_create(r->pl_log, r->vulkan->gpu);
    if (!r->dispatch_diff) { LOG("GPU", "ERROR: pl_dispatch_create (diff)"); return false; }

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
        float t = fclamp01((d + aa) / (2.0f * aa));
        return t * t * (3.0f - 2.0f * t);   /* smoothstep */
    }
    default: return 0.0f;
    }
}

static bool rect_same(struct pl_rect2df a, struct pl_rect2df b)
{
    return fabsf(a.x0 - b.x0) < 0.51f && fabsf(a.y0 - b.y0) < 0.51f &&
           fabsf(a.x1 - b.x1) < 0.51f && fabsf(a.y1 - b.y1) < 0.51f;
}

static struct pl_hook_res ambient_contrast_hook(
    void *priv, const struct pl_hook_params *params)
{
    Renderer *r = priv;
    int width = (int)roundf(fabsf(params->rect.x1 - params->rect.x0));
    int height = (int)roundf(fabsf(params->rect.y1 - params->rect.y0));
    struct pl_shader_var vars[] = {
        {
            .var = pl_var_float("ambient_exponent"),
            .data = &r->ambient_shader_exponent,
            .dynamic = true,
        }, {
            .var = pl_var_float("ambient_peak_norm"),
            .data = &r->ambient_shader_peak_norm,
            .dynamic = true,
        },
    };
    static const char *header =
        "vec3 ambient_pq_eotf(vec3 e) {\n"
        "    const float m1 = 0.1593017578125;\n"
        "    const float m2 = 78.84375;\n"
        "    const float c1 = 0.8359375;\n"
        "    const float c2 = 18.8515625;\n"
        "    const float c3 = 18.6875;\n"
        "    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));\n"
        "    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));\n"
        "}\n"
        "vec3 ambient_pq_oetf(vec3 l) {\n"
        "    const float m1 = 0.1593017578125;\n"
        "    const float m2 = 78.84375;\n"
        "    const float c1 = 0.8359375;\n"
        "    const float c2 = 18.8515625;\n"
        "    const float c3 = 18.6875;\n"
        "    vec3 p = pow(clamp(l, 0.0, 1.0), vec3(m1));\n"
        "    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));\n"
        "}\n";
    static const char *body =
        "vec3 linear = ambient_pq_eotf(color.rgb);\n"
        "float y = max(dot(linear, vec3(0.2627, 0.6780, 0.0593)), 0.0);\n"
        "float relative_y = clamp(y / max(ambient_peak_norm, 1e-6), 0.0, 1.0);\n"
        "float scale = y > 1e-9\n"
        "    ? pow(max(relative_y, 1e-6), ambient_exponent - 1.0) : 0.0;\n"
        "color.rgb = ambient_pq_oetf(max(linear * scale, 0.0));\n";
    bool ok = pl_shader_custom(params->sh, &(struct pl_custom_shader){
        .header = header,
        .description = "hdrplay ambient contrast policy (non-standard)",
        .body = body,
        .input = PL_SHADER_SIG_COLOR,
        .output = PL_SHADER_SIG_COLOR,
        .variables = vars,
        .num_variables = 2,
        .output_w = width,
        .output_h = height,
    });
    return (struct pl_hook_res){
        .failed = !ok,
        .output = PL_HOOK_SIG_COLOR,
        .sh = params->sh,
        .repr = params->repr,
        .color = params->color,
        .components = params->components,
        .rect = params->rect,
    };
}

/* Freeze a selected HLG OOTF into an absolute PQ texture.
 *
 * pl_color_space_infer_map() intentionally tunes an HLG source to an HDR
 * destination by replacing source max_luma with destination max_luma. That
 * is the right automatic policy, but it also means changing only the source
 * metadata has no visible effect. The first pass makes the requested L_W the
 * destination of the HLG conversion. A second texture pass fits that absolute
 * PQ result to the current output peak before overlay composition. */
static float compute_sdr_peak(const Renderer *r);

static bool prepare_hlg_policy(Renderer *r, int si, float peak,
                               float output_peak, float ambient_exponent)
{
    typeof(r->slot[0]) *sl = &r->slot[si];
    struct pl_frame *src = &sl->image;
    sl->render_image = *src;
    sl->hlg_peak_effective = 0.0f;

    bool ambient_active = fabsf(ambient_exponent - 1.0f) > 0.001f;
    if (src->color.transfer != PL_COLOR_TRC_HLG ||
        (r->hlg_peak_override <= 0.0f && !ambient_active))
        return true;

    int w = (int)fabsf(src->crop.x1 - src->crop.x0);
    int h = (int)fabsf(src->crop.y1 - src->crop.y0);
    if (w <= 0 || h <= 0) return false;

    if (!sl->hlg_tex || !sl->hlg_out_tex ||
        sl->hlg_w != w || sl->hlg_h != h) {
        if (sl->hlg_tex) pl_tex_destroy(r->vulkan->gpu, &sl->hlg_tex);
        if (sl->hlg_out_tex)
            pl_tex_destroy(r->vulkan->gpu, &sl->hlg_out_tex);
        pl_fmt fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba16hf");
        if (!fmt) {
            LOG("REND", "no RGBA16F format for HLG peak conversion");
            return false;
        }
        sl->hlg_tex = pl_tex_create(r->vulkan->gpu, pl_tex_params(
            .w          = w,
            .h          = h,
            .format     = fmt,
            .sampleable = true,
            .renderable = true,
            .blit_dst   = true,
        ));
        sl->hlg_out_tex = pl_tex_create(r->vulkan->gpu, pl_tex_params(
            .w          = w,
            .h          = h,
            .format     = fmt,
            .sampleable = true,
            .renderable = true,
            .blit_dst   = true,
        ));
        if (!sl->hlg_tex || !sl->hlg_out_tex) {
            LOG("REND", "HLG peak texture allocation failed");
            return false;
        }
        sl->hlg_w = w;
        sl->hlg_h = h;
    }

    struct pl_frame target = {
        .num_planes = 1,
        .planes = {{
            .texture           = sl->hlg_tex,
            .components        = 4,
            .component_mapping = { 0, 1, 2, 3 },
        }},
        .crop = { 0, 0, w, h },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    target.repr.alpha = PL_ALPHA_NONE;
    target.color.primaries = src->color.primaries;
    target.color.hdr.max_luma = peak;
    target.color.hdr.min_luma = PL_COLOR_HDR_BLACK;

    struct pl_render_params rp = pl_render_default_params;
    struct pl_hook ambient_hook = {
        .stages = PL_HOOK_PRE_OUTPUT,
        .input = PL_HOOK_SIG_COLOR,
        .priv = r,
        .hook = ambient_contrast_hook,
        .signature = UINT64_C(0x686472706c617578),
    };
    const struct pl_hook *ambient_hook_ptr = &ambient_hook;
    if (ambient_active) {
        r->ambient_shader_exponent = ambient_exponent;
        r->ambient_shader_peak_norm = peak / 10000.0f;
        rp.hooks = &ambient_hook_ptr;
        rp.num_hooks = 1;
    }
    if (!pl_render_image(r->renderer_hlg, src, &target, &rp)) {
        LOG("REND", "HLG peak conversion failed for source %d", si);
        return false;
    }

    /* Freeze L_W first, then fit that absolute-PQ result to the physical
     * output. The old direct overlay skipped this second operation, so PQ
     * values above the current EDR ceiling were left for the panel to clip. */
    struct pl_frame output = {
        .num_planes = 1,
        .planes = {{
            .texture           = sl->hlg_out_tex,
            .components        = 4,
            .component_mapping = { 0, 1, 2, 3 },
        }},
        .crop = { 0, 0, w, h },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    output.repr.alpha = PL_ALPHA_NONE;
    output.color.primaries = src->color.primaries;
    output.color.hdr.max_luma = output_peak;
    output.color.hdr.min_luma = PL_COLOR_HDR_BLACK;

    struct pl_render_params output_rp = pl_render_default_params;
    if (!pl_render_image(r->renderer_hlg_out, &target, &output, &output_rp)) {
        LOG("REND", "HLG display fit failed for source %d", si);
        return false;
    }
    sl->render_image = output;
    sl->hlg_peak_effective = peak;
    sl->hlg_output_peak = output_peak;
    if (!r->hlg_peak_logged[si]) {
        LOG("REND", "source %d HLG playback converted at %.0f nits, fitted to %.0f nits",
            si, peak, output_peak);
        r->hlg_peak_logged[si] = true;
    }
    return true;
}

/* `dst` is the window-space rect the image will actually occupy. Alpha
 * is forced to 0 outside it, which is what letterboxes the intermediate.
 *
 * Doing it here, in the mask, rather than leaving it to libplacebo's
 * border fill, is deliberate. The mask is uploaded ONCE and thereafter
 * only survives because render_to_intermediate blends with dst_alpha =
 * ONE. A border fill covering the letterbox region writes alpha there
 * and the mask never comes back — the pane then composites opaque and
 * hides everything under it. Owning the whole alpha channel here means
 * the render never has to touch a pixel outside `dst`. */
static void ensure_inter_tex(Renderer *r, int si, int w, int h, int mask_mode,
                             struct pl_rect2df dst)
{
    typeof(r->slot[0]) *sl = &r->slot[si];
    bool size_changed = !sl->inter_tex || sl->inter_w != w || sl->inter_h != h;
    bool mask_changed = sl->inter_mask != mask_mode;
    bool dst_changed  = !rect_same(sl->inter_dst, dst);
    if (!size_changed && !mask_changed && !dst_changed) return;

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
    sl->inter_dst  = dst;

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
            /* Outside the drawn rect there is no image, only whatever
             * the previous frame left behind. Make it invisible. */
            if ((float)x < dst.x0 || (float)x >= dst.x1 ||
                (float)y < dst.y0 || (float)y >= dst.y1)
                a = 0.0f;

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
/* On an HDR-capable swapchain we synthesize SDR by clamping the      */
/* target's peak luminance, so libplacebo's tone-mapper does the      */
/* heavy lifting. The swapchain is NOT always HDR-capable, though —   */
/* macOS/CoreAnimation always grants EDR, but a Linux compositor      */
/* without HDR negotiates plain BT.709 + sRGB. Pick the target to     */
/* match what the swapchain actually gave us; see apply_sdr_target.   */
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

/* Non-HDR swapchain (Linux/X11, or any compositor without HDR): keep the
 * colorspace pl_frame_from_swapchain already negotiated — BT.709 + sRGB —
 * and only declare the luminance range, so libplacebo tone-maps PQ down
 * into it. Overwriting the negotiated transfer with PQ is what made HDR
 * content look washed out: PQ codes landed in an sRGB buffer and the
 * display applied the sRGB EOTF to them, lifting every shadow. */
static void apply_sdr_target(const Renderer *r, struct pl_frame *t, float peak)
{
    t->color.hdr.max_luma = peak;
    t->color.hdr.min_luma = inter_min_luma(r, peak);
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
    struct pl_rect2df dst,
    int mask_mode,
    bool sdr,
    float sdr_peak,
    const struct pl_render_params *rp_base)
{
    ensure_inter_tex(r, si, tw, th, mask_mode, dst);
    pl_tex tex = r->slot[si].inter_tex;
    if (!tex) return false;

    /* The texture is window-sized and `dst` is in window space, so the
     * content lands at the same coordinates it will occupy on screen.
     * That 1:1 correspondence keeps the pre-baked alpha mask (also
     * window-space) aligned with the image, and lets the overlay blit
     * the whole texture to the whole window with no offset. */
    struct pl_frame target = {
        .num_planes = 1,
        .planes = {{
            .texture           = tex,
            .components        = 4,
            .component_mapping = { 0, 1, 2, 3 },
        }},
        .crop  = dst,
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
    /* The alpha mask outside `dst` is ours (see ensure_inter_tex) and a
     * border fill would overwrite it, permanently — the mask is uploaded
     * once, and only the keep_alpha blend below preserves it thereafter.
     * This was the regression that broke the HDR/SDR wipe: once the
     * target crop stopped covering the whole texture, the border fill
     * started running and the mask went away. */
    rp.border     = PL_CLEAR_SKIP;
    rp.background = PL_CLEAR_SKIP;
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
    return pl_render_image(r->renderer_inter,
                           src_image, &target, &rp);
}

/* Build an overlay pointing at a source's intermediate. */
static void make_inter_overlay(
    Renderer *r,
    int si,
    struct pl_overlay *out_overlay,
    struct pl_overlay_part *out_part,
    int win_w, int win_h,
    bool sdr,
    float sdr_peak)
{
    /* Whole texture -> whole window, 1:1. The texture is window-sized and
     * holds the image already positioned in window coordinates, with
     * alpha 0 everywhere it does not cover, so there is nothing to
     * rescale or offset here. Letterboxing lives in the mask, not in
     * this rect — which is what keeps this identical for one pane or
     * two, letterboxed or not. */
    *out_part = (struct pl_overlay_part){
        .src = { 0, 0, (float)win_w, (float)win_h },
        .dst = { 0, 0, (float)win_w, (float)win_h },
    };
    *out_overlay = (struct pl_overlay){
        .tex   = r->slot[si].inter_tex,
        .mode  = PL_OVERLAY_NORMAL,
        .parts = out_part,
        .num_parts = 1,
        .repr  = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    /* Do NOT set repr.alpha here. Declaring PL_ALPHA_INDEPENDENT looks
     * more correct than leaving it PL_ALPHA_UNKNOWN — the mask really is
     * a separate channel — but it changes how libplacebo composites and
     * broke the single-file HDR/SDR wipe that had worked for months.
     * The default is load-bearing. Leave it alone. */

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

/* Present the display-fitted HLG conversion as absolute PQ. Sending this
 * texture through the swapchain's ordinary image path would normalize it
 * again and erase the requested 500-vs-1000 nit distinction. Overlay
 * composition preserves the absolute light already established by the two
 * texture passes. SRC_FRAME coordinates make it follow the base image's crop,
 * scale and rotation. */
static void make_hlg_override_overlay(
    const Renderer *r,
    int si,
    struct pl_overlay *out_overlay,
    struct pl_overlay_part *out_part)
{
    const typeof(r->slot[0]) *sl = &r->slot[si];
    *out_part = (struct pl_overlay_part){
        .src = { 0, 0, (float)sl->hlg_w, (float)sl->hlg_h },
        .dst = { 0, 0, (float)sl->hlg_w, (float)sl->hlg_h },
    };
    *out_overlay = (struct pl_overlay){
        .tex    = sl->hlg_out_tex,
        .mode   = PL_OVERLAY_NORMAL,
        .coords = PL_OVERLAY_COORDS_SRC_FRAME,
        .parts  = out_part,
        .num_parts = 1,
        .repr   = pl_color_repr_rgb,
        .color  = pl_color_space_hdr10,
    };
    out_overlay->repr.alpha = PL_ALPHA_NONE;
    out_overlay->color.primaries = sl->image.color.primaries;
    out_overlay->color.hdr.max_luma = sl->hlg_output_peak;
    out_overlay->color.hdr.min_luma = PL_COLOR_HDR_BLACK;
}

/* ------------------------------------------------------------------ */
/* GPU frame difference (A/B or current/previous).                    */
/*                                                                    */
/* The two inputs are already display-referred BT.2020/BT.709 PQ in   */
/* window-sized intermediates. Decode PQ, subtract in linear display  */
/* light, amplify 4x, then encode PQ again for the HDR swapchain.      */
/* This makes black mean equal while retaining the colour of an RGB    */
/* error. Alpha limits the comparison to pixels covered by both files, */
/* so mismatched aspect ratios do not turn letterbox bars into signal. */
/* ------------------------------------------------------------------ */
static bool ensure_diff_tex(Renderer *r, int w, int h)
{
    if (r->diff_tex && r->diff_w == w && r->diff_h == h) return true;
    if (r->diff_tex) pl_tex_destroy(r->vulkan->gpu, &r->diff_tex);

    r->diff_w = w;
    r->diff_h = h;
    pl_fmt fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba16hf");
    if (!fmt) fmt = pl_find_named_fmt(r->vulkan->gpu, "rgba8");
    r->diff_tex = pl_tex_create(r->vulkan->gpu, pl_tex_params(
        .w          = w,
        .h          = h,
        .format     = fmt,
        .sampleable = true,
        .renderable = true,
    ));
    if (!r->diff_tex) {
        LOG("REND", "diff texture allocation failed");
        return false;
    }
    LOG("REND", "diff texture %dx%d (%s) allocated",
        w, h, fmt ? fmt->name : "?");
    return true;
}

static bool render_diff_texture(Renderer *r, int w, int h)
{
    if (!r->slot[0].inter_tex || !r->slot[1].inter_tex ||
        !ensure_diff_tex(r, w, h))
        return false;

    struct pl_shader_desc desc[] = {
        {
            .desc = { .name = "diff_a", .type = PL_DESC_SAMPLED_TEX },
            .binding = { .object = r->slot[0].inter_tex,
                         .address_mode = PL_TEX_ADDRESS_CLAMP,
                         .sample_mode = PL_TEX_SAMPLE_NEAREST },
        }, {
            .desc = { .name = "diff_b", .type = PL_DESC_SAMPLED_TEX },
            .binding = { .object = r->slot[1].inter_tex,
                         .address_mode = PL_TEX_ADDRESS_CLAMP,
                         .sample_mode = PL_TEX_SAMPLE_NEAREST },
        },
    };
    const float uv[4][2] = {
        { 0.0f, 0.0f }, { 1.0f, 0.0f },
        { 0.0f, 1.0f }, { 1.0f, 1.0f },
    };
    struct pl_shader_va va = {
        .attr = {
            .name = "diff_uv",
            .fmt = pl_find_vertex_fmt(r->vulkan->gpu, PL_FMT_FLOAT, 2),
        },
        .data = { uv[0], uv[1], uv[2], uv[3] },
    };
    if (!va.attr.fmt) {
        LOG("REND", "diff shader has no vec2 vertex format");
        return false;
    }

    static const char *header =
        "vec3 pq_eotf(vec3 e) {\n"
        "    const float m1 = 0.1593017578125;\n"
        "    const float m2 = 78.84375;\n"
        "    const float c1 = 0.8359375;\n"
        "    const float c2 = 18.8515625;\n"
        "    const float c3 = 18.6875;\n"
        "    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));\n"
        "    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));\n"
        "}\n"
        "vec3 pq_oetf(vec3 l) {\n"
        "    const float m1 = 0.1593017578125;\n"
        "    const float m2 = 78.84375;\n"
        "    const float c1 = 0.8359375;\n"
        "    const float c2 = 18.8515625;\n"
        "    const float c3 = 18.6875;\n"
        "    vec3 p = pow(clamp(l, 0.0, 1.0), vec3(m1));\n"
        "    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));\n"
        "}\n";
    static const char *body =
        "vec4 a = texture(diff_a, diff_uv);\n"
        "vec4 b = texture(diff_b, diff_uv);\n"
        "vec3 delta = min(abs(pq_eotf(a.rgb) - pq_eotf(b.rgb)) * 4.0, 1.0);\n"
        "color = vec4(pq_oetf(delta), 1.0);\n"
        "if (min(a.a, b.a) < 0.5) color.rgb = vec3(0.0);\n";

    pl_dispatch_reset_frame(r->dispatch_diff);
    pl_shader sh = pl_dispatch_begin(r->dispatch_diff);
    if (!sh) {
        LOG("REND", "diff shader allocation failed");
        return false;
    }
    if (!pl_shader_custom(sh, &(struct pl_custom_shader){
            .header = header,
            .description = "4x absolute linear-light frame difference",
            .body = body,
            .input = PL_SHADER_SIG_NONE,
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = desc,
            .num_descriptors = 2,
            .vertex_attribs = &va,
            .num_vertex_attribs = 1,
            .output_w = w,
            .output_h = h,
        })) {
        LOG("REND", "diff shader generation failed");
        pl_dispatch_abort(r->dispatch_diff, &sh);
        return false;
    }

    if (!pl_dispatch_finish(r->dispatch_diff, pl_dispatch_params(
            .shader = &sh,
            .target = r->diff_tex,
        ))) {
        LOG("REND", "diff shader dispatch failed");
        return false;
    }
    return true;
}

static void make_diff_overlay(Renderer *r, struct pl_overlay *out_overlay,
                              struct pl_overlay_part *out_part,
                              int win_w, int win_h, bool sdr, float sdr_peak)
{
    *out_part = (struct pl_overlay_part){
        .src = { 0, 0, (float)win_w, (float)win_h },
        .dst = { 0, 0, (float)win_w, (float)win_h },
    };
    *out_overlay = (struct pl_overlay){
        .tex = r->diff_tex,
        .mode = PL_OVERLAY_NORMAL,
        .parts = out_part,
        .num_parts = 1,
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    if (sdr) {
        out_overlay->color.primaries = PL_COLOR_PRIM_BT_709;
        out_overlay->color.hdr.max_luma = sdr_peak;
        out_overlay->color.hdr.min_luma = inter_min_luma(r, sdr_peak);
    } else {
        out_overlay->color.primaries = PL_COLOR_PRIM_BT_2020;
        out_overlay->color.hdr.max_luma = 203.0f * r->display_hdr_headroom;
        out_overlay->color.hdr.min_luma = 0.005f;
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

static struct pl_hook_res plane_hook(void *priv,
                                     const struct pl_hook_params *params)
{
    int component = *(const int *)priv;
    int width = (int)roundf(fabsf(params->rect.x1 - params->rect.x0));
    int height = (int)roundf(fabsf(params->rect.y1 - params->rect.y0));
    const char *body = component == 0 ? "color.rgb = color.rrr;" :
                       component == 1 ? "color.rgb = color.ggg;" :
                                        "color.rgb = color.bbb;";
    bool ok = pl_shader_custom(params->sh, &(struct pl_custom_shader){
        .description = "grayscale component view",
        .body = body,
        .input = PL_SHADER_SIG_COLOR,
        .output = PL_SHADER_SIG_COLOR,
        .output_w = width,
        .output_h = height,
    });

    struct pl_color_repr repr = params->repr;
    repr.sys = PL_COLOR_SYSTEM_RGB;
    repr.alpha = PL_ALPHA_NONE;
    repr.dovi = NULL;
    return (struct pl_hook_res){
        .failed = !ok,
        .output = PL_HOOK_SIG_COLOR,
        .sh = params->sh,
        .repr = repr,
        .color = params->color,
        .components = 3,
        .rect = params->rect,
    };
}

static struct pl_hook_res clipping_hook(void *priv,
                                        const struct pl_hook_params *params)
{
    Renderer *r = priv;
    int width = (int)roundf(fabsf(params->rect.x1 - params->rect.x0));
    int height = (int)roundf(fabsf(params->rect.y1 - params->rect.y0));
    struct pl_shader_var vars[] = {
        { .var = pl_var_float("clip_low"),
          .data = &r->clip_low_active, .dynamic = true },
        { .var = pl_var_float("clip_high"),
          .data = &r->clip_high_active, .dynamic = true },
        { .var = pl_var_float("clip_epsilon"),
          .data = &r->clip_epsilon_active, .dynamic = true },
    };
    static const char *body =
        "float y = color.r;\n"
        "float gray = clamp((y - clip_low) / max(clip_high - clip_low, 1e-6), 0.0, 1.0);\n"
        "color.rgb = vec3(gray);\n"
        "if (y >= clip_high - clip_epsilon) color.rgb = vec3(1.0, 0.0, 0.0);\n"
        "else if (y <= clip_low + clip_epsilon) color.rgb = vec3(0.0, 0.0, 1.0);\n";
    bool ok = pl_shader_custom(params->sh, &(struct pl_custom_shader){
        .description = "exact decoded-luma clipping view",
        .body = body,
        .input = PL_SHADER_SIG_COLOR,
        .output = PL_SHADER_SIG_COLOR,
        .variables = vars,
        .num_variables = 3,
        .output_w = width,
        .output_h = height,
    });

    struct pl_color_repr repr = pl_color_repr_rgb;
    repr.alpha = PL_ALPHA_NONE;
    return (struct pl_hook_res){
        .failed = !ok,
        .output = PL_HOOK_SIG_COLOR,
        .sh = params->sh,
        .repr = repr,
        .color = params->color,
        .components = 3,
        .rect = params->rect,
    };
}

static struct pl_hook_res plateau_hook(void *priv,
                                       const struct pl_hook_params *params)
{
    Renderer *r = priv;
    int width = (int)roundf(fabsf(params->rect.x1 - params->rect.x0));
    int height = (int)roundf(fabsf(params->rect.y1 - params->rect.y0));
    struct pl_shader_var vars[] = {
        { .var = pl_var_float("plateau_low"),
          .data = &r->plateau_low_active, .dynamic = true },
        { .var = pl_var_float("plateau_high"),
          .data = &r->plateau_high_active, .dynamic = true },
        { .var = pl_var_float("plateau_delta"),
          .data = &r->plateau_delta_active, .dynamic = true },
    };
    /* Derivatives compare the four neighboring fragments in a 2x2 quad.
     * Requiring the near-endpoint value and a small local slope avoids
     * flagging isolated hot pixels while still tolerating codec noise around
     * a plateau which was clipped before a later rescale or encode. */
    static const char *body =
        "float y = color.r;\n"
        "float local_delta = max(abs(dFdx(y)), abs(dFdy(y)));\n"
        "float gray = clamp(y, 0.0, 1.0);\n"
        "color.rgb = vec3(gray);\n"
        "if (y >= plateau_high && local_delta <= plateau_delta) color.rgb = vec3(1.0, 0.0, 0.0);\n"
        "else if (y <= plateau_low && local_delta <= plateau_delta) color.rgb = vec3(0.0, 0.0, 1.0);\n";
    bool ok = pl_shader_custom(params->sh, &(struct pl_custom_shader){
        .description = "near-endpoint luma plateau view",
        .body = body,
        .input = PL_SHADER_SIG_COLOR,
        .output = PL_SHADER_SIG_COLOR,
        .variables = vars,
        .num_variables = 3,
        .output_w = width,
        .output_h = height,
    });

    struct pl_color_repr repr = pl_color_repr_rgb;
    repr.alpha = PL_ALPHA_NONE;
    return (struct pl_hook_res){
        .failed = !ok,
        .output = PL_HOOK_SIG_COLOR,
        .sh = params->sh,
        .repr = repr,
        .color = params->color,
        .components = 3,
        .rect = params->rect,
    };
}

/* At NATIVE, libplacebo has already aligned and combined the decoded Y, Cb
 * and Cr textures but has not converted them to RGB. Replicating the selected
 * native component here gives true grayscale without copying a plane or
 * binding the same texture multiple times. Distinct signatures keep the
 * renderer's shader cache correct as C cycles between components. */
static const int plane_components[3] = { 0, 1, 2 };
static const struct pl_hook plane_hooks[3] = {
    {
        .stages = PL_HOOK_NATIVE,
        .input = PL_HOOK_SIG_COLOR,
        .priv = (void *)&plane_components[0],
        .hook = plane_hook,
        .signature = UINT64_C(0x687064706c616e59),
    }, {
        .stages = PL_HOOK_NATIVE,
        .input = PL_HOOK_SIG_COLOR,
        .priv = (void *)&plane_components[1],
        .hook = plane_hook,
        .signature = UINT64_C(0x687064706c614362),
    }, {
        .stages = PL_HOOK_NATIVE,
        .input = PL_HOOK_SIG_COLOR,
        .priv = (void *)&plane_components[2],
        .hook = plane_hook,
        .signature = UINT64_C(0x687064706c614372),
    },
};

static void select_clip_thresholds(Renderer *r, int si)
{
    bool legal = r->plane_view == HDRPLAY_PLANE_LEGAL;
    r->clip_low_active = legal ? r->legal_low[si] : r->clip_low[si];
    r->clip_high_active = legal ? r->legal_high[si] : r->clip_high[si];
    r->clip_epsilon_active = r->clip_epsilon[si];
}

static void select_plateau_thresholds(Renderer *r, int si)
{
    r->plateau_low_active = r->plateau_low[si];
    r->plateau_high_active = r->plateau_high[si];
    r->plateau_delta_active = r->plateau_delta[si];
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

bool renderer_window_to_source(const Renderer *r,
                               double x, double y,
                               int coordinate_w, int coordinate_h,
                               bool clamp_to_image,
                               int *source_x, int *source_y)
{
    if (!r || !r->focus_map_valid || coordinate_w <= 0 || coordinate_h <= 0)
        return false;
    double wx = x * r->focus_map_win_w / coordinate_w;
    double wy = y * r->focus_map_win_h / coordinate_h;
    return layout_window_to_source(r->focus_map_target, r->focus_map_image,
                                   r->focus_map_frame_w,
                                   r->focus_map_frame_h,
                                   r->focus_map_rotation,
                                   wx, wy, clamp_to_image,
                                   source_x, source_y);
}

bool renderer_render(Renderer *r, Source *sources, int n)
{
    if (n < 1) return false;
    if (n > 2) n = 2;
    renderer_update_display_state(r);
    renderer_update_ambient_sensor(r);

    struct pl_swapchain_frame sf;
    if (!pl_swapchain_start_frame(r->swapchain, &sf)) {
        LOG("SWAP", "skip frame: swapchain not ready");
        return true; /* not fatal — try next frame */
    }

    /* In one-file diff playback, slot 1 is a virtual source containing
     * the previous frame. Reusing the A/B slots here keeps color treatment,
     * plane views, zoom and the actual subtract shader exactly identical
     * between spatial and temporal comparisons. */
    bool temporal_diff = r->diff_view && n == 1 && sources[0].previous;
    bool ab_diff = r->diff_view && n > 1 && r->solo < 0;
    bool active_diff = temporal_diff || ab_diff;
    AVFrame *slot_frame[2] = { sources[0].shown, NULL };
    int slot_rotation[2] = { r->rotation[0], r->rotation[1] };
    int slot_count = n;
    if (n > 1) {
        slot_frame[1] = sources[1].shown;
    } else if (temporal_diff) {
        slot_frame[1] = sources[0].previous;
        slot_rotation[1] = r->rotation[0];
        slot_count = 2;
    }

    /* Map every source once up front. A pass and an intermediate can
     * both reference the same source, and DIAG maps both sources in one
     * swapchain frame, so per-pass mapping would either double-map or
     * unmap something still queued. */
    for (int i = 0; i < 2; i++) r->slot[i].mapped = false;
    for (int i = 0; i < slot_count; i++) {
        if (!slot_frame[i]) continue;
        if (!pl_map_avframe_ex(r->vulkan->gpu, &r->slot[i].image,
                pl_avframe_params(.frame = slot_frame[i],
                                  .tex   = r->slot[i].plane_tex))) {
            LOG("REND", "pl_map_avframe_ex failed for source %d", i);
            continue;
        }
        r->slot[i].mapped = true;

        /* Native-plane values have already been normalized for storage bit
         * shift by the time PL_HOOK_NATIVE runs. Reproduce libplacebo's
         * normalization here so the false-color thresholds select exactly
         * the legal luma codes, rather than an arbitrary near-white band. */
        struct pl_bit_encoding bits = r->slot[i].image.repr.bits;
        const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(slot_frame[i]->format);
        int color_bits = bits.color_depth;
        if (!color_bits && pixdesc) color_bits = pixdesc->comp[0].depth;
        if (!color_bits) color_bits = 8;
        int sample_bits = bits.sample_depth ? bits.sample_depth : color_bits;
        int bit_shift = bits.bit_shift > 0 ? bits.bit_shift : 0;
        double tex_max = ldexp(1.0, sample_bits) - 1.0;
        double scale = 1.0 / ldexp(1.0, bit_shift);
        bool full_range = pl_color_levels_guess(&r->slot[i].image.repr) ==
                          PL_COLOR_LEVELS_FULL;
        if (full_range)
            scale *= tex_max / (ldexp(1.0, color_bits) - 1.0);
        else
            scale *= ldexp(1.0, sample_bits - color_bits);
        double code_step = ldexp(1.0, bit_shift) / tex_max * scale;
        int code_scale = 1 << (color_bits > 8 ? color_bits - 8 : 0);
        r->clip_low[i] = 0.0f;
        r->clip_high[i] =
            (float)((ldexp(1.0, color_bits) - 1.0) * code_step);
        r->legal_low[i] = full_range ? r->clip_low[i]
                                     : (float)(16 * code_scale * code_step);
        r->legal_high[i] = full_range ? r->clip_high[i]
                                      : (float)(235 * code_scale * code_step);
        r->clip_epsilon[i] = (float)(0.25 * code_step);
        /* Search the outer 10% of the usable interval. For limited-range
         * input that means the legal interval; for full-range input it means
         * the complete code interval. Eight code values tolerate the small
         * ripples introduced when an already-clipped image is compressed. */
        float usable_low = full_range ? r->clip_low[i] : r->legal_low[i];
        float usable_high = full_range ? r->clip_high[i] : r->legal_high[i];
        float usable_span = usable_high - usable_low;
        r->plateau_low[i] = usable_low + 0.10f * usable_span;
        r->plateau_high[i] = usable_low + 0.90f * usable_span;
        r->plateau_delta[i] = (float)(8.0 * code_step);

        int source_index = temporal_diff && i == 1 ? 0 : i;
        const Decoder *dec = &sources[source_index].dec;
        const char *reference_source = "none";
        float reference_lux = 0.0f;
        if (r->ambient_reference_override > 0.0f) {
            reference_lux = r->ambient_reference_override;
            reference_source = "CLI";
        } else if (dec->has_ambient_viewing) {
            reference_lux = (float)dec->ambient_illuminance_lux;
            reference_source = "AMVE";
        } else if (r->ambient_lux_override > 0.0f) {
            /* An explicit viewing level must remain useful for untagged HLG.
             * 314 lux is Apple's documented default HLG capture environment,
             * not a value required by H.274 or BT.2100. Automatic sensor-only
             * adaptation stays disabled when neither AMVE nor a reference
             * override exists. */
            reference_lux = 314.0f;
            reference_source = "314 default";
        }
        float viewing_lux = r->ambient_lux_current;
        float ambient_exponent = ambient_contrast_exponent(reference_lux,
                                                            viewing_lux);
        float previous_ref = r->ambient_reference_effective[i];
        float previous_exp = r->ambient_exponent[i];
        r->ambient_reference_effective[i] = reference_lux;
        r->ambient_exponent[i] = ambient_exponent;

        if (reference_lux > 0.0f && viewing_lux > 0.0f &&
            (!r->ambient_logged[i] || fabsf(previous_ref - reference_lux) > 0.5f ||
             fabsf(previous_exp - ambient_exponent) > 0.01f))
        {
            LOG("AMBIENT", "source %d reference=%.0f lux (%s), viewing=%.0f lux (%s), exponent=%.3f [hdrplay policy, not standardized]",
                i, reference_lux,
                reference_source,
                viewing_lux,
                r->ambient_lux_override > 0.0f ? "CLI" : "sensor",
                ambient_exponent);
            r->ambient_logged[i] = true;
        } else if (reference_lux > 0.0f && viewing_lux <= 0.0f &&
                   !r->ambient_unavailable_logged) {
            LOG("AMBIENT", "reference %.0f lux available, but no ambient sensor reading; use --ambient-lux",
                reference_lux);
            r->ambient_unavailable_logged = true;
        }

        /* HLG is scene-referred. Its conversion to display light depends
         * on the nominal display peak L_W. The actual conversion is done
         * below, after all AVFrames have been mapped. */
        float hlg_peak = r->hlg_peak_override > 0.0f
                       ? r->hlg_peak_override
                       : r->slot[i].image.color.hdr.max_luma > 0.0f
                           ? r->slot[i].image.color.hdr.max_luma
                           : PL_COLOR_HLG_PEAK;
        if (r->hlg_peak_override > 0.0f &&
            r->slot[i].image.color.transfer == PL_COLOR_TRC_HLG)
            r->slot[i].image.color.hdr.max_luma = r->hlg_peak_override;

        float output_peak = r->display_hdr_capable
                          ? 203.0f * r->display_hdr_headroom
                          : compute_sdr_peak(r);
        if (!prepare_hlg_policy(r, i, hlg_peak, output_peak,
                                ambient_exponent))
            LOG("REND", "using automatic HLG mapping for source %d", i);

        /* Set on the image actually consumed downstream. HLG conversion is
         * deliberately unrotated and source-sized; view rotation belongs to
         * the ordinary presentation pass. */
        pl_rotation rotation =
            pl_rotation_normalize(slot_rotation[i] / 90);
        r->slot[i].image.rotation = rotation;
        r->slot[i].render_image.rotation = rotation;
    }

    int focus = renderer_focus_source(r);
    if (!r->slot[focus].mapped) {
        r->focus_map_valid = false;
        /* Nothing to draw from the focused source; still present so the
         * window does not freeze. */
        for (int i = 0; i < slot_count; i++)
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
    struct pl_hook clip_hook = {
        .stages = PL_HOOK_NATIVE,
        .input = PL_HOOK_SIG_COLOR,
        .priv = r,
        .hook = clipping_hook,
        .signature = UINT64_C(0x687064636c697000),
    };
    struct pl_hook plateau_hook_desc = {
        .stages = PL_HOOK_NATIVE,
        .input = PL_HOOK_SIG_COLOR,
        .priv = r,
        .hook = plateau_hook,
        .signature = UINT64_C(0x687064706c617400),
    };
    const struct pl_hook *plane_hook_ptr = NULL;
    if (r->plane_view != HDRPLAY_PLANE_COLOR) {
        if (r->plane_view == HDRPLAY_PLANE_PLATEAU)
            plane_hook_ptr = &plateau_hook_desc;
        else if (r->plane_view == HDRPLAY_PLANE_LEGAL ||
                 r->plane_view == HDRPLAY_PLANE_CLIP)
            plane_hook_ptr = &clip_hook;
        else
            plane_hook_ptr = &plane_hooks[r->plane_view - HDRPLAY_PLANE_Y];
        rp.hooks = &plane_hook_ptr;
        rp.num_hooks = 1;
        /* Preserve individual 4:2:0/4:2:2 chroma samples when zooming rather
         * than smoothing their boundaries before the diagnostic hook sees
         * them. */
        rp.plane_upscaler = &pl_filter_nearest;
    }

    struct pl_frame base_target;
    pl_frame_from_swapchain(&base_target, &sf);
    int win_w = (int)(base_target.crop.x1 - base_target.crop.x0);
    int win_h = (int)(base_target.crop.y1 - base_target.crop.y0);

    r->sdr_peak_effective = compute_sdr_peak(r);
    const float sdr_peak = r->sdr_peak_effective;

    /* Every compositing decision comes from here — see layout.c. */
    LayoutInput li = {
        .mode = r->mode, .orient = r->split_orient,
        .n_sources = n,
        .solo = r->solo, .swapped = r->swapped,
        .diff_view = active_diff,
        .win_w = win_w, .win_h = win_h,
        /* Filled in below: rotation swaps the axes, and layout has to see
         * the frame as displayed. */
        .src_w = { 0, 0 },
        .src_h = { 0, 0 },
        .zoom = r->zoom, .pan_x = r->pan_x, .pan_y = r->pan_y,
        .hud_hidden = r->hud_hidden,
        .session_panel = r->session_panel,
        .scope_visible = r->scope_view != HDRPLAY_SCOPE_OFF,
    };
    for (int i = 0; i < slot_count; i++) {
        if (!slot_frame[i]) continue;
        layout_rotated_dims(slot_rotation[i],
                            slot_frame[i]->width, slot_frame[i]->height,
                            &li.src_w[i], &li.src_h[i]);
    }

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

    for (int i = 0; i < plan.n_pass; i++)
        if (plan.pass[i].src == focus) { r->last_scale = plan.pass[i].scale; break; }

    const LayoutPass *focus_pass = NULL;
    for (int i = 0; i < plan.n_pass; i++)
        if (plan.pass[i].src == focus) { focus_pass = &plan.pass[i]; break; }
    r->focus_map_valid = focus_pass != NULL && sources[focus].shown != NULL;
    if (r->focus_map_valid) {
        r->focus_map_src = focus;
        r->focus_map_win_w = win_w;
        r->focus_map_win_h = win_h;
        r->focus_map_frame_w = sources[focus].shown->width;
        r->focus_map_frame_h = sources[focus].shown->height;
        r->focus_map_rotation = r->rotation[focus];
        r->focus_map_target = focus_pass->target_crop;
        r->focus_map_image = focus_pass->image_crop;
    }

    /* The probe maps a window coordinate back to a source pixel, so it
     * has to account for zoom/pan the same way the render does. */
    if (r->probe_active && r->probe_x >= 0 && r->probe_y >= 0 &&
        r->probe_win_w > 0 && r->probe_win_h > 0)
    {
        AVFrame *pf = sources[focus].shown;

        int sx, sy;
        if (!renderer_window_to_source(r,
                                       r->probe_x, r->probe_y,
                                       r->probe_win_w, r->probe_win_h,
                                       false, &sx, &sy)) {
            /* Over a letterbox bar or the other pane — no source pixel
             * there. Reporting the nearest edge pixel instead would be a
             * confident answer about something not on screen. */
            r->probe_nits = NAN;
        } else {
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
    }

    HudOverlays hud_ov;
    hud_prepare(r, sources, n, &plan, win_w, win_h, &hud_ov);

    /* 1. Intermediates first — they are inputs to the passes below. */
    for (int i = 0; i < plan.n_inter; i++) {
        int si = plan.inter[i].src;
        if (si < 0 || si >= slot_count || !r->slot[si].mapped) continue;
        /* Same crop the corresponding pass uses, or the intermediate
         * would draw the whole frame into a rect sized for a cropped
         * one — i.e. stretch by exactly the zoom factor. */
        struct pl_frame img = r->plane_view == HDRPLAY_PLANE_COLOR
                            ? r->slot[si].render_image
                            : r->slot[si].image;
        if (r->plane_view == HDRPLAY_PLANE_LEGAL ||
            r->plane_view == HDRPLAY_PLANE_CLIP)
            select_clip_thresholds(r, si);
        else if (r->plane_view == HDRPLAY_PLANE_PLATEAU)
            select_plateau_thresholds(r, si);
        if (!rect_is_zero(plan.inter[i].image_crop))
            img.crop = to_pl_rect(plan.inter[i].image_crop);
        if (!render_to_intermediate(r, si, &img, win_w, win_h,
                                    to_pl_rect(plan.inter[i].dst),
                                    plan.inter[i].mask, plan.inter[i].sdr,
                                    sdr_peak, &rp))
            LOG("REND", "render_to_intermediate(%d) failed", si);
        else
            LOGV("REND", "inter[%d] src=%d mask=%d sdr=%d dst=[%.0f,%.0f %.0fx%.0f]",
                 i, si, plan.inter[i].mask, plan.inter[i].sdr,
                 plan.inter[i].dst.x0, plan.inter[i].dst.y0,
                 plan.inter[i].dst.x1 - plan.inter[i].dst.x0,
                 plan.inter[i].dst.y1 - plan.inter[i].dst.y0);
    }

    bool diff_ready = false;
    if (active_diff) {
        diff_ready = r->slot[0].mapped && r->slot[1].mapped &&
                     render_diff_texture(r, win_w, win_h);
        if (!diff_ready)
            LOG("REND", "difference view unavailable for this frame");
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
        if (lp->src < 0 || lp->src >= slot_count || !r->slot[lp->src].mapped) continue;

        struct pl_frame image  = r->slot[lp->src].image;
        if (r->plane_view == HDRPLAY_PLANE_LEGAL ||
            r->plane_view == HDRPLAY_PLANE_CLIP)
            select_clip_thresholds(r, lp->src);
        else if (r->plane_view == HDRPLAY_PLANE_PLATEAU)
            select_plateau_thresholds(r, lp->src);
        struct pl_frame target = base_target;
        target.crop = to_pl_rect(lp->target_crop);
        if (r->display_hdr_capable)
            apply_hdr_target(&target, r->display_hdr_headroom);
        else
            apply_sdr_target(r, &target, sdr_peak);

        /* libplacebo rotates AFTER cropping, so image_crop stays in the
         * frame's own unrotated pixels and layout needs no rotation
         * awareness beyond the dimension swap fed into LayoutInput. */
        if (!rect_is_zero(lp->image_crop))
            image.crop = to_pl_rect(lp->image_crop);

        int n_ov = 0;
        if (r->plane_view == HDRPLAY_PLANE_COLOR &&
            r->slot[lp->src].hlg_tex &&
            r->slot[lp->src].hlg_out_tex &&
            r->slot[lp->src].hlg_peak_effective > 0.0f &&
            r->slot[lp->src].image.color.transfer == PL_COLOR_TRC_HLG)
        {
            make_hlg_override_overlay(r, lp->src,
                                      &ov_store[n_ov], &ov_parts[n_ov]);
            n_ov++;
        }
        for (int oi = 0; oi < lp->n_ov && n_ov < LAYOUT_MAX_OVERLAYS; oi++) {
            const LayoutOverlay *ov = &lp->ov[oi];
            switch (ov->kind) {
            case LAYOUT_OV_INTERMEDIATE: {
                int si = ov->src;
                if (si < 0 || si >= slot_count || !r->slot[si].inter_tex) break;
                bool sdr = false;
                for (int k = 0; k < plan.n_inter; k++)
                    if (plan.inter[k].src == si) sdr = plan.inter[k].sdr;
                make_inter_overlay(r, si, &ov_store[n_ov], &ov_parts[n_ov],
                                   win_w, win_h, sdr, sdr_peak);
                n_ov++;
                break;
            }
            case LAYOUT_OV_DIFF:
                if (!diff_ready) break;
                make_diff_overlay(r, &ov_store[n_ov], &ov_parts[n_ov],
                                  win_w, win_h,
                                  r->mode == HDRPLAY_MODE_SDR, sdr_peak);
                n_ov++;
                break;
            case LAYOUT_OV_STATUS:
                if (hud_ov.has_status)  ov_store[n_ov++] = hud_ov.status;
                break;
            case LAYOUT_OV_SESSION:
                if (hud_ov.has_session) ov_store[n_ov++] = hud_ov.session;
                break;
            case LAYOUT_OV_SCOPE:
                if (hud_ov.has_scope) ov_store[n_ov++] = hud_ov.scope;
                break;
            case LAYOUT_OV_SCOPE_ROI:
                if (hud_ov.has_scope_roi)
                    ov_store[n_ov++] = hud_ov.scope_roi;
                break;
            case LAYOUT_OV_PLANE:
                if (hud_ov.has_plane)   ov_store[n_ov++] = hud_ov.plane;
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

    for (int i = 0; i < slot_count; i++)
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
            if (r->slot[s].hlg_tex)
                pl_tex_destroy(r->vulkan->gpu, &r->slot[s].hlg_tex);
            if (r->slot[s].hlg_out_tex)
                pl_tex_destroy(r->vulkan->gpu, &r->slot[s].hlg_out_tex);
        }
        if (r->diff_tex)
            pl_tex_destroy(r->vulkan->gpu, &r->diff_tex);
        hud_close(r->vulkan->gpu);
    }
    if (r->renderer)     pl_renderer_destroy(&r->renderer);
    if (r->renderer_inter) pl_renderer_destroy(&r->renderer_inter);
    if (r->renderer_hlg) pl_renderer_destroy(&r->renderer_hlg);
    if (r->renderer_hlg_out) pl_renderer_destroy(&r->renderer_hlg_out);
    if (r->dispatch_diff) pl_dispatch_destroy(&r->dispatch_diff);
    if (r->swapchain) pl_swapchain_destroy(&r->swapchain);
    if (r->vulkan)    pl_vulkan_destroy(&r->vulkan);
    if (r->vk_inst)   pl_vk_inst_destroy(&r->vk_inst);
    if (r->pl_log)    pl_log_destroy(&r->pl_log);
    if (r->window)    SDL_DestroyWindow(r->window);
    SDL_Quit();
}
