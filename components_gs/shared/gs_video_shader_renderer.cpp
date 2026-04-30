#include "gs_video_shader_renderer.h"

#include <GLES2/gl2.h>

#include "Log.h"

#include <algorithm>
#include <cmath>

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

constexpr char kFastFragmentShader[] = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTexture;
uniform vec2 uUv0;
uniform vec2 uUv1;
uniform vec2 uFrameSize;
uniform vec3 uStabilizationInv0;
uniform vec3 uStabilizationInv1;
uniform float uStabilizationEnabled;
varying vec2 vTexCoord;

void main()
{
    vec2 range = uUv1 - uUv0;
    range.x = abs(range.x) < 0.0001 ? 0.0001 : range.x;
    range.y = abs(range.y) < 0.0001 ? 0.0001 : range.y;
    vec2 sample_coord = (vTexCoord - uUv0) / range;
    if (uStabilizationEnabled > 0.5)
    {
        vec2 pixel_coord = sample_coord * uFrameSize;
        sample_coord = vec2(
            dot(uStabilizationInv0, vec3(pixel_coord, 1.0)) / uFrameSize.x,
            dot(uStabilizationInv1, vec3(pixel_coord, 1.0)) / uFrameSize.y);
    }
    if (sample_coord.x < 0.0 || sample_coord.x > 1.0 || sample_coord.y < 0.0 || sample_coord.y > 1.0)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    gl_FragColor = texture2D(uTexture, mix(uUv0, uUv1, sample_coord));
}
)glsl";

constexpr char kLensFragmentShader[] = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTexture;
uniform vec2 uUv0;
uniform vec2 uUv1;
uniform vec3 uRadial;
uniform vec2 uTangential;
uniform vec2 uFocalNorm;
uniform vec2 uPrincipalNorm;
uniform vec2 uFrameSize;
uniform vec3 uStabilizationInv0;
uniform vec3 uStabilizationInv1;
uniform float uStabilizationEnabled;
varying vec2 vTexCoord;

void main()
{
    vec2 range = uUv1 - uUv0;
    range.x = abs(range.x) < 0.0001 ? 0.0001 : range.x;
    range.y = abs(range.y) < 0.0001 ? 0.0001 : range.y;
    vec2 normalized_coord = (vTexCoord - uUv0) / range;
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
    vec2 sample_coord = corrected * focal + uPrincipalNorm;
    if (uStabilizationEnabled > 0.5)
    {
        vec2 pixel_coord = sample_coord * uFrameSize;
        sample_coord = vec2(
            dot(uStabilizationInv0, vec3(pixel_coord, 1.0)) / uFrameSize.x,
            dot(uStabilizationInv1, vec3(pixel_coord, 1.0)) / uFrameSize.y);
    }
    if (sample_coord.x < 0.0 || sample_coord.x > 1.0 || sample_coord.y < 0.0 || sample_coord.y > 1.0)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    gl_FragColor = texture2D(uTexture, mix(uUv0, uUv1, sample_coord));
}
)glsl";

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
// Releases GLES resources owned by VideoShaderRenderer.
VideoShaderRenderer::~VideoShaderRenderer()
{
    release();
}

//===================================================================================
//===================================================================================
// Deletes lazily-created shader programs.
void VideoShaderRenderer::release()
{
    if (m_fast_program != 0)
    {
        glDeleteProgram(m_fast_program);
        m_fast_program = 0;
    }
    if (m_lens_program != 0)
    {
        glDeleteProgram(m_lens_program);
        m_lens_program = 0;
    }
}

//===================================================================================
//===================================================================================
// Draws one textured video quad, applying lens correction and stabilization in shader.
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
                               const gs::stabilization::StabilizationTransform& stabilization_transform)
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

    const bool lens_enabled = isLensCorrectionEnabled(lens_params);
    GLuint& program_slot = lens_enabled ? m_lens_program : m_fast_program;
    if (program_slot == 0)
    {
        program_slot = createProgram(lens_enabled ? kLensFragmentShader : kFastFragmentShader);
        if (program_slot == 0)
        {
            return false;
        }
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

    glUseProgram(program_slot);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program_slot, "uTexture"), 0);
    glUniform2f(glGetUniformLocation(program_slot, "uUv0"), quad.u0, quad.v0);
    glUniform2f(glGetUniformLocation(program_slot, "uUv1"), quad.u1, quad.v1);
    glUniform2f(glGetUniformLocation(program_slot, "uFrameSize"),
                static_cast<float>(std::max(frame_width, 1)),
                static_cast<float>(std::max(frame_height, 1)));

    float inv00 = 1.0f;
    float inv01 = 0.0f;
    float inv02 = 0.0f;
    float inv10 = 0.0f;
    float inv11 = 1.0f;
    float inv12 = 0.0f;
    const bool stabilization_enabled = stabilization_transform.enabled &&
                                       stabilization_transform.width == frame_width &&
                                       stabilization_transform.height == frame_height;
    if (stabilization_enabled)
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
        }
    }
    glUniform1f(glGetUniformLocation(program_slot, "uStabilizationEnabled"), stabilization_enabled ? 1.0f : 0.0f);
    glUniform3f(glGetUniformLocation(program_slot, "uStabilizationInv0"), inv00, inv01, inv02);
    glUniform3f(glGetUniformLocation(program_slot, "uStabilizationInv1"), inv10, inv11, inv12);
    if (lens_enabled)
    {
        const float aspect = frame_height > 0
            ? static_cast<float>(std::max(frame_width, 1)) / static_cast<float>(frame_height)
            : 1.0f;
        glUniform3f(glGetUniformLocation(program_slot, "uRadial"), lens_params.k1, lens_params.k2, lens_params.k3);
        glUniform2f(glGetUniformLocation(program_slot, "uTangential"), lens_params.p1, lens_params.p2);
        const float focal_x = lens_params.has_camera_matrix ? lens_params.fx_norm : 0.5f / std::max(aspect, 0.0001f);
        const float focal_y = lens_params.has_camera_matrix ? lens_params.fy_norm : 0.5f;
        const float principal_x = lens_params.has_camera_matrix ? lens_params.cx_norm : 0.5f;
        const float principal_y = lens_params.has_camera_matrix ? lens_params.cy_norm : 0.5f;
        glUniform2f(glGetUniformLocation(program_slot, "uFocalNorm"), focal_x, focal_y);
        glUniform2f(glGetUniformLocation(program_slot, "uPrincipalNorm"), principal_x, principal_y);
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
