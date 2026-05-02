#include "gs_video_shader_renderer.h"

#include <GLES2/gl2.h>

#include "Log.h"
#include "gs_shared_state.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
constexpr char kVideoVertexShader[] = R"glsl(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)glsl";

enum ShaderFeature : uint8_t
{
    kShaderFeatureLens = 1u << 0,
    kShaderFeatureStabilization = 1u << 1,
    kShaderFeatureDeblocking = 1u << 2,
    kShaderFeatureDebanding = 1u << 3,
    kShaderFeatureDithering = 1u << 4,
};

constexpr uint8_t kShaderFeatureCount = 32;

constexpr char kComposedFragmentShader[] = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform sampler2D uTexture;
uniform vec2 uUv0;
uniform vec2 uUv1;
#if GS_NEEDS_FRAME_SIZE
uniform vec2 uFrameSize;
#endif
#if GS_FEATURE_LENS
uniform vec3 uRadial;
uniform vec2 uTangential;
uniform vec2 uFocalNorm;
uniform vec2 uPrincipalNorm;
#endif
#if GS_FEATURE_STABILIZATION
uniform vec3 uStabilizationInv0;
uniform vec3 uStabilizationInv1;
#endif
#if GS_FEATURE_DEBLOCKING
uniform vec4 uDeblockParams;
#endif
#if GS_FEATURE_DEBANDING
uniform vec3 uDebandParams;
#endif
#if GS_FEATURE_DITHERING
uniform vec2 uDitherParams;
#endif
varying vec2 vTexCoord;

vec2 normalizedVideoCoord()
{
    vec2 range = uUv1 - uUv0;
    range.x = abs(range.x) < 0.0001 ? 0.0001 : range.x;
    range.y = abs(range.y) < 0.0001 ? 0.0001 : range.y;
    return (vTexCoord - uUv0) / range;
}

bool inVideoFrame(vec2 coord)
{
    return coord.x >= 0.0 && coord.x <= 1.0 && coord.y >= 0.0 && coord.y <= 1.0;
}

vec3 sampleVideo(vec2 coord)
{
    return texture2D(uTexture, mix(uUv0, uUv1, coord)).rgb;
}

#if GS_FEATURE_LENS
vec2 applyLensCorrection(vec2 normalized_coord)
{
    vec2 focal = max(uFocalNorm, vec2(0.0001, 0.0001));
    vec2 p = (normalized_coord - uPrincipalNorm) / focal;
    float r2 = dot(p, p);
    float r4 = r2 * r2;
    float r6 = r4 * r2;
    float radial = 1.0 + uRadial.x * r2 + uRadial.y * r4 + uRadial.z * r6;
    float xy2 = 2.0 * p.x * p.y;
    vec2 corrected = vec2(
        p.x * radial + uTangential.x * xy2 + uTangential.y * (r2 + 2.0 * p.x * p.x),
        p.y * radial + uTangential.x * (r2 + 2.0 * p.y * p.y) + uTangential.y * xy2);
    return corrected * focal + uPrincipalNorm;
}
#endif

#if GS_FEATURE_STABILIZATION
vec2 applyStabilization(vec2 sample_coord)
{
    vec2 pixel_coord = sample_coord * uFrameSize;
    return vec2(
        dot(uStabilizationInv0, vec3(pixel_coord, 1.0)) / uFrameSize.x,
        dot(uStabilizationInv1, vec3(pixel_coord, 1.0)) / uFrameSize.y);
}
#endif

#if GS_FEATURE_DEBLOCKING || GS_FEATURE_DEBANDING || GS_FEATURE_DITHERING
float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}
#endif

