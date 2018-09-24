//------------------------------------------------------------------------------
//  modplay-sapp.c
//  sokol_app + sokol_audio + libmodplug
//------------------------------------------------------------------------------
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_audio.h"
#include "modplug.h"
#include "data/mods.h"
#include <assert.h>

/* select between mono (1) and stereo (2) */
#define MODPLAY_NUM_CHANNELS (2)
/* use stream callback (0) or push-from-mainthread (1) model */
#define MODPLAY_USE_PUSH (0)
/* big enough for packet_size * num_packets * num_channels */
#define MODPLAY_SRCBUF_SAMPLES (16*1024)

static bool mpf_valid = false;
static ModPlugFile* mpf;

/* integer-to-float conversion buffer */
static int int_buf[MODPLAY_SRCBUF_SAMPLES];
#if MODPLAY_USE_PUSH
static float flt_buf[MODPLAY_SRCBUF_SAMPLES];
#endif

#if !MODPLAY_USE_PUSH
static void stream_cb(float* buffer, int num_samples) {
    assert(num_samples < MODPLAY_SRCBUF_SAMPLES);
    if (mpf_valid) {
        /* read sampled from libmodplug, and convert to float, left/right
           channel are interleaved
        */
        int res = ModPlug_Read(mpf, (void*)int_buf, sizeof(int)*num_samples);
        int samples_in_buffer = res / sizeof(int);
        int i;
        for (i = 0; i < samples_in_buffer; i++) {
            buffer[i] = int_buf[i] / (float)0x7fffffff;
        }
        for (; i < num_samples; i++) {
            buffer[i] = 0.0f;
        }
    }
    else {
        /* if file wasn't loaded, fill the output buffer with silence */
        for (int i = 0; i < num_samples; i++) {
            buffer[i] = 0.0f;
        }
    }
}
#endif

void init(void) {
    /* setup sokol_gfx */
    sg_setup(&(sg_desc){
        .mtl_device = sapp_metal_get_device(),
        .mtl_renderpass_descriptor_cb = sapp_metal_get_renderpass_descriptor,
        .mtl_drawable_cb = sapp_metal_get_drawable,
        .d3d11_device = sapp_d3d11_get_device(),
        .d3d11_device_context = sapp_d3d11_get_device_context(),
        .d3d11_render_target_view_cb = sapp_d3d11_get_render_target_view,
        .d3d11_depth_stencil_view_cb = sapp_d3d11_get_depth_stencil_view
    });

    /* setup sokol_audio (default sample rate is 44100Hz) */
    saudio_setup(&(saudio_desc){
        .num_channels = MODPLAY_NUM_CHANNELS,
        #if !MODPLAY_USE_PUSH
        .stream_cb = stream_cb
        #endif
    });

    /* setup libmodplug and load mod from embedded C array */
    ModPlug_Settings mps;
    ModPlug_GetSettings(&mps);
    mps.mChannels = saudio_channels();
    mps.mBits = 32;
    mps.mFrequency = saudio_sample_rate();
    mps.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
    mps.mMaxMixChannels = 64;
    mps.mLoopCount = -1; /* loop play seems to be disabled in current libmodplug */
    mps.mFlags = MODPLUG_ENABLE_OVERSAMPLING;
    ModPlug_SetSettings(&mps);

    mpf = ModPlug_Load(dump_disco_feva_baby, sizeof(dump_disco_feva_baby));
    if (mpf) {
        mpf_valid = true;
    }
}

void frame(void) {
    #if MODPLAY_USE_PUSH
        int num_frames = saudio_expect();
        if (num_frames > 0) {
            int num_samples = num_frames * MODPLAY_NUM_CHANNELS;
            assert(num_samples <= MODPLAY_SRCBUF_SAMPLES);
            if (mpf_valid) {
                int res = ModPlug_Read(mpf, (void*)int_buf, sizeof(int)*num_samples);
                int samples_in_buffer = res / sizeof(int);
                int i;
                for (i = 0; i < samples_in_buffer; i++) {
                    flt_buf[i] = int_buf[i] / (float)0x7fffffff;
                }
                for (; i < num_samples; i++) {
                    flt_buf[i] = 0.0f;
                }
            }
            else {
                for (int i = 0; i < num_samples; i++) {
                    flt_buf[i] = 0.0f;
                }
            }
            saudio_push(flt_buf, num_frames);
        }
    #endif
    sg_pass_action pass_action = {
        .colors[0] = { .action = SG_ACTION_CLEAR, .val = { 0.5f, 1.0f, 0.0f, 1.0f } }
    };
    sg_begin_default_pass(&pass_action, sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
}

void cleanup(void) {
    saudio_shutdown();
    if (mpf_valid) {
        ModPlug_Unload(mpf);
    }
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .width = 400,
        .height = 300,
        .window_title = "Sokol Audio + LibModPlug",
    };
}