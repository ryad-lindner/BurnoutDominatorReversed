#include "runtime/wayland_surface.h"

#ifdef PS2X_WAYLAND_CONTENT_TYPE
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <wayland-client.h>
#include "content-type-v1-client-protocol.h"

#include <cstdint>
#include <cstring>
#include <iostream>

#include "raylib.h"
#endif

namespace ps2x
{
#ifdef PS2X_WAYLAND_CONTENT_TYPE
    namespace
    {
        struct Offer
        {
            uint32_t name = 0;
        };

        void onGlobal(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t)
        {
            if (std::strcmp(interface, wp_content_type_manager_v1_interface.name) == 0)
                static_cast<Offer *>(data)->name = name;
        }

        void onGlobalRemove(void *, wl_registry *, uint32_t) {}

        const wl_registry_listener kRegistryListener = {onGlobal, onGlobalRemove};
    }

    void tagWindowAsGame()
    {
        GLFWwindow *window = static_cast<GLFWwindow *>(GetWindowHandle());
        if (!window || glfwGetPlatform() != GLFW_PLATFORM_WAYLAND)
            return;
        wl_display *display = glfwGetWaylandDisplay();
        wl_surface *surface = glfwGetWaylandWindow(window);
        if (!display || !surface)
            return;

        // GLFW owns the display's default queue: the registry gets a queue of its own.
        wl_event_queue *queue = wl_display_create_queue(display);
        wl_display *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
        wl_registry *registry = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        Offer offer;
        wl_registry_add_listener(registry, &kRegistryListener, &offer);
        if (wl_display_roundtrip_queue(display, queue) < 0)
        {
            std::cerr << "[display] Wayland registry round trip failed (" << std::strerror(wl_display_get_error(display))
                      << "): no content type" << std::endl;
            wl_registry_destroy(registry);
            wl_event_queue_destroy(queue);
            return;
        }

        if (offer.name)
        {
            auto *manager = static_cast<wp_content_type_manager_v1 *>(
                wl_registry_bind(registry, offer.name, &wp_content_type_manager_v1_interface, 1));
            wp_content_type_v1 *content = wp_content_type_manager_v1_get_surface_content_type(manager, surface);
            wp_content_type_v1_set_content_type(content, WP_CONTENT_TYPE_V1_TYPE_GAME);
            // Neither object has events: back to the default queue, so the private one can go. They live
            // as long as the window; the type applies with the surface's next commit (the next swap).
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(manager), nullptr);
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(content), nullptr);
            wl_display_flush(display);
            std::cout << "[display] Wayland surface content type: game" << std::endl;
        }
        else
            std::cout << "[display] the compositor has no wp_content_type_manager_v1: no content type" << std::endl;

        wl_registry_destroy(registry);
        wl_event_queue_destroy(queue);
    }
#else
    void tagWindowAsGame() {}
#endif
}