#if GS_FEATURE_DITHERING
float hashNoise(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
#endif

#if GS_FEATURE_DEBLOCKING
vec3 applyDeblocking(vec2 sample_coord, vec3 color)
{
    vec2 px = 1.0 / max(uFrameSize, vec2(1.0, 1.0));
    vec2 pixel = floor(sample_coord * uFrameSize);
    vec3 result = color;
    float block_x = mod(pixel.x, 8.0);
    if (block_x < 0.5 || block_x > 6.5)
    {
        bool q_side = block_x < 0.5;
        vec2 p0_coord = sample_coord + vec2(q_side ? -px.x : 0.0, 0.0);
        vec2 q0_coord = sample_coord + vec2(q_side ? 0.0 : px.x, 0.0);
        vec2 p1_coord = sample_coord + vec2(q_side ? -2.0 * px.x : -px.x, 0.0);
        vec2 q1_coord = sample_coord + vec2(q_side ? px.x : 2.0 * px.x, 0.0);
        if (inVideoFrame(p1_coord) && inVideoFrame(q1_coord))
        {
            vec3 p1c = sampleVideo(p1_coord);
            vec3 p0c = sampleVideo(p0_coord);
            vec3 q0c = sampleVideo(q0_coord);
            vec3 q1c = sampleVideo(q1_coord);
            float p1 = luma(p1c);
            float p0 = luma(p0c);
            float q0 = luma(q0c);
            float q1 = luma(q1c);
            float edge = abs(q0 - p0);
            float flatness = max(abs(p0 - p1), abs(q1 - q0));
            if (edge < uDeblockParams.y && flatness < uDeblockParams.z)
            {
                float weight = uDeblockParams.x *
                               (1.0 - smoothstep(uDeblockParams.y * 0.35, uDeblockParams.y, edge)) *
                               (1.0 - smoothstep(uDeblockParams.z * 0.35, uDeblockParams.z, flatness));
                vec3 across = q_side ? p0c : q0c;
                vec3 local = (p1c + p0c + q0c + q1c) * 0.25;
                result = mix(result, mix(across, local, 0.35), clamp(weight, 0.0, uDeblockParams.w));
            }
        }
    }

    float block_y = mod(pixel.y, 8.0);
    if (block_y < 0.5 || block_y > 6.5)
    {
        bool q_side = block_y < 0.5;
        vec2 p0_coord = sample_coord + vec2(0.0, q_side ? -px.y : 0.0);
        vec2 q0_coord = sample_coord + vec2(0.0, q_side ? 0.0 : px.y);
        vec2 p1_coord = sample_coord + vec2(0.0, q_side ? -2.0 * px.y : -px.y);
        vec2 q1_coord = sample_coord + vec2(0.0, q_side ? px.y : 2.0 * px.y);
        if (inVideoFrame(p1_coord) && inVideoFrame(q1_coord))
        {
            vec3 p1c = sampleVideo(p1_coord);
            vec3 p0c = sampleVideo(p0_coord);
            vec3 q0c = sampleVideo(q0_coord);
            vec3 q1c = sampleVideo(q1_coord);
            float p1 = luma(p1c);
            float p0 = luma(p0c);
            float q0 = luma(q0c);
            float q1 = luma(q1c);
            float edge = abs(q0 - p0);
            float flatness = max(abs(p0 - p1), abs(q1 - q0));
            if (edge < uDeblockParams.y && flatness < uDeblockParams.z)
            {
                float weight = uDeblockParams.x *
                               (1.0 - smoothstep(uDeblockParams.y * 0.35, uDeblockParams.y, edge)) *
                               (1.0 - smoothstep(uDeblockParams.z * 0.35, uDeblockParams.z, flatness));
                vec3 across = q_side ? p0c : q0c;
                vec3 local = (p1c + p0c + q0c + q1c) * 0.25;
                result = mix(result, mix(across, local, 0.35), clamp(weight, 0.0, uDeblockParams.w));
            }
        }
    }

    return clamp(result, 0.0, 1.0);
}
#endif

#if GS_FEATURE_DEBANDING
vec3 applyDebanding(vec2 sample_coord, vec3 color)
{
    vec2 px = 1.0 / max(uFrameSize, vec2(1.0, 1.0));
    float center = luma(color);
    vec2 left_coord = max(sample_coord - vec2(px.x, 0.0), vec2(0.0, 0.0));
    vec2 right_coord = min(sample_coord + vec2(px.x, 0.0), vec2(1.0, 1.0));
    vec2 up_coord = max(sample_coord - vec2(0.0, px.y), vec2(0.0, 0.0));
    vec2 down_coord = min(sample_coord + vec2(0.0, px.y), vec2(1.0, 1.0));
    vec3 left = sampleVideo(left_coord);
    vec3 right = sampleVideo(right_coord);
    vec3 up = sampleVideo(up_coord);
    vec3 down = sampleVideo(down_coord);
    float local_gradient = max(abs(center - luma(left)) + abs(center - luma(right)),
                               abs(center - luma(up)) + abs(center - luma(down))) * 0.5;
    float flat_area = 1.0 - smoothstep(uDebandParams.y * 0.5, uDebandParams.y, local_gradient);
    if (flat_area <= 0.0)
    {
        return color;
    }

    vec3 left2 = sampleVideo(max(sample_coord - vec2(px.x * 2.0, 0.0), vec2(0.0, 0.0)));
    vec3 right2 = sampleVideo(min(sample_coord + vec2(px.x * 2.0, 0.0), vec2(1.0, 1.0)));
    vec3 up2 = sampleVideo(max(sample_coord - vec2(0.0, px.y * 2.0), vec2(0.0, 0.0)));
    vec3 down2 = sampleVideo(min(sample_coord + vec2(0.0, px.y * 2.0), vec2(1.0, 1.0)));
    float w_left = 1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(left)));
    float w_right = 1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(right)));
    float w_up = 1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(up)));
    float w_down = 1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(down)));
    float w_left2 = 0.55 * (1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(left2))));
    float w_right2 = 0.55 * (1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(right2))));
    float w_up2 = 0.55 * (1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(up2))));
    float w_down2 = 0.55 * (1.0 - smoothstep(uDebandParams.z * 0.5, uDebandParams.z, abs(center - luma(down2))));
    vec3 smoothed = color +
                    left * w_left + right * w_right + up * w_up + down * w_down +
                    left2 * w_left2 + right2 * w_right2 + up2 * w_up2 + down2 * w_down2;
    float total_weight = 1.0 + w_left + w_right + w_up + w_down + w_left2 + w_right2 + w_up2 + w_down2;
    smoothed /= max(total_weight, 0.0001);
    return clamp(mix(color, smoothed, clamp(uDebandParams.x * flat_area, 0.0, 1.0)), 0.0, 1.0);
}
#endif

