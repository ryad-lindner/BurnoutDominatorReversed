#pragma once

#include <cstddef>
#include <cstdint>

// Frames that stay on the GPU. The paraLLEl-GS backend copies each scan-out into one of a few
// images that the window's OpenGL context imports (EXT_memory_object, EXT_semaphore), instead
// of reading it back to host memory for the window to upload again: 18 MiB a frame at 16
// samples a pixel with the high-resolution scan-out. Windows (Win32 handles) and Linux (opaque
// file descriptors). Without the extensions, with the OpenGL context on another GPU, or with
// BDR_SHARED_FRAMES=0, frames go through host memory.
namespace ps2x::shared_frames
{
    constexpr int kSlots = 4;

    // GS backend.
    // Its Vulkan device, which the window must share: the LUID (8 bytes) on Windows, the device
    // UUID (16 bytes) elsewhere, with the driver UUID, which OpenGL must have too.
    void setDeviceId(const uint8_t *id, size_t size, const uint8_t *driver = nullptr);
    int beginFrame();                          // a slot to copy the frame into, or -1 (none, or not in use)
    // A new image for the slot. The window takes the memory handle (a Win32 handle or a file
    // descriptor) and closes it, or hands it to OpenGL.
    void setImage(int slot, intptr_t memory, uint64_t bytes, uint32_t width, uint32_t height);
    void publish(int slot, intptr_t ready);    // copied once `ready` (closed by the window) is signalled
    void release(int slot);                    // free again: a frame given up, or one the window has drawn
    void discard(int slot);                    // free again, closing an image the window has not taken: sharing ends

    // Window, on its OpenGL thread.
    bool startWindow(); // after InitWindow: whether frames are shared from now on
    void stopWindow();
    struct ShownFrame
    {
        unsigned texture; // an OpenGL texture name
        uint32_t width, height;
    };
    bool showNewFrame(ShownFrame &out); // the newest frame, if one came since the last call
    void releaseDrawn();                // gives back the slots the GPU has drawn (showNewFrame does too)
    bool shownFrame(ShownFrame &out);   // the frame showNewFrame last returned
}
