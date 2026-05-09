#pragma once

namespace gs::openxr
{

// Pushes an ImGui key (cast to int) into a small thread-safe queue. Used by the
// OpenXR thread to forward controller-button rising edges to the renderer.
void publishImGuiKey(int imgui_key);

// Pops the oldest pending ImGui key. Returns false when the queue is empty.
bool consumeImGuiKey(int& out_imgui_key);

// Stores the OpenXR thread's EGLContext (passed as opaque void*). The renderer's
// surface backend reads this and creates its own context as a share-group sibling
// so GL textures can be sampled directly across threads — no glReadPixels copy.
void setSharedEglContext(void* egl_context);
void* getSharedEglContext();

// Publishes the renderer's offscreen color texture (id + dimensions) so the
// OpenXR thread can sample it directly into the head-locked quad swapchain.
void publishRendererTexture(unsigned int gl_texture, int width, int height);
bool getRendererTexture(unsigned int& gl_texture, int& width, int& height);

} // namespace gs::openxr
