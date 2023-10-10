/* Example video player based on ffmpeg. Designed to expose every libplacebo
 * option for testing purposes. Not a serious video player, no real error
 * handling. Simply infinitely loops its input.
 *
 * License: CC0 / Public Domain
 */

#include <stdatomic.h>

#include <libavutil/cpu.h>
#include <libavutil/file.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "common.h"
#include "window.h"
#include "pl_clock.h"
#include "pl_thread.h"

#ifdef PL_HAVE_WIN32
#include <shlwapi.h>
#define PL_BASENAME PathFindFileNameA
#define strdup _strdup
#else
#include <libgen.h>
#define PL_BASENAME basename
#endif

#ifdef HAVE_NUKLEAR
#include "ui.h"
#else
struct ui;
static void ui_destroy(struct ui **ui) {}
static bool ui_draw(struct ui *ui, const struct pl_swapchain_frame *frame) { return true; };
#endif

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/frame_queue.h>

#define MAX_FRAME_PASSES 256
#define MAX_BLEND_PASSES 8
#define MAX_BLEND_FRAMES 8

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct plplay {
    struct window *win;
    struct ui *ui;

    // libplacebo
    pl_log log;
    pl_renderer renderer;
    pl_queue queue;

    // libav*
    AVFormatContext *format;
    AVCodecContext *codec;
    const AVStream *stream; // points to first video stream of `format`
    pl_thread decoder_thread;
    bool decoder_thread_created;
    bool exit_thread;

    // settings / ui state
    const struct pl_filter_preset *upscaler, *downscaler, *plane_scaler, *frame_mixer;
    struct pl_render_params params;
    struct pl_deband_params deband_params;
    struct pl_sigmoid_params sigmoid_params;
    struct pl_color_adjustment color_adjustment;
    struct pl_peak_detect_params peak_detect_params;
    struct pl_color_map_params color_map_params;
    struct pl_dither_params dither_params;
    struct pl_deinterlace_params deinterlace_params;
    struct pl_distort_params distort_params;
    struct pl_icc_params icc_params;
    struct pl_cone_params cone_params;
    struct pl_icc_profile target_icc;
    char *target_icc_name;
    pl_rotation target_rot;
    bool colorspace_hint;
    bool colorspace_hint_dynamic;
    bool ignore_dovi;
    bool toggle_fullscreen;
    bool software_decoding;

    bool target_override; // if false, fields below are ignored
    struct pl_color_repr force_repr;
    enum pl_color_primaries force_prim;
    enum pl_color_transfer force_trc;
    struct pl_hdr_metadata force_hdr;
    bool force_hdr_enable;
    bool use_icc_luma;

    // custom shaders
    const struct pl_hook **shader_hooks;
    char **shader_paths;
    size_t shader_num;
    size_t shader_size;

    // pass metadata
    struct pl_dispatch_info blend_info[MAX_BLEND_FRAMES][MAX_BLEND_PASSES];
    struct pl_dispatch_info frame_info[MAX_FRAME_PASSES];
    int num_frame_passes;
    int num_blend_passes[MAX_BLEND_FRAMES];

    // playback statistics
    struct {
        _Atomic uint32_t decoded;
        uint32_t rendered;
        uint32_t mapped;
        uint32_t dropped;
        uint32_t stalled;
        double stalled_ms;
        double current_pts;

        struct timing {
            double sum, sum2, peak;
            uint64_t count;
        } acquire, update, render, draw_ui, submit, swap,
          vsync_interval, pts_interval;
    } stats;
};

static inline void log_time(struct timing *t, double ts)
{
    t->sum += ts;
    t->sum2 += ts * ts;
    t->peak = fmax(t->peak, ts);
    t->count++;
}

static void uninit(struct plplay *p)
{
    if (p->decoder_thread_created) {
        p->exit_thread = true;
        pl_queue_push(p->queue, NULL); // Signal EOF to wake up thread
        pl_thread_join(p->decoder_thread);
    }

    pl_queue_destroy(&p->queue);
    pl_renderer_destroy(&p->renderer);

    for (int i = 0; i < p->shader_num; i++) {
        pl_mpv_user_shader_destroy(&p->shader_hooks[i]);
        free(p->shader_paths[i]);
    }

    for (int i = 0; i < MAX_FRAME_PASSES; i++)
        pl_shader_info_deref(&p->frame_info[i].shader);
    for (int j = 0; j < MAX_BLEND_FRAMES; j++) {
        for (int i = 0; i < MAX_BLEND_PASSES; i++)
            pl_shader_info_deref(&p->blend_info[j][i].shader);
    }

    free(p->shader_hooks);
    free(p->shader_paths);
    free(p->target_icc_name);
    av_file_unmap((void *) p->target_icc.data, p->target_icc.len);

    // Free this before destroying the window to release associated GPU buffers
    avcodec_free_context(&p->codec);
    avformat_free_context(p->format);

    ui_destroy(&p->ui);
    window_destroy(&p->win);

    pl_log_destroy(&p->log);
    memset(p, 0, sizeof(*p));
}

static bool open_file(struct plplay *p, const char *filename)
{
    printf("Opening file: '%s'\n", filename);
    if (avformat_open_input(&p->format, filename, NULL, NULL) != 0) {
        fprintf(stderr, "libavformat: Failed opening file!\n");
        return false;
    }

    printf("Format: %s\n", p->format->iformat->name);

    if (p->format->duration != AV_NOPTS_VALUE)
        printf("Duration: %.3f s\n", p->format->duration / 1e6);

    if (avformat_find_stream_info(p->format,  NULL) < 0) {
        fprintf(stderr, "libavformat: Failed finding stream info!\n");
        return false;
    }

    // Find "best" video stream
    int stream_idx =
        av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (stream_idx < 0) {
        fprintf(stderr, "plplay: File contains no video streams?\n");
        return false;
    }

    const AVStream *stream = p->format->streams[stream_idx];
    const AVCodecParameters *par = stream->codecpar;
    printf("Found video track (stream %d)\n", stream_idx);
    printf("Resolution: %d x %d\n", par->width, par->height);

    if (stream->avg_frame_rate.den && stream->avg_frame_rate.num)
        printf("FPS: %f\n", av_q2d(stream->avg_frame_rate));

    if (stream->r_frame_rate.den && stream->r_frame_rate.num)
        printf("TBR: %f\n", av_q2d(stream->r_frame_rate));

    if (stream->time_base.den && stream->time_base.num)
        printf("TBN: %f\n", av_q2d(stream->time_base));

    if (par->bit_rate)
        printf("Bitrate: %"PRIi64" kbps\n", par->bit_rate / 1000);

    printf("Format: %s\n", av_get_pix_fmt_name(par->format));

    p->stream = stream;
    return true;
}

static bool init_codec(struct plplay *p)
{
    assert(p->stream);
    assert(p->win->gpu);

    const AVCodec *codec = avcodec_find_decoder(p->stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "libavcodec: Failed finding matching codec\n");
        return false;
    }

    p->codec = avcodec_alloc_context3(codec);
    if (!p->codec) {
        fprintf(stderr, "libavcodec: Failed allocating codec\n");
        return false;
    }

    if (avcodec_parameters_to_context(p->codec, p->stream->codecpar) < 0) {
        fprintf(stderr, "libavcodec: Failed copying codec parameters to codec\n");
        return false;
    }

    printf("Codec: %s (%s)\n", codec->name, codec->long_name);

    const AVCodecHWConfig *hwcfg = 0;
    if (!p->software_decoding) {
        for (int i = 0; (hwcfg = avcodec_get_hw_config(codec, i)); i++) {
            if (!pl_test_pixfmt(p->win->gpu, hwcfg->pix_fmt))
                continue;
            if (!(hwcfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                continue;

            int ret = av_hwdevice_ctx_create(&p->codec->hw_device_ctx,
                                            hwcfg->device_type,
                                            NULL, NULL, 0);
            if (ret < 0) {
                fprintf(stderr, "libavcodec: Failed opening HW device context, skipping\n");
                continue;
            }

            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(hwcfg->pix_fmt);
            printf("Using hardware frame format: %s\n", desc->name);
            p->codec->extra_hw_frames = 4;
            break;
        }
    }

    if (!hwcfg)
        printf("Using software decoding\n");

    p->codec->thread_count = MIN(av_cpu_count() + 1, 16);
    p->codec->get_buffer2 = pl_get_buffer2;
    p->codec->opaque = &p->win->gpu;
#if LIBAVCODEC_VERSION_MAJOR < 60
    AV_NOWARN_DEPRECATED({
        p->codec->thread_safe_callbacks = 1;
    });
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 113, 100)
    p->codec->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
#endif

    if (avcodec_open2(p->codec, codec, NULL) < 0) {
        fprintf(stderr, "libavcodec: Failed opening codec\n");
        return false;
    }

    return true;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex,
                      const struct pl_source_frame *src,
                      struct pl_frame *out_frame)
{
    AVFrame *frame = src->frame_data;
    struct plplay *p = frame->opaque;
    bool ok = pl_map_avframe_ex(gpu, out_frame, pl_avframe_params(
        .frame      = frame,
        .tex        = tex,
        .map_dovi   = !p->ignore_dovi,
    ));

    av_frame_free(&frame); // references are preserved by `out_frame`
    if (!ok) {
        fprintf(stderr, "Failed mapping AVFrame!\n");
        return false;
    }

    p->stats.mapped++;
    pl_frame_copy_stream_props(out_frame, p->stream);
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    pl_unmap_avframe(gpu, frame);
}

static void discard_frame(const struct pl_source_frame *src)
{
    AVFrame *frame = src->frame_data;
    struct plplay *p = frame->opaque;
    p->stats.dropped++;
    av_frame_free(&frame);
    printf("Dropped frame with PTS %.3f\n", src->pts);
}

