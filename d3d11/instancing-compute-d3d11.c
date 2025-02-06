//------------------------------------------------------------------------------
//  instancing-compute-d3d11.c
//------------------------------------------------------------------------------
#include "d3d11entry.h"
#define SOKOL_IMPL
#define SOKOL_D3D11
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_log.h"
#define HANDMADE_MATH_IMPLEMENTATION
#define HANDMADE_MATH_NO_SSE
#include "HandmadeMath.h"

#define MAX_PARTICLES (512 * 1024)
#define NUM_PARTICLES_EMITTED_PER_FRAME (10)

typedef struct {
    uint64_t last_time;
    int num_particles;
    float ry;
    struct {
        sg_buffer buf;
        sg_pipeline pip;
    } compute;
    struct {
        sg_buffer vbuf;
        sg_buffer ibuf;
        sg_pipeline pip;
        sg_pass_action pass_action;
    } display;
} state_t;

typedef struct {
    float dt;
    uint32_t num_particles;
} cs_params_t;

typedef struct {
    hmm_mat4 mvp;
} vs_params_t;

typedef struct {
    float pos[4];
    float vel[4];
} particle_t;

static inline uint32_t xorshift32(void) {
    static uint32_t x = 0x12345678;
    x ^= x<<13;
    x ^= x>>17;
    x ^= x<<5;
    return x;
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    
    state_t state = {
        .display.pass_action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 1} },
        },
    };
    
    d3d11_init(&(d3d11_desc_t){
        .width = 640,
        .height = 480,
        .sample_count = 4,
        .title = L"instancing-compute-d3d11.c"
    });

    sg_setup(&(sg_desc){
        .environment = d3d11_environment(),
        .logger.func = slog_func,
    });
    stm_setup();

    // a storage buffer which holds the particle positions and velocities,
    // updated by a compute shader
    {
        particle_t* p = calloc(MAX_PARTICLES, sizeof(particle_t));
        for (size_t i = 0; i < MAX_PARTICLES; i++) {
            p[i].vel[0] = ((float)(xorshift32() & 0x7FFF) / 0x7FFF) - 0.5f;
            p[i].vel[1] = ((float)(xorshift32() & 0x7FFF) / 0x7FFF) * 0.5f + 2.0f;
            p[i].vel[2] = ((float)(xorshift32() & 0x7FFF) / 0x7FFF) - 0.5f;
        }
        state.compute.buf = sg_make_buffer(&(sg_buffer_desc){
            .type = SG_BUFFERTYPE_STORAGEBUFFER,
            .data = {
                .ptr = p,
                .size = MAX_PARTICLES * sizeof(particle_t),
            },
            .label = "particle-buffer",
        });
        free(p);
    }

    // create compute shader
    sg_shader compute_shd = sg_make_shader(&(sg_shader_desc){
        .compute_func.source =
            "cbuffer params: register(b0) {\n"
            "  float dt;\n"
            "  uint num_particles;\n"
            "};\n"
            "RWByteAddressBuffer particles: register(u0);\n"
            "[numthreads(64,1,1)] void main(uint3 dtid: SV_DispatchThreadID) {\n"
            "  uint idx = dtid.x;\n"
            "  if (idx >= num_particles) {\n"
            "    return;\n"
            "  }\n"
            "  uint pos_offset = idx * 32 + 0;\n"
            "  uint vel_offset = idx * 32 + 16;\n"
            "  float4 pos = asfloat(particles.Load4(pos_offset));\n"
            "  float4 vel = asfloat(particles.Load4(vel_offset));\n"
            "  vel.y -= 1.0 * dt;\n"
            "  pos += vel * dt;\n"
            "  if (pos.y < 2.0) {\n"
            "    pos.y = 1.8;\n"
            "    vel *= float4(0.8, -0.8, 0.8, 0);\n"
            "  }\n"
            "  particles.Store4(pos_offset, asuint(pos));\n"
            "  particles.Store4(vel_offset, asuint(vel));\n"
            "}\n",
        .compute_workgroup_size = { .x = 64 },
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_COMPUTE,
            .size = sizeof(cs_params_t),
            .hlsl_register_b_n = 0,
        },
        .storage_buffers[0] = {
            .stage = SG_SHADERSTAGE_COMPUTE,
            .readonly = false,
            .hlsl_register_u_n = 0,
        },
        .label = "compute-shader",
    });
    
    // create a compute pipeline object
    state.compute.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .compute = true,
        .shader = compute_shd,
        .label = "compute-pipeline",
    });

    // vertex buffer for static geometry, goes into vertex-buffer-slot 0
    const float r = 0.05f;
    const float vertices[] = {
        // positions            colors
        0.0f,   -r, 0.0f,       1.0f, 0.0f, 0.0f, 1.0f,
           r, 0.0f, r,          0.0f, 1.0f, 0.0f, 1.0f,
           r, 0.0f, -r,         0.0f, 0.0f, 1.0f, 1.0f,
          -r, 0.0f, -r,         1.0f, 1.0f, 0.0f, 1.0f,
          -r, 0.0f, r,          0.0f, 1.0f, 1.0f, 1.0f,
        0.0f,    r, 0.0f,       1.0f, 0.0f, 1.0f, 1.0f
    };
    state.display.vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(vertices),
        .label = "geometry-vbuf",
    });

    // index buffer for static geometry
    const uint16_t indices[] = {
        0, 1, 2,    0, 2, 3,    0, 3, 4,    0, 4, 1,
        5, 1, 2,    5, 2, 3,    5, 3, 4,    5, 4, 1
    };
    state.display.ibuf = sg_make_buffer(&(sg_buffer_desc){
        .type = SG_BUFFERTYPE_INDEXBUFFER,
        .data = SG_RANGE(indices),
        .label = "geometry-ibuf",
    });
    
    // shader for rendering with instance position pulled from the storage buffer
    sg_shader display_shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source =
            "FIXME",
        .fragment_func.source =
            "FIXME",
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size = sizeof(vs_params_t),
            .hlsl_register_b_n = 0,
        },
        .storage_buffers[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .readonly = true,
            .hlsl_register_t_n = 0,
        },
        .label = "render-shader",
    });
    
    // a pipeline object for rendering, this uses geometry from
    // traditional vertex buffer, but pull per-instance positions
    // from the compute-update storage buffer
    state.display.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .layout = {
            .attrs = {
                [0] = { .format=SG_VERTEXFORMAT_FLOAT3 },  // position
                [1] = { .format=SG_VERTEXFORMAT_FLOAT4 },  // color
            }
        },
        .shader = display_shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .depth = {
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
            .write_enabled = true,
        },
        .cull_mode = SG_CULLMODE_BACK,
        .label = "render-pipeline",
    });
    
    // frame loop
    while (d3d11_process_events()) {
        state.num_particles += NUM_PARTICLES_EMITTED_PER_FRAME;
        if (state.num_particles > MAX_PARTICLES) {
            state.num_particles = MAX_PARTICLES;
        }
    
        // compute pass to update the particle positions
        const cs_params_t cs_params = {
            .dt = (float)stm_sec(stm_laptime(&state.last_time)),
            .num_particles = (uint32_t)state.num_particles,
        };
        sg_begin_pass(&(sg_pass){ .compute = true, .label = "compute-pass" });
        sg_apply_pipeline(state.compute.pip);
        sg_apply_bindings(&(sg_bindings){
            .storage_buffers[0] = state.compute.buf
        });
        sg_apply_uniforms(0, &SG_RANGE(cs_params));
        sg_dispatch((state.num_particles + 63)/64, 1, 1);
        sg_end_pass();

        // render pass to render instanced geometry, with the instance-positions
        // pulled from the compute-updated storage buffer
        state.ry += 1;
        const hmm_mat4 proj = HMM_Perspective(60.0f, (float)d3d11_width()/(float)d3d11_height(), 0.01f, 50.0f);
        const hmm_mat4 view = HMM_LookAt(HMM_Vec3(0.0f, 1.5f, 12.0f), HMM_Vec3(0.0f, 0.0f, 0.0f), HMM_Vec3(0.0f, 1.0f, 0.0f));
        const hmm_mat4 view_proj = HMM_MultiplyMat4(proj, view);
        const vs_params_t vs_params = {
            .mvp = HMM_MultiplyMat4(view_proj, HMM_Rotate(state.ry, HMM_Vec3(0.0f, 1.0f, 0.0f)))
        };
        sg_begin_pass(&(sg_pass){
            .action = state.display.pass_action,
            .swapchain = d3d11_swapchain(),
            .label = "render-pass",
        });
        sg_apply_pipeline(state.display.pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = state.display.vbuf,
            .index_buffer = state.display.ibuf,
            .storage_buffers[0] = state.compute.buf,
        });
        sg_apply_uniforms(0, &SG_RANGE(vs_params));
        sg_draw(0, 24, state.num_particles);
        sg_end_pass();
        sg_commit();
    }
    sg_shutdown();
}