#if GS_FEATURE_DITHERING
vec3 applyDithering(vec2 sample_coord, vec3 color)
{
    vec2 px = 1.0 / max(uFrameSize, vec2(1.0, 1.0));
    float g = max(abs(luma(color) - luma(sampleVideo(min(sample_coord + vec2(px.x, 0.0), vec2(1.0, 1.0))))),
                  abs(luma(color) - luma(sampleVideo(min(sample_coord + vec2(0.0, px.y), vec2(1.0, 1.0))))));
    float flat_area = 1.0 - smoothstep(uDitherParams.y * 0.5, uDitherParams.y, g);
    float n = hashNoise(gl_FragCoord.xy + vec2(luma(color) * 31.0, luma(color) * 17.0)) - 0.5;
    return clamp(color + vec3(n * uDitherParams.x * flat_area), 0.0, 1.0);
}
#endif

void main()
{
    vec2 sample_coord = normalizedVideoCoord();
#if GS_FEATURE_LENS
    sample_coord = applyLensCorrection(sample_coord);
#endif
#if GS_FEATURE_STABILIZATION
    sample_coord = applyStabilization(sample_coord);
#endif
    if (!inVideoFrame(sample_coord))
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 color = sampleVideo(sample_coord);
#if GS_FEATURE_DEBLOCKING
    color = applyDeblocking(sample_coord, color);
#endif
#if GS_FEATURE_DEBANDING
    color = applyDebanding(sample_coord, color);
#endif
#if GS_FEATURE_DITHERING
    color = applyDithering(sample_coord, color);
#endif
    gl_FragColor = vec4(color, 1.0);
}
)glsl";

//===================================================================================
//===================================================================================
// Builds a fragment shader by enabling only the fragments needed by this feature mask.
std::string buildFragmentShader(uint8_t features)
{
    const bool needs_frame_size = (features & (kShaderFeatureStabilization |
                                               kShaderFeatureDeblocking |
                                               kShaderFeatureDebanding |
                                               kShaderFeatureDithering)) != 0;
    std::string source;
    source.reserve(12000);
    source += "#define GS_FEATURE_LENS ";
    source += (features & kShaderFeatureLens) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_STABILIZATION ";
    source += (features & kShaderFeatureStabilization) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_DEBLOCKING ";
    source += (features & kShaderFeatureDeblocking) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_DEBANDING ";
    source += (features & kShaderFeatureDebanding) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_DITHERING ";
    source += (features & kShaderFeatureDithering) != 0 ? "1\n" : "0\n";
    source += "#define GS_NEEDS_FRAME_SIZE ";
    source += needs_frame_size ? "1\n" : "0\n";
    source += kComposedFragmentShader;
    return source;
}

//===================================================================================
//===================================================================================
// Compiles one GLES shader and logs driver errors.
GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Video shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