static PL_THREAD_VOID decode_loop(void *arg)
{
    int ret;
    struct plplay *p = arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!frame || !packet)
        goto done;

    float frame_duration = av_q2d(av_inv_q(p->stream->avg_frame_rate));
    double first_pts = 0.0, base_pts = 0.0, last_pts = 0.0;
    uint64_t num_frames = 0;

    while (!p->exit_thread) {
        switch ((ret = av_read_frame(p->format, packet))) {
        case 0:
            if (packet->stream_index != p->stream->index) {
                // Ignore unrelated packets
                av_packet_unref(packet);
                continue;
            }
            ret = avcodec_send_packet(p->codec, packet);
            av_packet_unref(packet);
            break;
        case AVERROR_EOF:
            // Send empty input to flush decoder
            ret = avcodec_send_packet(p->codec, NULL);
            break;
        default:
            fprintf(stderr, "libavformat: Failed reading packet: %s\n",
                    av_err2str(ret));
            goto done;
        }

        if (ret < 0) {
            fprintf(stderr, "libavcodec: Failed sending packet to decoder: %s\n",
                    av_err2str(ret));
            goto done;
        }

        // Decode all frames from this packet
        while ((ret = avcodec_receive_frame(p->codec, frame)) == 0) {
            last_pts = frame->pts * av_q2d(p->stream->time_base);
            if (num_frames++ == 0)
                first_pts = last_pts;
            frame->opaque = p;
            (void) atomic_fetch_add(&p->stats.decoded, 1);
            pl_queue_push_block(p->queue, UINT64_MAX, &(struct pl_source_frame) {
                .pts = last_pts - first_pts + base_pts,
                .duration = frame_duration,
                .map = map_frame,
                .unmap = unmap_frame,
                .discard = discard_frame,
                .frame_data = frame,

                // allow soft-disabling deinterlacing at the source frame level
                .first_field = p->params.deinterlace_params
                                    ? pl_field_from_avframe(frame)
                                    : PL_FIELD_NONE,
            });
            frame = av_frame_alloc();
        }

        switch (ret) {
        case AVERROR(EAGAIN):
            continue;
        case AVERROR_EOF:
            if (num_frames <= 1)
                goto done; // still image or empty file
            // loop infinitely
            ret = av_seek_frame(p->format, p->stream->index, 0, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                fprintf(stderr, "libavformat: Failed seeking in stream: %s\n",
                        av_err2str(ret));
                goto done;
            }
            avcodec_flush_buffers(p->codec);
            base_pts += last_pts;
            num_frames = 0;
            continue;
        default:
            fprintf(stderr, "libavcodec: Failed decoding frame: %s\n",
                    av_err2str(ret));
            goto done;
        }
    }

done:
    pl_queue_push(p->queue, NULL); // Signal EOF to flush queue
    av_packet_free(&packet);
    av_frame_free(&frame);
    PL_THREAD_RETURN();
}

static void apply_csp_overrides(struct plplay *p, struct pl_color_space *csp)
{
    if (p->force_prim) {
        csp->primaries = p->force_prim;
        csp->hdr.prim = *pl_raw_primaries_get(csp->primaries);
    }
    if (p->force_trc)
        csp->transfer = p->force_trc;
    if (p->force_hdr_enable) {
        struct pl_hdr_metadata fix = p->force_hdr;
        fix.prim = csp->hdr.prim;
        csp->hdr = fix;
    } else if (p->colorspace_hint_dynamic) {
        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
            .color      = csp,
            .metadata   = PL_HDR_METADATA_ANY,
            .scaling    = PL_HDR_NITS,
            .out_min    = &csp->hdr.min_luma,
            .out_max    = &csp->hdr.max_luma,
        ));
    }
}

static void update_settings(struct plplay *p, const struct pl_frame *target);

static void update_colorspace_hint(struct plplay *p, const struct pl_frame_mix *mix)
{
    const struct pl_frame *frame = NULL;

    for (int i = 0; i < mix->num_frames; i++) {
        if (mix->timestamps[i] > 0.0)
            break;
        frame = mix->frames[i];
    }

    if (!frame)
        return;

    struct pl_color_space hint = {0};
    if (p->colorspace_hint)
        hint = frame->color;
    if (p->target_override)
        apply_csp_overrides(p, &hint);
    pl_swapchain_colorspace_hint(p->win->swapchain, &hint);
}

static bool render_frame(struct plplay *p, const struct pl_swapchain_frame *frame,
                         const struct pl_frame_mix *mix)
{
    struct pl_frame target;
    pl_frame_from_swapchain(&target, frame);
    update_settings(p, &target);

    if (p->target_override) {
        target.profile = p->target_icc;
        target.repr = p->force_repr;
        pl_color_repr_merge(&target.repr, &frame->color_repr);
        apply_csp_overrides(p, &target.color);

        if (p->use_icc_luma) {
            // Always use detected luminance
            p->icc_params.max_luma = 0;
        } else {
            // Use HDR levels if available, fall back to default luminance
            p->icc_params.max_luma = target.color.hdr.max_luma;
            if (!p->icc_params.max_luma)
                p->icc_params.max_luma = pl_icc_default_params.max_luma;
        }
    }

    assert(mix->num_frames);
    double dar = pl_rect2df_aspect(&mix->frames[0]->crop);
    if (p->stream->sample_aspect_ratio.num)
        dar *= av_q2d(p->stream->sample_aspect_ratio);
    target.rotation = p->target_rot;
    pl_rect2df_aspect_set_rot(&target.crop, dar,
                              mix->frames[0]->rotation - target.rotation,
                              0.0);

    struct pl_color_map_params *cpars = &p->color_map_params;
    if (cpars->visualize_lut) {
        cpars->visualize_rect = (pl_rect2df) {0, 0, 1, 1};
        float tar = pl_rect2df_aspect(&target.crop);
        pl_rect2df_aspect_set(&cpars->visualize_rect, 1.0f / tar, 0.0f);
    }

    pl_clock_t ts_pre = pl_clock_now();
    if (!pl_render_image_mix(p->renderer, mix, &target, &p->params))
        return false;
    pl_clock_t ts_rendered = pl_clock_now();
    if (!ui_draw(p->ui, frame))
        return false;
    pl_clock_t ts_ui_drawn = pl_clock_now();

    log_time(&p->stats.render, pl_clock_diff(ts_rendered, ts_pre));
    log_time(&p->stats.draw_ui, pl_clock_diff(ts_ui_drawn, ts_rendered));

    p->stats.rendered++;
    return true;
}

static bool render_loop(struct plplay *p)
{
    struct pl_queue_params qparams = {
        .interpolation_threshold = 0.01,
        .timeout = UINT64_MAX,
    };

    // Initialize the frame queue, blocking indefinitely until done
    struct pl_frame_mix mix;
    switch (pl_queue_update(p->queue, &mix, &qparams)) {
    case PL_QUEUE_OK:  break;
    case PL_QUEUE_EOF: return true;
    case PL_QUEUE_ERR: goto error;
    default: abort();
    }

    struct pl_swapchain_frame frame;
    update_colorspace_hint(p, &mix);
    if (!pl_swapchain_start_frame(p->win->swapchain, &frame))
        goto error;

    // Disable background transparency by default if the swapchain does not
    // appear to support alpha transaprency
    if (frame.color_repr.alpha == PL_ALPHA_UNKNOWN)
        p->params.background_transparency = 0.0;

    if (!render_frame(p, &frame, &mix))
        goto error;
    if (!pl_swapchain_submit_frame(p->win->swapchain))
        goto error;

    // Wait until rendering is complete. Do this before measuring the time
    // start, to ensure we don't count initialization overhead as part of the
    // first vsync.
    pl_gpu_finish(p->win->gpu);
    p->stats.render = p->stats.draw_ui = (struct timing) {0};

    pl_clock_t ts_start = 0, ts_prev = 0;
    pl_swapchain_swap_buffers(p->win->swapchain);
    window_poll(p->win, false);

    double prev_pts = 0.0;

    while (!p->win->window_lost) {
        if (window_get_key(p->win, KEY_ESC))
            break;

        if (p->toggle_fullscreen)
            window_toggle_fullscreen(p->win, !window_is_fullscreen(p->win));

        update_colorspace_hint(p, &mix);
        pl_clock_t ts_acquire = pl_clock_now();
        if (!pl_swapchain_start_frame(p->win->swapchain, &frame)) {
            // Window stuck/invisible? Block for events and try again.
            window_poll(p->win, true);
            continue;
        }

        pl_clock_t ts_pre_update = pl_clock_now();
        log_time(&p->stats.acquire, pl_clock_diff(ts_pre_update, ts_acquire));
        if (!ts_start)
            ts_start = ts_pre_update;

        qparams.timeout = 0; // non-blocking update
        qparams.radius = pl_frame_mix_radius(&p->params);
        qparams.pts = pl_clock_diff(ts_pre_update, ts_start);
        p->stats.current_pts = qparams.pts;
        if (qparams.pts != prev_pts)
            log_time(&p->stats.pts_interval, qparams.pts - prev_pts);
        prev_pts = qparams.pts;

retry:
        switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_ERR: goto error;
        case PL_QUEUE_EOF:
            printf("End of file reached\n");
            return true;
        case PL_QUEUE_OK:
            break;
        case PL_QUEUE_MORE:
            qparams.timeout = UINT64_MAX; // retry in blocking mode
            goto retry;
        }

        pl_clock_t ts_post_update = pl_clock_now();
        log_time(&p->stats.update, pl_clock_diff(ts_post_update, ts_pre_update));

        if (qparams.timeout) {
            double stuck_ms = 1e3 * pl_clock_diff(ts_post_update, ts_pre_update);
            fprintf(stderr, "Stalled for %.4f ms due to frame queue underrun!\n", stuck_ms);
            ts_start += ts_post_update - ts_pre_update; // subtract time spent waiting
            p->stats.stalled++;
            p->stats.stalled_ms += stuck_ms;
        }

        if (!render_frame(p, &frame, &mix))
            goto error;


        pl_clock_t ts_pre_submit = pl_clock_now();
        if (!pl_swapchain_submit_frame(p->win->swapchain)) {
            fprintf(stderr, "libplacebo: failed presenting frame!\n");
            goto error;
        }
        pl_clock_t ts_post_submit = pl_clock_now();
        log_time(&p->stats.submit, pl_clock_diff(ts_post_submit, ts_pre_submit));

        if (ts_prev)
            log_time(&p->stats.vsync_interval, pl_clock_diff(ts_post_submit, ts_prev));
        ts_prev = ts_post_submit;

        pl_swapchain_swap_buffers(p->win->swapchain);
        pl_clock_t ts_post_swap = pl_clock_now();
        log_time(&p->stats.swap, pl_clock_diff(ts_post_swap, ts_post_submit));

        window_poll(p->win, false);
    }

    return true;

