#pragma once

namespace ps2x
{
    // On a native Wayland window whose compositor offers wp_content_type_manager_v1, tags the window's
    // surface with the content type "game" (a compositor may use it for the display's low latency
    // mode, ALLM on a television). Nothing elsewhere. Call after InitWindow.
    void tagWindowAsGame();
}