//===================================================================================
//===================================================================================
// Links a GLES program from static vertex and fragment shader sources.
GLuint createProgram(const char* fragment_source)
{
    const GLuint vertex_shader = compileShader(GL_VERTEX_SHADER, kVideoVertexShader);
    const GLuint fragment_shader = compileShader(GL_FRAGMENT_SHADER, fragment_source);
    if (vertex_shader == 0 || fragment_shader == 0)
    {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Video shader link failed: {}", log);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

//===================================================================================
//===================================================================================
// Compiles the complete shader table once a GL context is available.
bool compileShaderPrograms(GLuint* programs, uint8_t program_count)
{
    for (uint8_t features = 0; features < program_count; ++features)
    {
        const std::string fragment_shader = buildFragmentShader(features);
        programs[features] = createProgram(fragment_shader.c_str());
        if (programs[features] == 0)
        {
            LOGE("Video shader feature program failed mask={}", static_cast<int>(features));
            for (uint8_t index = 0; index < features; ++index)
            {
                if (programs[index] != 0)
                {
                    glDeleteProgram(programs[index]);
                    programs[index] = 0;
                }
            }
            return false;
        }
    }
    LOGI("Video shader compiled {} feature combinations", static_cast<int>(program_count));
    return true;
}

//===================================================================================
//===================================================================================
// Converts a screen coordinate into OpenGL normalized device coordinates.
float screenToNdcX(float value, float surface_width)
{
    return value / surface_width * 2.0f - 1.0f;
}

//===================================================================================
//===================================================================================
// Converts a screen coordinate into OpenGL normalized device coordinates.
float screenToNdcY(float value, float surface_height)
{
    return 1.0f - value / surface_height * 2.0f;
}
}

namespace gs::render
{

//===================================================================================
//===================================================================================
// Converts an OFF/LOW/MED/HIGH postprocessing level into a coefficient multiplier.
float postprocessingLevelScale(uint8_t level)
{
    switch (level)
    {
    case 1: return 0.5f;
    case 2: return 1.0f;
    case 3: return 2.0f;
    default: return 0.0f;
    }
}

//===================================================================================
//===================================================================================
// Builds bounded postprocessing strengths from GS controls and current OV JPEG quality.
VideoPostprocessingParams buildVideoPostprocessingParams(uint8_t current_quality)
{
    VideoPostprocessingParams params = {};
    const float deblocking_scale = s_postprocessingState.jpeg_deblocking_enabled ? 1.0f : 0.0f;
    const float debanding_scale = postprocessingLevelScale(s_postprocessingState.debanding_level);
    const float dithering_scale = postprocessingLevelScale(s_postprocessingState.adaptive_dithering_level);

    // A zero current_quality means no air stats have reached GS yet, not maximum
    // camera quality. Use a visible middle fallback until real stats arrive.
    const uint8_t effective_quality = current_quality != 0 ? current_quality : kJpegQualityUnknownFallback;
    const float artifact = std::clamp((static_cast<float>(effective_quality) - kJpegQualityBest) /
                                      (kJpegQualityWorst - kJpegQualityBest),
                                      0.0f,
                                      1.0f);

    if (deblocking_scale > 0.0f)
    {
        // OV sensors use larger quality numbers for stronger JPEG quantization, so the
        // shader uses wider alpha/beta/tc thresholds at lower quality while still
        // keeping smoothing focused on likely JPEG block boundaries.
        params.deblocking_strength = (0.50f + 0.30f * artifact) * deblocking_scale;
        params.deblocking_alpha = (28.0f + 40.0f * artifact) / 255.0f;
        params.deblocking_beta = (22.0f + 32.0f * artifact) / 255.0f;
        params.deblocking_tc = std::min((0.50f + 0.30f * artifact) * deblocking_scale, 1.0f);
    }

    if (debanding_scale > 0.0f)
    {
        params.debanding_strength = (0.16f + 0.16f * artifact) * debanding_scale;
        params.debanding_flat_threshold = 0.012f + 0.016f * artifact;
        params.debanding_range = 0.018f + 0.026f * artifact;
    }

    if (dithering_scale > 0.0f)
    {
        params.dithering_strength = ((6.0f + 7.0f * artifact) * dithering_scale) / 255.0f;
        params.dithering_flat_threshold = 0.020f + 0.022f * artifact;
    }

    return params;
}

//===================================================================================
//===================================================================================
// Releases GLES resources owned by VideoShaderRenderer.
VideoShaderRenderer::~VideoShaderRenderer()
{
    release();
}

//===================================================================================
//===================================================================================
// Deletes startup-compiled shader programs.
void VideoShaderRenderer::release()
{
    for (unsigned int& program : m_programs)
    {
        if (program != 0)
        {
            glDeleteProgram(program);
            program = 0;
        }
    }
    m_programs_ready = false;
}

//===================================================================================
//===================================================================================
// Draws one textured video quad with the compiled shader matching active features.
bool VideoShaderRenderer::draw(unsigned int texture,
                               const VideoQuad& quad,
                               float clip_x,
                               float clip_y,
                               float clip_width,
                               float clip_height,
                               float surface_width,
                               float surface_height,
                               int frame_width,
                               int frame_height,
                               const LensCorrectionParams& lens_params,
                               const gs::stabilization::StabilizationTransform& stabilization_transform,
                               const VideoPostprocessingParams& postprocessing_params)
{
    if (texture == 0 ||
        quad.width <= 0.0f ||
        quad.height <= 0.0f ||
        clip_width <= 0.0f ||
        clip_height <= 0.0f ||
        surface_width <= 0.0f ||
        surface_height <= 0.0f)
    {
        return false;
    }

    if (!m_programs_ready)
    {
        if (!compileShaderPrograms(m_programs, kShaderFeatureCount))
        {
            return false;
        }
        m_programs_ready = true;
    }

    float inv00 = 1.0f;
    float inv01 = 0.0f;
    float inv02 = 0.0f;
    float inv10 = 0.0f;
    float inv11 = 1.0f;
    float inv12 = 0.0f;
    bool stabilization_enabled = false;
    if (stabilization_transform.enabled &&
        stabilization_transform.width == frame_width &&
        stabilization_transform.height == frame_height)
    {
        const float det = stabilization_transform.m00 * stabilization_transform.m11 -
                          stabilization_transform.m01 * stabilization_transform.m10;
        if (std::abs(det) > 0.000001f)
        {
            const float inv_det = 1.0f / det;
            inv00 = stabilization_transform.m11 * inv_det;
            inv01 = -stabilization_transform.m01 * inv_det;
            inv10 = -stabilization_transform.m10 * inv_det;
            inv11 = stabilization_transform.m00 * inv_det;
            inv02 = -(inv00 * stabilization_transform.m02 + inv01 * stabilization_transform.m12);
            inv12 = -(inv10 * stabilization_transform.m02 + inv11 * stabilization_transform.m12);
            stabilization_enabled = true;
        }
    }

    const bool lens_enabled = isLensCorrectionEnabled(lens_params);
    const bool deblocking_enabled = postprocessing_params.deblocking_strength > 0.0f;
    const bool debanding_enabled = postprocessing_params.debanding_strength > 0.0f;
    const bool dithering_enabled = postprocessing_params.dithering_strength > 0.0f;
    const bool active_postprocessing = deblocking_enabled || debanding_enabled || dithering_enabled;
    uint8_t program_features = 0;
    program_features |= lens_enabled ? kShaderFeatureLens : 0;
    program_features |= stabilization_enabled ? kShaderFeatureStabilization : 0;
    program_features |= deblocking_enabled ? kShaderFeatureDeblocking : 0;
    program_features |= debanding_enabled ? kShaderFeatureDebanding : 0;
    program_features |= dithering_enabled ? kShaderFeatureDithering : 0;
    const GLuint program = m_programs[program_features];
    if (program == 0)
    {
        return false;
    }

    const float x1 = quad.x;
    const float y1 = quad.y;
    const float x2 = quad.x + quad.width;
    const float y2 = quad.y + quad.height;
    const GLfloat vertices[] = {
        screenToNdcX(x1, surface_width), screenToNdcY(y1, surface_height), quad.u0, quad.v0,
        screenToNdcX(x2, surface_width), screenToNdcY(y1, surface_height), quad.u1, quad.v0,
        screenToNdcX(x1, surface_width), screenToNdcY(y2, surface_height), quad.u0, quad.v1,
        screenToNdcX(x2, surface_width), screenToNdcY(y2, surface_height), quad.u1, quad.v1,
    };

    GLint previous_program = 0;
    GLint previous_texture = 0;
    GLint previous_active_texture = 0;
    GLint previous_scissor_box[4] = {};
    GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_SCISSOR_BOX, previous_scissor_box);

    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
    glUniform2f(glGetUniformLocation(program, "uUv0"), quad.u0, quad.v0);
    glUniform2f(glGetUniformLocation(program, "uUv1"), quad.u1, quad.v1);
    const bool frame_size_uniform_needed = (program_features & (kShaderFeatureStabilization |
                                                                kShaderFeatureDeblocking |
                                                                kShaderFeatureDebanding |
                                                                kShaderFeatureDithering)) != 0;
    if (frame_size_uniform_needed)
    {
        glUniform2f(glGetUniformLocation(program, "uFrameSize"),
                    static_cast<float>(std::max(frame_width, 1)),
                    static_cast<float>(std::max(frame_height, 1)));
    }
    if (stabilization_enabled)
    {
        glUniform3f(glGetUniformLocation(program, "uStabilizationInv0"), inv00, inv01, inv02);
        glUniform3f(glGetUniformLocation(program, "uStabilizationInv1"), inv10, inv11, inv12);
    }
    if (active_postprocessing)
    {
        static bool s_logged_postprocessing_active = false;
        if (!s_logged_postprocessing_active)
        {
            s_logged_postprocessing_active = true;
            LOGI("Video postprocessing shader active deblock=({:.3f},{:.3f},{:.3f},{:.3f}) deband=({:.3f},{:.3f},{:.3f}) dither=({:.4f},{:.3f})",
                 postprocessing_params.deblocking_strength,
                 postprocessing_params.deblocking_alpha,
                 postprocessing_params.deblocking_beta,
                 postprocessing_params.deblocking_tc,
                 postprocessing_params.debanding_strength,
                 postprocessing_params.debanding_flat_threshold,
                 postprocessing_params.debanding_range,
                 postprocessing_params.dithering_strength,
                 postprocessing_params.dithering_flat_threshold);
        }
        if (deblocking_enabled)
        {
            glUniform4f(glGetUniformLocation(program, "uDeblockParams"),
                        postprocessing_params.deblocking_strength,
                        postprocessing_params.deblocking_alpha,
                        postprocessing_params.deblocking_beta,
                        postprocessing_params.deblocking_tc);
        }
        if (debanding_enabled)
        {
            glUniform3f(glGetUniformLocation(program, "uDebandParams"),
                        postprocessing_params.debanding_strength,
                        postprocessing_params.debanding_flat_threshold,
                        postprocessing_params.debanding_range);
        }
        if (dithering_enabled)
        {
            glUniform2f(glGetUniformLocation(program, "uDitherParams"),
                        postprocessing_params.dithering_strength,
                        postprocessing_params.dithering_flat_threshold);
        }
    }
    if (lens_enabled)
    {
        const float aspect = frame_height > 0
            ? static_cast<float>(std::max(frame_width, 1)) / static_cast<float>(frame_height)
            : 1.0f;
        glUniform3f(glGetUniformLocation(program, "uRadial"), lens_params.k1, lens_params.k2, lens_params.k3);
        glUniform2f(glGetUniformLocation(program, "uTangential"), lens_params.p1, lens_params.p2);
        const float focal_x = lens_params.has_camera_matrix ? lens_params.fx_norm : 0.5f / std::max(aspect, 0.0001f);
        const float focal_y = lens_params.has_camera_matrix ? lens_params.fy_norm : 0.5f;
        const float principal_x = lens_params.has_camera_matrix ? lens_params.cx_norm : 0.5f;
        const float principal_y = lens_params.has_camera_matrix ? lens_params.cy_norm : 0.5f;
        glUniform2f(glGetUniformLocation(program, "uFocalNorm"), focal_x, focal_y);
        glUniform2f(glGetUniformLocation(program, "uPrincipalNorm"), principal_x, principal_y);
    }

    const GLint scissor_x = static_cast<GLint>(std::round(clip_x));
    const GLint scissor_y = static_cast<GLint>(std::round(surface_height - clip_y - clip_height));
    const GLsizei scissor_w = static_cast<GLsizei>(std::round(clip_width));
    const GLsizei scissor_h = static_cast<GLsizei>(std::round(clip_height));
    glEnable(GL_SCISSOR_TEST);
    glScissor(scissor_x, scissor_y, scissor_w, scissor_h);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    if (scissor_was_enabled)
    {
        glScissor(previous_scissor_box[0],
                  previous_scissor_box[1],
                  previous_scissor_box[2],
                  previous_scissor_box[3]);
    }
    else
    {
        glDisable(GL_SCISSOR_TEST);
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    glUseProgram(static_cast<GLuint>(previous_program));
    glActiveTexture(static_cast<GLenum>(previous_active_texture));
    return true;
}
}