error:
    fprintf(stderr, "Render loop failed, exiting early...\n");
    return false;
}

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct plplay *p = priv;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME:
        if (info->index >= MAX_FRAME_PASSES)
            return;
        p->num_frame_passes = info->index + 1;
        pl_dispatch_info_move(&p->frame_info[info->index], info->pass);
        return;

    case PL_RENDER_STAGE_BLEND:
        if (info->index >= MAX_BLEND_PASSES || info->count >= MAX_BLEND_FRAMES)
            return;
        p->num_blend_passes[info->count] = info->index + 1;
        pl_dispatch_info_move(&p->blend_info[info->count][info->index], info->pass);
        return;

    case PL_RENDER_STAGE_COUNT:
        break;
    }

    abort();
}

static struct plplay state;

int main(int argc, char **argv)
{
    state = (struct plplay) {
        .params = pl_render_default_params,
        .deband_params = pl_deband_default_params,
        .sigmoid_params = pl_sigmoid_default_params,
        .color_adjustment = pl_color_adjustment_neutral,
        .peak_detect_params = pl_peak_detect_default_params,
        .color_map_params = pl_color_map_default_params,
        .dither_params = pl_dither_default_params,
        .icc_params = pl_icc_default_params,
        .cone_params = pl_vision_normal,
        .deinterlace_params = pl_deinterlace_default_params,
        .distort_params = pl_distort_default_params,
        .target_override = true,
        .use_icc_luma = true,
    };

    const char *filename = NULL;
    enum pl_log_level log_level = PL_LOG_INFO;
    bool print_help = false;
    const char *win_tag = NULL;

    for (char **arg = argv + 1; !print_help && *arg; ++arg) {
        if ((*arg)[0] != '-') {
            filename = *arg++;
            while (*arg)
                fprintf(stderr, "Ignored trailing arg: %s\n", *arg++);
            break;
        }
        switch ((*arg)[1]) {
        case 's': state.software_decoding = true; break;
        case 'v': log_level = PL_LOG_DEBUG; break;
        case 'a':
            win_tag = *++arg;
            if (!win_tag)
                print_help = true;
            break;
        case 'h':
            state.params = pl_render_high_quality_params;
            state.color_map_params = pl_color_map_high_quality_params;
            state.peak_detect_params = pl_peak_detect_high_quality_params;;
            break;
        default:
            fprintf(stderr, "Invalid arg: %s\n", *arg);
            print_help = true;
            break;
        }
    }

    if (print_help || !filename) {
        fprintf(stderr,
            "Usage: %s [-a] [-h] [-s] [-v] <filename>\n\n"
            "Options:\n"
            "\t-a api\twindow api selection [glfw-vk, glfw-gl, glfw-d3d11, sdl2-vk, sdl2-gl]\n"
            "\t-h\thigh quality defaults\n"
            "\t-s\tsoftware decoding\n"
            "\t-v\tverbose\n"
        , argv[0]);
        return -1;
    }

    if (log_level == PL_LOG_DEBUG)
        av_log_set_level(AV_LOG_VERBOSE);
    else
        av_log_set_level(AV_LOG_INFO);

    // Redirect all of the pointers in `params.default` to instead point to the
    // structs inside `struct plplay`, so we can adjust them using the UI
#define DEFAULT_PARAMS(field) \
        state.params.field = state.params.field ? &state.field : NULL
    DEFAULT_PARAMS(deband_params);
    DEFAULT_PARAMS(sigmoid_params);
    DEFAULT_PARAMS(peak_detect_params);
    DEFAULT_PARAMS(dither_params);
    DEFAULT_PARAMS(deinterlace_params);
    state.params.color_adjustment = &state.color_adjustment;
    state.params.color_map_params = &state.color_map_params;
    state.params.cone_params = &state.cone_params;
    state.params.icc_params = &state.icc_params;

    // Enable dynamic parameters by default, due to plplay's heavy reliance on
    // GUI controls for dynamically adjusting render parameters.
    state.params.dynamic_constants = true;

    // Hook up our pass info callback
    state.params.info_callback = info_callback;
    state.params.info_priv = &state;

    struct plplay *p = &state;
    if (!open_file(p, filename))
        goto error;

    const AVCodecParameters *par = p->stream->codecpar;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(par->format);
    if (!desc)
        goto error;

    struct window_params params = {
        .title = "plplay",
        .width = par->width,
        .height = par->height,
        .forced_impl = win_tag,
    };

    if (desc->flags & AV_PIX_FMT_FLAG_ALPHA) {
        params.alpha = true;
        state.params.background_transparency = 1.0;
    }

    p->log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = pl_log_color,
        .log_level = log_level,
    ));

    p->win = window_create(p->log, &params);
    if (!p->win)
        goto error;

    // Test the AVPixelFormat against the GPU capabilities
    if (!pl_test_pixfmt(p->win->gpu, par->format)) {
        fprintf(stderr, "Unsupported AVPixelFormat: %s\n", desc->name);
        goto error;
    }

#ifdef HAVE_NUKLEAR
    p->ui = ui_create(p->win->gpu);
    if (!p->ui)
        goto error;

    // Find the right named filter entries for the defaults
    const struct pl_filter_preset *f;
    for (f = pl_scale_filters; f->name; f++) {
        if (p->params.upscaler == f->filter)
            p->upscaler = f;
        if (p->params.downscaler == f->filter)
            p->downscaler = f;
        if (p->params.plane_upscaler == f->filter)
            p->plane_scaler = f;
    }

    for (f = pl_frame_mixers; f->name; f++) {
        if (p->params.frame_mixer == f->filter)
            p->frame_mixer = f;
    }

    assert(p->upscaler && p->downscaler && p->frame_mixer);
#endif

    if (!init_codec(p))
        goto error;

    p->queue = pl_queue_create(p->win->gpu);
    int ret = pl_thread_create(&p->decoder_thread, decode_loop, p);
    if (ret != 0) {
        fprintf(stderr, "Failed creating decode thread: %s\n", strerror(errno));
        goto error;
    }

    p->decoder_thread_created = true;

    p->renderer = pl_renderer_create(p->log, p->win->gpu);
    if (!render_loop(p))
        goto error;

    printf("Exiting...\n");
    uninit(p);
    return 0;

error:
    uninit(p);
    return 1;
}

#ifdef HAVE_NUKLEAR

static void add_hook(struct plplay *p, const struct pl_hook *hook, const char *path)
{
    if (!hook)
        return;

    if (p->shader_num == p->shader_size) {
        // Grow array if needed
        size_t new_size = p->shader_size ? p->shader_size * 2 : 16;
        void *new_hooks = realloc(p->shader_hooks, new_size * sizeof(void *));
        if (!new_hooks)
            goto error;
        p->shader_hooks = new_hooks;
        char **new_paths = realloc(p->shader_paths, new_size * sizeof(char *));
        if (!new_paths)
            goto error;
        p->shader_paths = new_paths;
        p->shader_size = new_size;
    }

    // strip leading path
    while (true) {
        const char *fname = strchr(path, '/');
        if (!fname)
            break;
        path = fname + 1;
    }

    char *path_copy = strdup(path);
    if (!path_copy)
        goto error;

    p->shader_hooks[p->shader_num] = hook;
    p->shader_paths[p->shader_num] = path_copy;
    p->shader_num++;
    return;

error:
    pl_mpv_user_shader_destroy(&hook);
}

static const char *pscale_desc(const struct pl_filter_preset *f)
{
    return f->filter ? f->description : "None (Use regular upscaler)";
}

static void auto_property_int(struct nk_context *nk, int auto_val, int min, int *val,
                         int max, int step, float inc_per_pixel)
{
    int value = *val;
    if (!value)
        value = auto_val;

    // Auto label will be delayed 1 frame
    nk_property_int(nk, *val ? "" : "Auto", min, &value, max, step, inc_per_pixel);

    if (*val || value != auto_val)
        *val = value;
}

