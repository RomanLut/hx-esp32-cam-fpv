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
    kShaderFeatureDithering = 1u << 4,
};

constexpr uint8_t kArtifactShaderFeatureMasks[] = {
    0,
    kShaderFeatureDeblocking,
    kShaderFeatureDithering,
    kShaderFeatureDeblocking | kShaderFeatureDithering,
};
constexpr uint8_t kDisplayShaderFeatureMasks[] = {
    0,
    kShaderFeatureLens,
    kShaderFeatureStabilization,
    kShaderFeatureLens | kShaderFeatureStabilization,
};

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

#if GS_FEATURE_DEBLOCKING || GS_FEATURE_DITHERING
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

#if GS_FEATURE_DITHERING
vec3 applyDithering(vec2 sample_coord, vec3 color)
{
    vec2 px = 1.0 / max(uFrameSize, vec2(1.0, 1.0));
    float color_luma = luma(color);
    float g = max(abs(color_luma - luma(sampleVideo(min(sample_coord + vec2(px.x, 0.0), vec2(1.0, 1.0))))),
                  abs(color_luma - luma(sampleVideo(min(sample_coord + vec2(0.0, px.y), vec2(1.0, 1.0))))));
    float flat_area = 1.0 - smoothstep(uDitherParams.y * 0.5, uDitherParams.y, g);
    vec2 max_pixel = max(uFrameSize - vec2(1.0, 1.0), vec2(0.0, 0.0));
    vec2 image_pixel = floor(clamp(sample_coord * uFrameSize, vec2(0.0, 0.0), max_pixel));
    float n = hashNoise(image_pixel + vec2(color_luma * 31.0, color_luma * 17.0)) - 0.5;
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
                                               kShaderFeatureDithering)) != 0;
    std::string source;
    source.reserve(12000);
    source += "#define GS_FEATURE_LENS ";
    source += (features & kShaderFeatureLens) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_STABILIZATION ";
    source += (features & kShaderFeatureStabilization) != 0 ? "1\n" : "0\n";
    source += "#define GS_FEATURE_DEBLOCKING ";
    source += (features & kShaderFeatureDeblocking) != 0 ? "1\n" : "0\n";
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
// Compiles one shader table from the feature masks used by that render pass.
bool compileShaderPrograms(GLuint* programs, const uint8_t* feature_masks, uint8_t program_count, const char* pass_name)
{
    for (uint8_t features = 0; features < program_count; ++features)
    {
        const std::string fragment_shader = buildFragmentShader(feature_masks[features]);
        programs[features] = createProgram(fragment_shader.c_str());
        if (programs[features] == 0)
        {
            LOGE("Video {} shader program failed index={} mask={}",
                 pass_name,
                 static_cast<int>(features),
                 static_cast<int>(feature_masks[features]));
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
    LOGI("Video {} shader compiled {} feature combinations", pass_name, static_cast<int>(program_count));
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
    for (unsigned int& program : m_artifact_programs)
    {
        if (program != 0)
        {
            glDeleteProgram(program);
            program = 0;
        }
    }
    for (unsigned int& program : m_display_programs)
    {
        if (program != 0)
        {
            glDeleteProgram(program);
            program = 0;
        }
    }
    m_programs_ready = false;
    releaseArtifactTarget();
}

//===================================================================================
//===================================================================================
// Deletes the source-resolution artifact pass framebuffer and texture.
void VideoShaderRenderer::releaseArtifactTarget()
{
    if (m_artifact_framebuffer != 0)
    {
        glDeleteFramebuffers(1, &m_artifact_framebuffer);
        m_artifact_framebuffer = 0;
    }
    if (m_artifact_texture != 0)
    {
        glDeleteTextures(1, &m_artifact_texture);
        m_artifact_texture = 0;
    }
    m_artifact_width = 0;
    m_artifact_height = 0;
}

//===================================================================================
//===================================================================================
// Creates or resizes the camera-resolution texture used by the artifact pass.
bool VideoShaderRenderer::ensureArtifactTarget(int frame_width, int frame_height)
{
    const int target_width = std::max(frame_width, 1);
    const int target_height = std::max(frame_height, 1);
    if (m_artifact_texture != 0 &&
        m_artifact_framebuffer != 0 &&
        m_artifact_width == target_width &&
        m_artifact_height == target_height)
    {
        return true;
    }

    releaseArtifactTarget();
    glGenTextures(1, &m_artifact_texture);
    glBindTexture(GL_TEXTURE_2D, m_artifact_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 target_width,
                 target_height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glGenFramebuffers(1, &m_artifact_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_artifact_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_artifact_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        LOGE("Video artifact framebuffer incomplete {}x{}", target_width, target_height);
        releaseArtifactTarget();
        return false;
    }
    m_artifact_width = target_width;
    m_artifact_height = target_height;
    return true;
}

//===================================================================================
//===================================================================================
// Draws one textured video quad through optional artifact and display shader stages.
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
        if (!compileShaderPrograms(m_artifact_programs,
                                   kArtifactShaderFeatureMasks,
                                   kArtifactShaderProgramCount,
                                   "artifact") ||
            !compileShaderPrograms(m_display_programs,
                                   kDisplayShaderFeatureMasks,
                                   kDisplayShaderProgramCount,
                                   "display"))
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
            // OpenCV warp matrices used by the stabilizer already map output pixel -> source sample.
            // Feed the same mapping to the shader (do not invert), otherwise compensation direction flips.
            inv00 = stabilization_transform.m00;
            inv01 = stabilization_transform.m01;
            inv10 = stabilization_transform.m10;
            inv11 = stabilization_transform.m11;
            inv02 = stabilization_transform.m02;
            inv12 = stabilization_transform.m12;
            stabilization_enabled = true;
        }
    }

    const bool lens_enabled = isLensCorrectionEnabled(lens_params);
    const bool deblocking_enabled = postprocessing_params.deblocking_strength > 0.0f;
    const bool dithering_enabled = postprocessing_params.dithering_strength > 0.0f;
    // Artifact correction is defined on the source JPEG pixel grid on every platform.
    // This keeps JPEG block boundaries and the dither pattern stable under display scaling.
    const bool dither_in_artifact_pass = dithering_enabled;
    const bool artifact_pass_enabled = deblocking_enabled || dither_in_artifact_pass;
    const bool full_frame_uv = std::abs(std::abs(quad.u1 - quad.u0) - 1.0f) < 0.0001f &&
                               std::abs(std::abs(quad.v1 - quad.v0) - 1.0f) < 0.0001f;
    // The direct path is valid only for a one-to-one source-to-target draw. Otherwise it
    // shades the target pixel grid instead of the camera pixel grid, which changes artifact
    // processing semantics and can miss KMS page-flip deadlines on scaled Mali output.
    const bool source_matches_target = frame_width == static_cast<int>(std::round(clip_width)) &&
                                       frame_height == static_cast<int>(std::round(clip_height));
    const bool direct_artifact_pass = artifact_pass_enabled &&
                                      source_matches_target &&
                                      !lens_enabled &&
                                      !stabilization_enabled &&
                                      full_frame_uv;
    const uint8_t artifact_program_index = (deblocking_enabled ? 1u : 0u) |
                                           (dither_in_artifact_pass ? 2u : 0u);
    const uint8_t display_program_index = (lens_enabled ? 1u : 0u) |
                                          (stabilization_enabled ? 2u : 0u);
    const GLuint artifact_program = m_artifact_programs[artifact_program_index];
    const GLuint display_program = m_display_programs[display_program_index];
    if ((artifact_pass_enabled && artifact_program == 0) ||
        (!direct_artifact_pass && display_program == 0))
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
    GLint previous_framebuffer = 0;
    GLint previous_viewport[4] = {};
    GLint previous_scissor_box[4] = {};
    GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, previous_scissor_box);

    const GLint scissor_x = static_cast<GLint>(std::round(clip_x));
    const GLint scissor_y = static_cast<GLint>(std::round(surface_height - clip_y - clip_height));
    const GLsizei scissor_w = static_cast<GLsizei>(std::round(clip_width));
    const GLsizei scissor_h = static_cast<GLsizei>(std::round(clip_height));

    GLuint display_texture = texture;
    if (artifact_pass_enabled)
    {
        if (direct_artifact_pass && m_artifact_framebuffer != 0)
        {
            releaseArtifactTarget();
        }
        if (!direct_artifact_pass && !ensureArtifactTarget(frame_width, frame_height))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
            glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
            glUseProgram(static_cast<GLuint>(previous_program));
            glActiveTexture(static_cast<GLenum>(previous_active_texture));
            return false;
        }

        static bool s_logged_artifact_pass_active = false;
        if (!s_logged_artifact_pass_active)
        {
            s_logged_artifact_pass_active = true;
            LOGI("Video artifact shader active target={} deblock=({:.3f},{:.3f},{:.3f},{:.3f}) dither=({:.4f},{:.3f})",
                 direct_artifact_pass ? "final" : "intermediate",
                 postprocessing_params.deblocking_strength,
                 postprocessing_params.deblocking_alpha,
                 postprocessing_params.deblocking_beta,
                 postprocessing_params.deblocking_tc,
                 postprocessing_params.dithering_strength,
                 postprocessing_params.dithering_flat_threshold);
        }

        const GLfloat artifact_vertices[] = {
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
        };
        const GLfloat* artifact_draw_vertices = direct_artifact_pass ? vertices : artifact_vertices;
        if (direct_artifact_pass)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
            glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        }
        else
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_artifact_framebuffer);
            glViewport(0, 0, m_artifact_width, m_artifact_height);
            glDisable(GL_SCISSOR_TEST);
        }
        glUseProgram(artifact_program);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(artifact_program, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(artifact_program, "uUv0"),
                    direct_artifact_pass ? quad.u0 : 0.0f,
                    direct_artifact_pass ? quad.v0 : 1.0f);
        glUniform2f(glGetUniformLocation(artifact_program, "uUv1"),
                    direct_artifact_pass ? quad.u1 : 1.0f,
                    direct_artifact_pass ? quad.v1 : 0.0f);
        glUniform2f(glGetUniformLocation(artifact_program, "uFrameSize"),
                    static_cast<float>(std::max(frame_width, 1)),
                    static_cast<float>(std::max(frame_height, 1)));
        if (deblocking_enabled)
        {
            glUniform4f(glGetUniformLocation(artifact_program, "uDeblockParams"),
                        postprocessing_params.deblocking_strength,
                        postprocessing_params.deblocking_alpha,
                        postprocessing_params.deblocking_beta,
                        postprocessing_params.deblocking_tc);
        }
        if (dither_in_artifact_pass)
        {
            glUniform2f(glGetUniformLocation(artifact_program, "uDitherParams"),
                        postprocessing_params.dithering_strength,
                        postprocessing_params.dithering_flat_threshold);
        }
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), artifact_draw_vertices);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), artifact_draw_vertices + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        if (!direct_artifact_pass)
        {
            display_texture = m_artifact_texture;
        }
    }

    if (!direct_artifact_pass)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
        glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        glUseProgram(display_program);
        glBindTexture(GL_TEXTURE_2D, display_texture);
        glUniform1i(glGetUniformLocation(display_program, "uTexture"), 0);
        glUniform2f(glGetUniformLocation(display_program, "uUv0"), quad.u0, quad.v0);
        glUniform2f(glGetUniformLocation(display_program, "uUv1"), quad.u1, quad.v1);
        if (stabilization_enabled)
        {
            glUniform2f(glGetUniformLocation(display_program, "uFrameSize"),
                        static_cast<float>(std::max(frame_width, 1)),
                        static_cast<float>(std::max(frame_height, 1)));
            glUniform3f(glGetUniformLocation(display_program, "uStabilizationInv0"), inv00, inv01, inv02);
            glUniform3f(glGetUniformLocation(display_program, "uStabilizationInv1"), inv10, inv11, inv12);
        }
        if (lens_enabled)
        {
            const float aspect = frame_height > 0
                ? static_cast<float>(std::max(frame_width, 1)) / static_cast<float>(frame_height)
                : 1.0f;
            glUniform3f(glGetUniformLocation(display_program, "uRadial"), lens_params.k1, lens_params.k2, lens_params.k3);
            glUniform2f(glGetUniformLocation(display_program, "uTangential"), lens_params.p1, lens_params.p2);
            const float focal_x = lens_params.has_camera_matrix ? lens_params.fx_norm : 0.5f / std::max(aspect, 0.0001f);
            const float focal_y = lens_params.has_camera_matrix ? lens_params.fy_norm : 0.5f;
            const float principal_x = lens_params.has_camera_matrix ? lens_params.cx_norm : 0.5f;
            const float principal_y = lens_params.has_camera_matrix ? lens_params.cy_norm : 0.5f;
            glUniform2f(glGetUniformLocation(display_program, "uFocalNorm"), focal_x, focal_y);
            glUniform2f(glGetUniformLocation(display_program, "uPrincipalNorm"), principal_x, principal_y);
        }

        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
    }

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
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    glActiveTexture(static_cast<GLenum>(previous_active_texture));
    return true;
}
}