static void draw_shader_pass(struct nk_context *nk,
                             const struct pl_dispatch_info *info)
{
    pl_shader_info shader = info->shader;

    char label[128];
    int count = snprintf(label, sizeof(label), "%.3f/%.3f/%.3f ms: %s",
             info->last / 1e6,
             info->average / 1e6,
             info->peak / 1e6,
             shader->description);

    if (count >= sizeof(label)) {
        label[sizeof(label) - 4] = '.';
        label[sizeof(label) - 3] = '.';
        label[sizeof(label) - 2] = '.';
    }

    int id = (unsigned int) (uintptr_t) info; // pointer into `struct plplay`
    if (nk_tree_push_id(nk, NK_TREE_NODE, label, NK_MINIMIZED, id)) {
        nk_layout_row_dynamic(nk, 32, 1);
        if (nk_chart_begin(nk, NK_CHART_LINES,
                           info->num_samples,
                           0.0f, info->peak))
        {
            for (int k = 0; k < info->num_samples; k++)
                nk_chart_push(nk, info->samples[k]);
            nk_chart_end(nk);
        }

        nk_layout_row_dynamic(nk, 24, 1);
        for (int n = 0; n < shader->num_steps; n++)
            nk_labelf(nk, NK_TEXT_LEFT, "%d. %s", n + 1, shader->steps[n]);
        nk_tree_pop(nk);
    }
}

static void draw_timing(struct nk_context *nk, const char *label,
                        const struct timing *t)
{
    const double avg = t->count ? t->sum / t->count : 0.0;
    const double stddev = t->count ? sqrt(t->sum2 / t->count - avg * avg) : 0.0;
    nk_label(nk, label, NK_TEXT_LEFT);
    nk_labelf(nk, NK_TEXT_LEFT, "%.4f ± %.4f ms (%.3f ms)",
              avg * 1e3, stddev * 1e3, t->peak * 1e3);
}

static void update_settings(struct plplay *p, const struct pl_frame *target)
{
    struct nk_context *nk = ui_get_context(p->ui);
    enum nk_panel_flags win_flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE |
                                    NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE |
                                    NK_WINDOW_TITLE;

    ui_update_input(p->ui, p->win);
    const char *dropped_file = window_get_file(p->win);

    struct pl_render_params *par = &p->params;

    if (nk_begin(nk, "Settings", nk_rect(100, 100, 600, 600), win_flags)) {

        if (nk_tree_push(nk, NK_TREE_NODE, "Window settings", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(nk, 24, 2);

            bool fullscreen = window_is_fullscreen(p->win);
            p->toggle_fullscreen = nk_checkbox_label(nk, "Fullscreen", &fullscreen);
            nk_property_float(nk, "Corner rounding", 0.0, &par->corner_rounding, 1.0, 0.1, 0.01);

            struct nk_colorf bg = {
                par->background_color[0],
                par->background_color[1],
                par->background_color[2],
                1.0 - par->background_transparency,
            };

            nk_layout_row_dynamic(nk, 24, 2);
            nk_label(nk, "Background color:", NK_TEXT_LEFT);
            if (nk_combo_begin_color(nk, nk_rgb_cf(bg), nk_vec2(nk_widget_width(nk), 300))) {
                nk_layout_row_dynamic(nk, 200, 1);
                nk_color_pick(nk, &bg, NK_RGBA);
                nk_combo_end(nk);

                par->background_color[0] = bg.r;
                par->background_color[1] = bg.g;
                par->background_color[2] = bg.b;
                par->background_transparency = 1.0 - bg.a;
            }

            nk_layout_row_dynamic(nk, 24, 2);
            par->blend_against_tiles = nk_check_label(nk, "Blend against tiles", par->blend_against_tiles);
            nk_property_int(nk, "Tile size", 2, &par->tile_size, 256, 1, 1);

            nk_layout_row(nk, NK_DYNAMIC, 24, 3, (float[]){ 0.4, 0.3, 0.3 });
            nk_label(nk, "Tile colors:", NK_TEXT_LEFT);
            for (int i = 0; i < 2; i++) {
                bg = (struct nk_colorf) {
                    par->tile_colors[i][0],
                    par->tile_colors[i][1],
                    par->tile_colors[i][2],
                };

                if (nk_combo_begin_color(nk, nk_rgb_cf(bg), nk_vec2(nk_widget_width(nk), 300))) {
                    nk_layout_row_dynamic(nk, 200, 1);
                    nk_color_pick(nk, &bg, NK_RGB);
                    nk_combo_end(nk);

                    par->tile_colors[i][0] = bg.r;
                    par->tile_colors[i][1] = bg.g;
                    par->tile_colors[i][2] = bg.b;
                }
            }

            static const char *rotations[4] = {
                [PL_ROTATION_0]   = "0°",
                [PL_ROTATION_90]  = "90°",
                [PL_ROTATION_180] = "180°",
                [PL_ROTATION_270] = "270°",
            };

            nk_layout_row_dynamic(nk, 24, 2);
            nk_label(nk, "Display orientation:", NK_TEXT_LEFT);
            p->target_rot = nk_combo(nk, rotations, 4, p->target_rot,
                                     16, nk_vec2(nk_widget_width(nk), 100));
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Image scaling", NK_MAXIMIZED)) {
            const struct pl_filter_preset *f;
            nk_layout_row(nk, NK_DYNAMIC, 24, 2, (float[]){ 0.3, 0.7 });
            nk_label(nk, "Upscaler:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, p->upscaler->description, nk_vec2(nk_widget_width(nk), 500))) {
                nk_layout_row_dynamic(nk, 16, 1);
                for (f = pl_scale_filters; f->name; f++) {
                    if (!f->description)
                        continue;
                    if (nk_combo_item_label(nk, f->description, NK_TEXT_LEFT))
                        p->upscaler = f;
                }
                par->upscaler = p->upscaler->filter;
                nk_combo_end(nk);
            }

            nk_label(nk, "Downscaler:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, p->downscaler->description, nk_vec2(nk_widget_width(nk), 500))) {
                nk_layout_row_dynamic(nk, 16, 1);
                for (f = pl_scale_filters; f->name; f++) {
                    if (!f->description)
                        continue;
                    if (nk_combo_item_label(nk, f->description, NK_TEXT_LEFT))
                        p->downscaler = f;
                }
                par->downscaler = p->downscaler->filter;
                nk_combo_end(nk);
            }

            nk_label(nk, "Plane scaler:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, pscale_desc(p->plane_scaler), nk_vec2(nk_widget_width(nk), 500))) {
                nk_layout_row_dynamic(nk, 16, 1);
                for (f = pl_scale_filters; f->name; f++) {
                    if (!f->description)
                        continue;
                    if (nk_combo_item_label(nk, pscale_desc(f), NK_TEXT_LEFT))
                        p->plane_scaler = f;
                }
                par->plane_upscaler = p->plane_scaler->filter;
                nk_combo_end(nk);
            }

            nk_label(nk, "Frame mixing:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, p->frame_mixer->description, nk_vec2(nk_widget_width(nk), 300))) {
                nk_layout_row_dynamic(nk, 16, 1);
                for (f = pl_frame_mixers; f->name; f++) {
                    if (!f->description)
                        continue;
                    if (nk_combo_item_label(nk, f->description, NK_TEXT_LEFT))
                        p->frame_mixer = f;
                }
                par->frame_mixer = p->frame_mixer->filter;
                nk_combo_end(nk);
            }

            nk_layout_row_dynamic(nk, 24, 2);
            par->skip_anti_aliasing = !nk_check_label(nk, "Anti-aliasing", !par->skip_anti_aliasing);
            nk_property_float(nk, "Antiringing", 0, &par->antiringing_strength, 1.0, 0.05, 0.001);
            nk_property_int(nk, "LUT precision", 0, &par->lut_entries, 256, 1, 1);

            float cutoff = par->polar_cutoff * 100.0;
            nk_property_float(nk, "Polar cutoff (%)", 0.0, &cutoff, 100.0, 0.1, 0.01);
            par->polar_cutoff = cutoff / 100.0;

            struct pl_sigmoid_params *spar = &p->sigmoid_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->sigmoid_params = nk_check_label(nk, "Sigmoidization", par->sigmoid_params) ? spar : NULL;
            if (nk_button_label(nk, "Default values"))
                *spar = pl_sigmoid_default_params;
            nk_property_float(nk, "Sigmoid center", 0, &spar->center, 1, 0.1, 0.01);
            nk_property_float(nk, "Sigmoid slope", 0, &spar->slope, 100, 1, 0.1);
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Deinterlacing", NK_MINIMIZED)) {
            struct pl_deinterlace_params *dpar = &p->deinterlace_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->deinterlace_params = nk_check_label(nk, "Enable", par->deinterlace_params) ? dpar : NULL;
            if (nk_button_label(nk, "Reset settings"))
                *dpar = pl_deinterlace_default_params;

            static const char *deint_algos[PL_DEINTERLACE_ALGORITHM_COUNT] = {
                [PL_DEINTERLACE_WEAVE]  = "Field weaving (no-op)",
                [PL_DEINTERLACE_BOB]    = "Naive bob (line doubling)",
                [PL_DEINTERLACE_YADIF]  = "Yadif (\"yet another deinterlacing filter\")",
            };

            nk_label(nk, "Deinterlacing algorithm", NK_TEXT_LEFT);
            dpar->algo = nk_combo(nk, deint_algos, PL_DEINTERLACE_ALGORITHM_COUNT,
                                  dpar->algo, 16, nk_vec2(nk_widget_width(nk), 300));

            switch (dpar->algo) {
            case PL_DEINTERLACE_WEAVE:
            case PL_DEINTERLACE_BOB:
                break;
            case PL_DEINTERLACE_YADIF:
                nk_checkbox_label(nk, "Skip spatial check", &dpar->skip_spatial_check);
                break;
            default: abort();
            }
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Debanding", NK_MINIMIZED)) {
            struct pl_deband_params *dpar = &p->deband_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->deband_params = nk_check_label(nk, "Enable", par->deband_params) ? dpar : NULL;
            if (nk_button_label(nk, "Reset settings"))
                *dpar = pl_deband_default_params;
            nk_property_int(nk, "Iterations", 0, &dpar->iterations, 8, 1, 0);
            nk_property_float(nk, "Threshold", 0, &dpar->threshold, 256, 1, 0.5);
            nk_property_float(nk, "Radius", 0, &dpar->radius, 256, 1, 0.2);
            nk_property_float(nk, "Grain", 0, &dpar->grain, 512, 1, 0.5);
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Distortion", NK_MINIMIZED)) {
            struct pl_distort_params *dpar = &p->distort_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->distort_params = nk_check_label(nk, "Enable", par->distort_params) ? dpar : NULL;
            if (nk_button_label(nk, "Reset settings"))
                *dpar = pl_distort_default_params;

            static const char *address_modes[PL_TEX_ADDRESS_MODE_COUNT] = {
                [PL_TEX_ADDRESS_CLAMP]  = "Clamp edges",
                [PL_TEX_ADDRESS_REPEAT] = "Repeat edges",
                [PL_TEX_ADDRESS_MIRROR] = "Mirror edges",
            };

            nk_checkbox_label(nk, "Constrain bounds", &dpar->constrain);
            dpar->address_mode = nk_combo(nk, address_modes, PL_TEX_ADDRESS_MODE_COUNT,
                                          dpar->address_mode, 16, nk_vec2(nk_widget_width(nk), 100));
            bool alpha = nk_check_label(nk, "Transparent background", dpar->alpha_mode);
            dpar->alpha_mode = alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_UNKNOWN;
            nk_checkbox_label(nk, "Bicubic interpolation", &dpar->bicubic);

            struct pl_transform2x2 *tf = &dpar->transform;
            nk_property_float(nk, "Scale X", -10.0, &tf->mat.m[0][0], 10.0, 0.1, 0.005);
            nk_property_float(nk, "Shear X", -10.0, &tf->mat.m[0][1], 10.0, 0.1, 0.005);
            nk_property_float(nk, "Shear Y", -10.0, &tf->mat.m[1][0], 10.0, 0.1, 0.005);
            nk_property_float(nk, "Scale Y", -10.0, &tf->mat.m[1][1], 10.0, 0.1, 0.005);
            nk_property_float(nk, "Offset X", -10.0, &tf->c[0], 10.0, 0.1, 0.005);
            nk_property_float(nk, "Offset Y", -10.0, &tf->c[1], 10.0, 0.1, 0.005);

            float zoom_ref = fabsf(tf->mat.m[0][0] * tf->mat.m[1][1] -
                                   tf->mat.m[0][1] * tf->mat.m[1][0]);
            zoom_ref = logf(fmaxf(zoom_ref, 1e-4));
            float zoom = zoom_ref;
            nk_property_float(nk, "log(Zoom)", -10.0, &zoom, 10.0, 0.1, 0.005);
            pl_transform2x2_scale(tf, expf(zoom - zoom_ref));

            float angle_ref = (atan2f(tf->mat.m[1][0], tf->mat.m[1][1]) -
                               atan2f(tf->mat.m[0][1], tf->mat.m[0][0])) / 2;
            angle_ref = fmodf(angle_ref * 180/M_PI + 540, 360) - 180;
            float angle = angle_ref;
            nk_property_float(nk, "Rotate (°)", -200, &angle, 200, -5, -0.2);
            float angle_delta = (angle - angle_ref) * M_PI / 180;
            const pl_matrix2x2 rot = pl_matrix2x2_rotation(angle_delta);
            pl_matrix2x2_rmul(&rot, &tf->mat);

            bool flip_ox = nk_button_label(nk, "Flip output X");
            bool flip_oy = nk_button_label(nk, "Flip output Y");
            bool flip_ix = nk_button_label(nk, "Flip input X");
            bool flip_iy = nk_button_label(nk, "Flip input Y");
            if (flip_ox ^ flip_ix)
                tf->mat.m[0][0] = -tf->mat.m[0][0];
            if (flip_ox ^ flip_iy)
                tf->mat.m[0][1] = -tf->mat.m[0][1];
            if (flip_oy ^ flip_ix)
                tf->mat.m[1][0] = -tf->mat.m[1][0];
            if (flip_oy ^ flip_iy)
                tf->mat.m[1][1] = -tf->mat.m[1][1];
            if (flip_ox)
                tf->c[0] = -tf->c[0];
            if (flip_oy)
                tf->c[1] = -tf->c[1];

            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Color adjustment", NK_MINIMIZED)) {
            struct pl_color_adjustment *adj = &p->color_adjustment;
            nk_layout_row_dynamic(nk, 24, 2);
            par->color_adjustment = nk_check_label(nk, "Enable", par->color_adjustment) ? adj : NULL;
            if (nk_button_label(nk, "Default values"))
                *adj = pl_color_adjustment_neutral;
            nk_property_float(nk, "Brightness", -1, &adj->brightness, 1, 0.1, 0.005);
            nk_property_float(nk, "Contrast", 0, &adj->contrast, 10, 0.1, 0.005);

            // Convert to (cyclical) degrees for display
            int deg = roundf(adj->hue * 180.0 / M_PI);
            nk_property_int(nk, "Hue (°)", -50, &deg, 400, 1, 1);
            adj->hue = ((deg + 360) % 360) * M_PI / 180.0;

            nk_property_float(nk, "Saturation", 0, &adj->saturation, 10, 0.1, 0.005);
            nk_property_float(nk, "Gamma", 0, &adj->gamma, 10, 0.1, 0.005);

            // Convert to human-friendly temperature values for display
            int temp = (int) roundf(adj->temperature * 3500) + 6500;
            nk_property_int(nk, "Temperature (K)", 3000, &temp, 10000, 10, 5);
            adj->temperature = (temp - 6500) / 3500.0;

            struct pl_cone_params *cpar = &p->cone_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->cone_params = nk_check_label(nk, "Color blindness", par->cone_params) ? cpar : NULL;
            if (nk_button_label(nk, "Default values"))
                *cpar = pl_vision_normal;
            nk_layout_row(nk, NK_DYNAMIC, 24, 5, (float[]){ 0.25, 0.25/3, 0.25/3, 0.25/3, 0.5 });
            nk_label(nk, "Cone model:", NK_TEXT_LEFT);
            unsigned int cones = cpar->cones;
            nk_checkbox_flags_label(nk, "L", &cones, PL_CONE_L);
            nk_checkbox_flags_label(nk, "M", &cones, PL_CONE_M);
            nk_checkbox_flags_label(nk, "S", &cones, PL_CONE_S);
            cpar->cones = cones;
            nk_property_float(nk, "Sensitivity", 0.0, &cpar->strength, 5.0, 0.1, 0.01);
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "HDR peak detection", NK_MINIMIZED)) {
            struct pl_peak_detect_params *ppar = &p->peak_detect_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->peak_detect_params = nk_check_label(nk, "Enable", par->peak_detect_params) ? ppar : NULL;
            if (nk_button_label(nk, "Reset settings"))
                *ppar = pl_peak_detect_default_params;
            nk_property_float(nk, "Threshold low", 0.0, &ppar->scene_threshold_low, 20.0, 0.5, 0.005);
            nk_property_float(nk, "Threshold high", 0.0, &ppar->scene_threshold_high, 20.0, 0.5, 0.005);
            nk_property_float(nk, "Smoothing period", 1.0, &ppar->smoothing_period, 1000.0, 5.0, 1.0);
            nk_property_float(nk, "Minimum peak", 0.0, &ppar->minimum_peak, 10.0, 0.1, 0.01);
            nk_property_float(nk, "Peak percentile", 95.0, &ppar->percentile, 100.0, 0.01, 0.001);
            nk_checkbox_label(nk, "Allow 1-frame delay", &ppar->allow_delayed);
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Tone mapping", NK_MINIMIZED)) {
            struct pl_color_map_params *cpar = &p->color_map_params;
            static const struct pl_color_map_params null_settings = {0};
            nk_layout_row_dynamic(nk, 24, 2);
            par->color_map_params = nk_check_label(nk, "Enable",
                par->color_map_params == cpar) ? cpar : &null_settings;
            if (nk_button_label(nk, "Reset settings"))
                *cpar = pl_color_map_default_params;

            nk_label(nk, "Gamut mapping function:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, cpar->gamut_mapping->description,
                                     nk_vec2(nk_widget_width(nk), 500)))
            {
                nk_layout_row_dynamic(nk, 16, 1);
                for (int i = 0; i < pl_num_gamut_map_functions; i++) {
                    const struct pl_gamut_map_function *f = pl_gamut_map_functions[i];
                    if (nk_combo_item_label(nk, f->description, NK_TEXT_LEFT))
                        cpar->gamut_mapping = f;
                }
                nk_combo_end(nk);
            }

            nk_label(nk, "Tone mapping function:", NK_TEXT_LEFT);
            if (nk_combo_begin_label(nk, cpar->tone_mapping_function->description,
                                     nk_vec2(nk_widget_width(nk), 500)))
            {
                nk_layout_row_dynamic(nk, 16, 1);
                for (int i = 0; i < pl_num_tone_map_functions; i++) {
                    const struct pl_tone_map_function *f = pl_tone_map_functions[i];
                    if (nk_combo_item_label(nk, f->description, NK_TEXT_LEFT)) {
                        if (f != cpar->tone_mapping_function)
                            cpar->tone_mapping_param = f->param_def;
                        cpar->tone_mapping_function = f;
                    }
                }
                nk_combo_end(nk);
            }

            nk_label(nk, "Algorithm parameter:", NK_TEXT_LEFT);
            const struct pl_tone_map_function *fun = cpar->tone_mapping_function;
            if (fun->param_desc) {
                nk_property_float(nk, fun->param_desc, fmaxf(fun->param_min, 0.001),
                                  &cpar->tone_mapping_param, fun->param_max,
                                  0.01, 0.001);
            } else {
                nk_label(nk, "(N/A)", NK_TEXT_LEFT);
            }

            static const char *metadata_types[PL_HDR_METADATA_TYPE_COUNT] = {
                [PL_HDR_METADATA_ANY]               = "Automatic selection",
                [PL_HDR_METADATA_NONE]              = "None (disabled)",
                [PL_HDR_METADATA_HDR10]             = "HDR10 (static)",
                [PL_HDR_METADATA_HDR10PLUS]         = "HDR10+ (MaxRGB)",
                [PL_HDR_METADATA_CIE_Y]             = "Luminance (CIE Y)",
            };

            nk_label(nk, "HDR metadata source:", NK_TEXT_LEFT);
            cpar->metadata = nk_combo(nk, metadata_types,
                                      PL_HDR_METADATA_TYPE_COUNT,
                                      cpar->metadata,
                                      16, nk_vec2(nk_widget_width(nk), 300));

            nk_property_float(nk, "Contrast recovery", 0.0, &cpar->contrast_recovery, 2.0, 0.05, 0.005);
            nk_property_float(nk, "Contrast smoothness", 1.0, &cpar->contrast_smoothness, 32.0, 0.1, 0.005);

            nk_property_int(nk, "LUT size", 16, &cpar->lut_size, 1024, 1, 1);
            nk_property_int(nk, "3DLUT size I", 7, &cpar->lut3d_size[0], 65, 1, 1);
            nk_property_int(nk, "3DLUT size C", 7, &cpar->lut3d_size[1], 256, 1, 1);
            nk_property_int(nk, "3DLUT size h", 7, &cpar->lut3d_size[2], 1024, 1, 1);

            nk_checkbox_label(nk, "Tricubic interpolation", &cpar->lut3d_tricubic);
            nk_checkbox_label(nk, "Force full LUT", &cpar->force_tone_mapping_lut);
            nk_checkbox_label(nk, "Inverse tone mapping", &cpar->inverse_tone_mapping);
            nk_checkbox_label(nk, "Show clipping", &cpar->show_clipping);
            nk_checkbox_label(nk, "Visualize LUT", &cpar->visualize_lut);

            if (cpar->visualize_lut) {
                nk_layout_row_dynamic(nk, 24, 2);
                const float huerange = 2 * M_PI;
                nk_property_float(nk, "Hue",   -1, &cpar->visualize_hue, huerange + 1.0, 0.1, 0.01);
                nk_property_float(nk, "Theta", 0.0, &cpar->visualize_theta, M_PI_2, 0.1, 0.01);
                cpar->visualize_hue = fmodf(cpar->visualize_hue + huerange, huerange);
            }

            nk_layout_row_dynamic(nk, 50, 1);
            if (ui_widget_hover(nk, "Drop .cube file here...") && dropped_file) {
                uint8_t *buf;
                size_t size;
                int ret = av_file_map(dropped_file, &buf, &size, 0, NULL);
                if (ret < 0) {
                    fprintf(stderr, "Failed opening '%s': %s\n", dropped_file,
                            av_err2str(ret));
                } else {
                    pl_lut_free((struct pl_custom_lut **) &par->lut);
                    par->lut = pl_lut_parse_cube(p->log, (char *) buf, size);
                    av_file_unmap(buf, size);
                }
            }

            static const char *lut_types[] = {
                [PL_LUT_UNKNOWN]    = "Auto (unknown)",
                [PL_LUT_NATIVE]     = "Raw RGB (native)",
                [PL_LUT_NORMALIZED] = "Linear RGB (normalized)",
                [PL_LUT_CONVERSION] = "Gamut conversion (native)",
            };

            nk_layout_row(nk, NK_DYNAMIC, 24, 3, (float[]){ 0.2, 0.3, 0.5 });
            if (nk_button_label(nk, "Reset LUT")) {
                pl_lut_free((struct pl_custom_lut **) &par->lut);
                par->lut_type = PL_LUT_UNKNOWN;
            }

            nk_label(nk, "LUT type:", NK_TEXT_CENTERED);
            par->lut_type = nk_combo(nk, lut_types, 4, par->lut_type,
                                     16, nk_vec2(nk_widget_width(nk), 100));

            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Dithering", NK_MINIMIZED)) {
            struct pl_dither_params *dpar = &p->dither_params;
            nk_layout_row_dynamic(nk, 24, 2);
            par->dither_params = nk_check_label(nk, "Enable", par->dither_params) ? dpar : NULL;
            if (nk_button_label(nk, "Reset settings"))
                *dpar = pl_dither_default_params;

            static const char *dither_methods[PL_DITHER_METHOD_COUNT] = {
                [PL_DITHER_BLUE_NOISE]      = "Blue noise",
                [PL_DITHER_ORDERED_LUT]     = "Ordered (LUT)",
                [PL_DITHER_ORDERED_FIXED]   = "Ordered (fixed size)",
                [PL_DITHER_WHITE_NOISE]     = "White noise",
            };

            nk_label(nk, "Dither method:", NK_TEXT_LEFT);
            dpar->method = nk_combo(nk, dither_methods, PL_DITHER_METHOD_COUNT, dpar->method,
                                    16, nk_vec2(nk_widget_width(nk), 100));

            static const char *lut_sizes[8] = {
                "2x2", "4x4", "8x8", "16x16", "32x32", "64x64", "128x128", "256x256",
            };

            nk_label(nk, "LUT size:", NK_TEXT_LEFT);
            switch (dpar->method) {
            case PL_DITHER_BLUE_NOISE:
            case PL_DITHER_ORDERED_LUT: {
                int size = dpar->lut_size - 1;
                nk_combobox(nk, lut_sizes, 8, &size, 16, nk_vec2(nk_widget_width(nk), 200));
                dpar->lut_size = size + 1;
                break;
            }
            case PL_DITHER_ORDERED_FIXED:
                nk_label(nk, "64x64", NK_TEXT_LEFT);
                break;
            default:
                nk_label(nk, "(N/A)", NK_TEXT_LEFT);
                break;
            }

            nk_checkbox_label(nk, "Temporal dithering", &dpar->temporal);

            nk_layout_row_dynamic(nk, 24, 2);
            nk_label(nk, "Error diffusion:", NK_TEXT_LEFT);
            const char *name = par->error_diffusion ? par->error_diffusion->description : "(None)";
            if (nk_combo_begin_label(nk, name, nk_vec2(nk_widget_width(nk), 500))) {
                nk_layout_row_dynamic(nk, 16, 1);
                if (nk_combo_item_label(nk, "(None)", NK_TEXT_LEFT))
                    par->error_diffusion = NULL;
                for (int i = 0; i < pl_num_error_diffusion_kernels; i++) {
                    const struct pl_error_diffusion_kernel *k = pl_error_diffusion_kernels[i];
                    if (nk_combo_item_label(nk, k->description, NK_TEXT_LEFT))
                        par->error_diffusion = k;
                }
                nk_combo_end(nk);
            }

            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Output color space", NK_MINIMIZED)) {
            struct pl_icc_params *iccpar = &p->icc_params;
            nk_layout_row_dynamic(nk, 24, 2);
            nk_checkbox_label(nk, "Enable", &p->target_override);
            bool reset = nk_button_label(nk, "Reset settings");
            bool reset_icc = reset;
            char buf[64] = {0};

            nk_layout_row(nk, NK_DYNAMIC, 24, 2, (float[]){ 0.3, 0.7 });

            const char *primaries[PL_COLOR_PRIM_COUNT] = {
                [PL_COLOR_PRIM_UNKNOWN]     = "Auto (unknown)",
                [PL_COLOR_PRIM_BT_601_525]  = "ITU-R Rec. BT.601 (525-line = NTSC, SMPTE-C)",
                [PL_COLOR_PRIM_BT_601_625]  = "ITU-R Rec. BT.601 (625-line = PAL, SECAM)",
                [PL_COLOR_PRIM_BT_709]      = "ITU-R Rec. BT.709 (HD), also sRGB",
                [PL_COLOR_PRIM_BT_470M]     = "ITU-R Rec. BT.470 M",
                [PL_COLOR_PRIM_EBU_3213]    = "EBU Tech. 3213-E / JEDEC P22 phosphors",
                [PL_COLOR_PRIM_BT_2020]     = "ITU-R Rec. BT.2020 (UltraHD)",
                [PL_COLOR_PRIM_APPLE]       = "Apple RGB",
                [PL_COLOR_PRIM_ADOBE]       = "Adobe RGB (1998)",
                [PL_COLOR_PRIM_PRO_PHOTO]   = "ProPhoto RGB (ROMM)",
                [PL_COLOR_PRIM_CIE_1931]    = "CIE 1931 RGB primaries",
                [PL_COLOR_PRIM_DCI_P3]      = "DCI-P3 (Digital Cinema)",
                [PL_COLOR_PRIM_DISPLAY_P3]  = "DCI-P3 (Digital Cinema) with D65 white point",
                [PL_COLOR_PRIM_V_GAMUT]     = "Panasonic V-Gamut (VARICAM)",
                [PL_COLOR_PRIM_S_GAMUT]     = "Sony S-Gamut",
                [PL_COLOR_PRIM_FILM_C]      = "Traditional film primaries with Illuminant C",
                [PL_COLOR_PRIM_ACES_AP0]    = "ACES Primaries #0",
                [PL_COLOR_PRIM_ACES_AP1]    = "ACES Primaries #1",
            };

            if (target->color.primaries) {
                snprintf(buf, sizeof(buf), "Auto (%s)", primaries[target->color.primaries]);
                primaries[PL_COLOR_PRIM_UNKNOWN] = buf;
            }

            nk_label(nk, "Primaries:", NK_TEXT_LEFT);
            p->force_prim = nk_combo(nk, primaries, PL_COLOR_PRIM_COUNT, p->force_prim,
                                       16, nk_vec2(nk_widget_width(nk), 200));

            const char *transfers[PL_COLOR_TRC_COUNT] = {
                [PL_COLOR_TRC_UNKNOWN]      = "Auto (unknown SDR)",
                [PL_COLOR_TRC_BT_1886]      = "ITU-R Rec. BT.1886 (CRT emulation + OOTF)",
                [PL_COLOR_TRC_SRGB]         = "IEC 61966-2-4 sRGB (CRT emulation)",
                [PL_COLOR_TRC_LINEAR]       = "Linear light content",
                [PL_COLOR_TRC_GAMMA18]      = "Pure power gamma 1.8",
                [PL_COLOR_TRC_GAMMA20]      = "Pure power gamma 2.0",
                [PL_COLOR_TRC_GAMMA22]      = "Pure power gamma 2.2",
                [PL_COLOR_TRC_GAMMA24]      = "Pure power gamma 2.4",
                [PL_COLOR_TRC_GAMMA26]      = "Pure power gamma 2.6",
                [PL_COLOR_TRC_GAMMA28]      = "Pure power gamma 2.8",
                [PL_COLOR_TRC_PRO_PHOTO]    = "ProPhoto RGB (ROMM)",
                [PL_COLOR_TRC_ST428]        = "Digital Cinema Distribution Master (XYZ)",
                [PL_COLOR_TRC_PQ]           = "ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048",
                [PL_COLOR_TRC_HLG]          = "ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67",
                [PL_COLOR_TRC_V_LOG]        = "Panasonic V-Log (VARICAM)",
                [PL_COLOR_TRC_S_LOG1]       = "Sony S-Log1",
                [PL_COLOR_TRC_S_LOG2]       = "Sony S-Log2",
            };

            if (target->color.transfer) {
                snprintf(buf, sizeof(buf), "Auto (%s)", transfers[target->color.transfer]);
                transfers[PL_COLOR_TRC_UNKNOWN] = buf;
            }

            nk_label(nk, "Transfer:", NK_TEXT_LEFT);
            p->force_trc = nk_combo(nk, transfers, PL_COLOR_TRC_COUNT, p->force_trc,
                                      16, nk_vec2(nk_widget_width(nk), 200));

            nk_layout_row_dynamic(nk, 24, 2);
            nk_checkbox_label(nk, "Override HDR levels", &p->force_hdr_enable);

            // Ensure these values are always legal by going through
            // pl_color_space_infer
            nk_layout_row_dynamic(nk, 24, 2);
            struct pl_color_space fix = target->color;
            apply_csp_overrides(p, &fix);
            pl_color_space_infer(&fix);

            fix.hdr.min_luma *= 1000; // better value range
            nk_property_float(nk, "White point (cd/m²)",
                                10.0, &fix.hdr.max_luma, 10000.0,
                                fix.hdr.max_luma / 100, fix.hdr.max_luma / 1000);
            nk_property_float(nk, "Black point (mcd/m²)",
                                PL_COLOR_HDR_BLACK * 1000, &fix.hdr.min_luma,
                                100.0 * 1000, 5, 2);
            fix.hdr.min_luma /= 1000;
            pl_color_space_infer(&fix);
            p->force_hdr = fix.hdr;

            struct pl_color_repr *trepr = &p->force_repr;
            nk_layout_row(nk, NK_DYNAMIC, 24, 2, (float[]){ 0.3, 0.7 });

            const char *systems[PL_COLOR_SYSTEM_COUNT] = {
                [PL_COLOR_SYSTEM_UNKNOWN]       = "Auto (unknown)",
                [PL_COLOR_SYSTEM_BT_601]        = "ITU-R Rec. BT.601 (SD)",
                [PL_COLOR_SYSTEM_BT_709]        = "ITU-R Rec. BT.709 (HD)",
                [PL_COLOR_SYSTEM_SMPTE_240M]    = "SMPTE-240M",
                [PL_COLOR_SYSTEM_BT_2020_NC]    = "ITU-R Rec. BT.2020 (non-constant luminance)",
                [PL_COLOR_SYSTEM_BT_2020_C]     = "ITU-R Rec. BT.2020 (constant luminance)",
                [PL_COLOR_SYSTEM_BT_2100_PQ]    = "ITU-R Rec. BT.2100 ICtCp PQ variant",
                [PL_COLOR_SYSTEM_BT_2100_HLG]   = "ITU-R Rec. BT.2100 ICtCp HLG variant",
                [PL_COLOR_SYSTEM_DOLBYVISION]   = "Dolby Vision (invalid for output)",
                [PL_COLOR_SYSTEM_YCGCO]         = "YCgCo (derived from RGB)",
                [PL_COLOR_SYSTEM_RGB]           = "Red, Green and Blue",
                [PL_COLOR_SYSTEM_XYZ]           = "Digital Cinema Distribution Master (XYZ)",
            };

            if (target->repr.sys) {
                snprintf(buf, sizeof(buf), "Auto (%s)", systems[target->repr.sys]);
                systems[PL_COLOR_SYSTEM_UNKNOWN] = buf;
            }

            nk_label(nk, "System:", NK_TEXT_LEFT);
            trepr->sys = nk_combo(nk, systems, PL_COLOR_SYSTEM_COUNT, trepr->sys,
                                  16, nk_vec2(nk_widget_width(nk), 200));
            if (trepr->sys == PL_COLOR_SYSTEM_DOLBYVISION)
                trepr->sys = PL_COLOR_SYSTEM_UNKNOWN;

            const char *levels[PL_COLOR_LEVELS_COUNT] = {
                [PL_COLOR_LEVELS_UNKNOWN]   = "Auto (unknown)",
                [PL_COLOR_LEVELS_LIMITED]   = "Limited/TV range, e.g. 16-235",
                [PL_COLOR_LEVELS_FULL]      = "Full/PC range, e.g. 0-255",
            };

            if (target->repr.levels) {
                snprintf(buf, sizeof(buf), "Auto (%s)", levels[target->repr.levels]);
                levels[PL_COLOR_LEVELS_UNKNOWN] = buf;
            }

            nk_label(nk, "Levels:", NK_TEXT_LEFT);
            trepr->levels = nk_combo(nk, levels, PL_COLOR_LEVELS_COUNT, trepr->levels,
                                     16, nk_vec2(nk_widget_width(nk), 200));

            const char *alphas[PL_ALPHA_MODE_COUNT] = {
                [PL_ALPHA_UNKNOWN]          = "Auto (unknown, or no alpha)",
                [PL_ALPHA_INDEPENDENT]      = "Independent alpha channel",
                [PL_ALPHA_PREMULTIPLIED]    = "Premultiplied alpha channel",
            };

            if (target->repr.alpha) {
                snprintf(buf, sizeof(buf), "Auto (%s)", alphas[target->repr.alpha]);
                alphas[PL_ALPHA_UNKNOWN] = buf;
            }

            nk_label(nk, "Alpha:", NK_TEXT_LEFT);
            trepr->alpha = nk_combo(nk, alphas, PL_ALPHA_MODE_COUNT, trepr->alpha,
                                    16, nk_vec2(nk_widget_width(nk), 200));

            const struct pl_bit_encoding *bits = &target->repr.bits;
            nk_label(nk, "Bit depth:", NK_TEXT_LEFT);
            auto_property_int(nk, bits->color_depth, 0,
                              &trepr->bits.color_depth, 16, 1, 0);

            if (bits->color_depth != bits->sample_depth) {
                nk_label(nk, "Sample bit depth:", NK_TEXT_LEFT);
                auto_property_int(nk, bits->sample_depth, 0,
                                  &trepr->bits.sample_depth, 16, 1, 0);
            } else {
                // Adjust these two fields in unison
                trepr->bits.sample_depth = trepr->bits.color_depth;
            }

            if (bits->bit_shift) {
                nk_label(nk, "Bit shift:", NK_TEXT_LEFT);
                auto_property_int(nk, bits->bit_shift, 0,
                                  &trepr->bits.bit_shift, 16, 1, 0);
            } else {
                trepr->bits.bit_shift = 0;
            }

            nk_layout_row_dynamic(nk, 24, 1);
            nk_checkbox_label(nk, "Forward input color space to display", &p->colorspace_hint);

            if (p->colorspace_hint && !p->force_hdr_enable) {
                nk_checkbox_label(nk, "Forward dynamic brightness changes to display",
                                  &p->colorspace_hint_dynamic);
            }

            nk_layout_row_dynamic(nk, 50, 1);
            if (ui_widget_hover(nk, "Drop ICC profile here...") && dropped_file) {
                uint8_t *iccbuf;
                size_t size;
                int ret = av_file_map(dropped_file, &iccbuf, &size, 0, NULL);
                if (ret < 0) {
                    fprintf(stderr, "Failed opening '%s': %s\n", dropped_file,
                            av_err2str(ret));
                } else {
                    av_file_unmap((void *) p->target_icc.data, p->target_icc.len);
                    p->target_icc.data = iccbuf;
                    p->target_icc.len = size;
                    p->target_icc.signature++;
                    free(p->target_icc_name);
                    p->target_icc_name = strdup(PL_BASENAME((char *) dropped_file));
                }
            }

            if (p->target_icc.len) {
                nk_layout_row_dynamic(nk, 24, 2);
                nk_labelf(nk, NK_TEXT_LEFT, "Loaded: %s",
                          p->target_icc_name ? p->target_icc_name : "(unknown)");
                reset_icc |= nk_button_label(nk, "Reset ICC");
                nk_checkbox_label(nk, "Force BPC", &iccpar->force_bpc);
                nk_checkbox_label(nk, "Use detected luminance", &p->use_icc_luma);
            }

            // Apply the reset last to prevent the UI from flashing for a frame
            if (reset) {
                p->force_repr = (struct pl_color_repr) {0};
                p->force_prim = PL_COLOR_PRIM_UNKNOWN;
                p->force_trc = PL_COLOR_TRC_UNKNOWN;
                p->force_hdr = (struct pl_hdr_metadata) {0};
                p->force_hdr_enable = false;
            }

            if (reset_icc && p->target_icc.len) {
                av_file_unmap((void *) p->target_icc.data, p->target_icc.len);
                free(p->target_icc_name);
                p->target_icc_name = NULL;
                p->target_icc = (struct pl_icc_profile) {
                    .signature = p->target_icc.signature + 1,
                };
            }

            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Custom shaders", NK_MINIMIZED)) {

            nk_layout_row_dynamic(nk, 50, 1);
            if (ui_widget_hover(nk, "Drop .hook/.glsl files here...") && dropped_file) {
                uint8_t *buf;
                size_t size;
                int ret = av_file_map(dropped_file, &buf, &size, 0, NULL);
                if (ret < 0) {
                    fprintf(stderr, "Failed opening '%s': %s\n", dropped_file,
                            av_err2str(ret));
                } else {
                    const struct pl_hook *hook;
                    hook = pl_mpv_user_shader_parse(p->win->gpu, (char *) buf, size);
                    av_file_unmap(buf, size);
                    add_hook(p, hook, dropped_file);
                }
            }

            const float px = 24.0;
            nk_layout_row_template_begin(nk, px);
            nk_layout_row_template_push_static(nk, px);
            nk_layout_row_template_push_static(nk, px);
            nk_layout_row_template_push_static(nk, px);
            nk_layout_row_template_push_dynamic(nk);
            nk_layout_row_template_end(nk);
            for (int i = 0; i < p->shader_num; i++) {

                if (i == 0) {
                    nk_label(nk, "·", NK_TEXT_CENTERED);
                } else if (nk_button_symbol(nk, NK_SYMBOL_TRIANGLE_UP)) {
                    const struct pl_hook *prev_hook = p->shader_hooks[i - 1];
                    char *prev_path = p->shader_paths[i - 1];
                    p->shader_hooks[i - 1] = p->shader_hooks[i];
                    p->shader_paths[i - 1] = p->shader_paths[i];
                    p->shader_hooks[i] = prev_hook;
                    p->shader_paths[i] = prev_path;
                }

                if (i == p->shader_num - 1) {
                    nk_label(nk, "·", NK_TEXT_CENTERED);
                } else if (nk_button_symbol(nk, NK_SYMBOL_TRIANGLE_DOWN)) {
                    const struct pl_hook *next_hook = p->shader_hooks[i + 1];
                    char *next_path = p->shader_paths[i + 1];
                    p->shader_hooks[i + 1] = p->shader_hooks[i];
                    p->shader_paths[i + 1] = p->shader_paths[i];
                    p->shader_hooks[i] = next_hook;
                    p->shader_paths[i] = next_path;
                }

                if (nk_button_symbol(nk, NK_SYMBOL_X)) {
                    pl_mpv_user_shader_destroy(&p->shader_hooks[i]);
                    free(p->shader_paths[i]);
                    p->shader_num--;
                    memmove(&p->shader_hooks[i], &p->shader_hooks[i+1],
                            (p->shader_num - i) * sizeof(void *));
                    memmove(&p->shader_paths[i], &p->shader_paths[i+1],
                            (p->shader_num - i) * sizeof(char *));
                    if (i == p->shader_num)
                        break;
                }

                if (p->shader_hooks[i]->num_parameters == 0) {
                    nk_label(nk, p->shader_paths[i], NK_TEXT_LEFT);
                    continue;
                }

                if (nk_combo_begin_label(nk, p->shader_paths[i], nk_vec2(nk_widget_width(nk), 500))) {
                    nk_layout_row_dynamic(nk, 32, 1);
                    for (int j = 0; j < p->shader_hooks[i]->num_parameters; j++) {
                        const struct pl_hook_par *hp = &p->shader_hooks[i]->parameters[j];
                        const char *name = hp->description ? hp->description : hp->name;
                        switch (hp->type) {
                        case PL_VAR_FLOAT:
                            nk_property_float(nk, name, hp->minimum.f,
                                              &hp->data->f, hp->maximum.f,
                                              hp->data->f / 100.0f,
                                              hp->data->f / 1000.0f);
                            break;
                        case PL_VAR_SINT:
                            nk_property_int(nk, name, hp->minimum.i,
                                            &hp->data->i, hp->maximum.i,
                                            1, 1.0f);
                            break;
                        case PL_VAR_UINT: {
                            int min = MIN(hp->minimum.u, INT_MAX);
                            int max = MIN(hp->maximum.u, INT_MAX);
                            int val = MIN(hp->data->u, INT_MAX);
                            nk_property_int(nk, name, min, &val, max, 1, 1);
                            hp->data->u = val;
                            break;
                        }
                        default: abort();
                        }
                    }
                    nk_combo_end(nk);
                }
            }

            par->hooks = p->shader_hooks;
            par->num_hooks = p->shader_num;
            nk_tree_pop(nk);
        }

        if (nk_tree_push(nk, NK_TREE_NODE, "Debug", NK_MINIMIZED)) {
            nk_layout_row_dynamic(nk, 24, 1);
            nk_checkbox_label(nk, "Preserve mixing cache", &par->preserve_mixing_cache);
            nk_checkbox_label(nk, "Disable linear scaling", &par->disable_linear_scaling);
            nk_checkbox_label(nk, "Disable built-in scalers", &par->disable_builtin_scalers);
            nk_checkbox_label(nk, "Force-enable dither", &par->force_dither);
            nk_checkbox_label(nk, "Disable gamma-aware dither", &par->disable_dither_gamma_correction);
            nk_checkbox_label(nk, "Disable FBOs / advanced rendering", &par->disable_fbos);
            nk_checkbox_label(nk, "Force low-bit depth FBOs", &par->force_low_bit_depth_fbos);
            nk_checkbox_label(nk, "Disable constant hard-coding", &par->dynamic_constants);
            nk_checkbox_label(nk, "Ignore ICC profiles", &par->ignore_icc_profiles);

            if (nk_check_label(nk, "Ignore Dolby Vision metadata", p->ignore_dovi) != p->ignore_dovi) {
                // Flush the renderer cache on changes, since this can
                // drastically alter the subjective appearance of the stream
                pl_renderer_flush_cache(p->renderer);
                p->ignore_dovi = !p->ignore_dovi;
            }

            nk_layout_row_dynamic(nk, 24, 2);
            if (nk_button_label(nk, "Flush renderer cache"))
                pl_renderer_flush_cache(p->renderer);
            if (nk_button_label(nk, "Recreate renderer")) {
                pl_renderer_destroy(&p->renderer);
                p->renderer = pl_renderer_create(p->log, p->win->gpu);
            }

            if (nk_tree_push(nk, NK_TREE_NODE, "Shader passes / GPU timing", NK_MINIMIZED)) {
                nk_layout_row_dynamic(nk, 26, 1);
                nk_label(nk, "Full frames:", NK_TEXT_LEFT);
                for (int i = 0; i < p->num_frame_passes; i++)
                    draw_shader_pass(nk, &p->frame_info[i]);

                nk_layout_row_dynamic(nk, 26, 1);
                nk_label(nk, "Output blending:", NK_TEXT_LEFT);
                for (int j = 0; j < MAX_BLEND_FRAMES; j++) {
                    for (int i = 0; i < p->num_blend_passes[j]; i++)
                        draw_shader_pass(nk, &p->blend_info[j][i]);
                }

                nk_tree_pop(nk);
            }

            if (nk_tree_push(nk, NK_TREE_NODE, "Frame statistics / CPU timing", NK_MINIMIZED)) {
                nk_layout_row_dynamic(nk, 24, 2);
                nk_label(nk, "Current PTS:", NK_TEXT_LEFT);
                nk_labelf(nk, NK_TEXT_LEFT, "%.3f", p->stats.current_pts);
                nk_label(nk, "Frames rendered:", NK_TEXT_LEFT);
                nk_labelf(nk, NK_TEXT_LEFT, "%"PRIu32, p->stats.rendered);
                nk_label(nk, "Decoded frames", NK_TEXT_LEFT);
                nk_labelf(nk, NK_TEXT_LEFT, "%"PRIu32, atomic_load(&p->stats.decoded));
                nk_label(nk, "Dropped frames:", NK_TEXT_LEFT);
                nk_labelf(nk, NK_TEXT_LEFT, "%"PRIu32, p->stats.dropped);
                nk_label(nk, "Times stalled:", NK_TEXT_LEFT);
                nk_labelf(nk, NK_TEXT_LEFT, "%"PRIu32" (%.3f ms)",
                          p->stats.stalled, p->stats.stalled_ms);
                draw_timing(nk, "Acquire FBO:", &p->stats.acquire);
                draw_timing(nk, "Update queue:", &p->stats.update);
                draw_timing(nk, "Render frame:", &p->stats.render);
                draw_timing(nk, "Draw interface:", &p->stats.draw_ui);
                draw_timing(nk, "Submit frame:", &p->stats.submit);
                draw_timing(nk, "Swap buffers:", &p->stats.swap);
                draw_timing(nk, "Vsync interval:", &p->stats.vsync_interval);
                draw_timing(nk, "PTS interval:", &p->stats.pts_interval);

                if (nk_button_label(nk, "Reset statistics"))
                    memset(&p->stats, 0, sizeof(p->stats));
                nk_tree_pop(nk);
            }

            nk_tree_pop(nk);
        }
    }
    nk_end(nk);
}

#else
static void update_settings(struct plplay *p, const struct pl_frame *target) { }
#endif // HAVE_NUKLEAR
