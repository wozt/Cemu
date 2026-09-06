#include "Cafe/HW/Latte/Core/BottomScreenBridge.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "config/CemuConfig.h"
#include "input/InputManager.h"
#include "input/emulated/VPADController.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

extern "C" {
#include "bs_mailbox.h"
#include "bs_net.h"
#include "bs_protocol.h"
#include "bs_server.h"
#include "bs_source.h"
}

void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height,
                                          sint32* fullWidth, sint32* fullHeight, bool padView);

namespace BottomScreen
{

namespace
{
    BsSource* g_source = nullptr;
    BsServer* g_server = nullptr;
    bool      g_tried  = false;
    int       g_width  = 0;
    int       g_height = 0;
    /* Buttons held by clients, indexed by VPADController::ButtonId.
     * Written once per frame by ApplyInput, read by VPADController. */
    uint64    g_held = 0;
    /* Axes as sent, indexed by BsAxis - 1. */
    sint16    g_axis[4] = {0, 0, 0, 0};

    /*
     * The announced frame rate is measured, not assumed.
     *
     * The GamePad is nominally 60 Hz, but a game that runs at 30 in
     * emulation submits at 30, and announcing 60 would have every client
     * pacing itself against a rate that never arrives. A short count
     * before the server opens costs half a second and tells the truth.
     */
    uint32    g_rate_first_us = 0;
    int       g_rate_frames = 0;
    int       g_fps = 0;
    constexpr int kWarmupFrames  = 90;   // discarded: the game is still starting
    constexpr int kMeasureFrames = 90;   // counted: long enough to absorb a stutter
    std::vector<uint8> g_pixels;

    /*
     * BsButton -> Wii U button. The GamePad has no SELECT in the DS
     * sense, and the protocol is shared, so a client built for another
     * console will send buttons this machine does not have. They map to
     * nothing and are dropped rather than treated as an error.
     */
    uint32 buttonFor(int bsButton)
    {
        switch (bsButton)
        {
        case BS_BTN_A:      return VPADController::kButtonId_A;
        case BS_BTN_B:      return VPADController::kButtonId_B;
        case BS_BTN_X:      return VPADController::kButtonId_X;
        case BS_BTN_Y:      return VPADController::kButtonId_Y;
        case BS_BTN_L:      return VPADController::kButtonId_L;
        case BS_BTN_R:      return VPADController::kButtonId_R;
        case BS_BTN_ZL:     return VPADController::kButtonId_ZL;
        case BS_BTN_ZR:     return VPADController::kButtonId_ZR;
        case BS_BTN_START:  return VPADController::kButtonId_Plus;
        case BS_BTN_SELECT: return VPADController::kButtonId_Minus;
        case BS_BTN_UP:     return VPADController::kButtonId_Up;
        case BS_BTN_DOWN:   return VPADController::kButtonId_Down;
        case BS_BTN_LEFT:   return VPADController::kButtonId_Left;
        case BS_BTN_RIGHT:  return VPADController::kButtonId_Right;
        default:            return 0;
        }
    }
}

void Start()
{
    if (g_tried)
        return;
    g_tried = true;

    if (!GetConfig().bottom_screen_enabled)
        return;
    if (g_width <= 0 || g_height <= 0 || g_fps <= 0)
    {
        // Nothing has been measured yet, so there is nothing honest to
        // announce. Wait rather than guess.
        g_tried = false;
        return;
    }

    g_source = bs_mailbox_create(BS_CONSOLE_WIIU, g_width, g_height, g_fps, BS_PIXFMT_RGBA);
    if (!g_source)
    {
        fprintf(stderr, "bottom_screen: cannot create the frame mailbox\n");
        return;
    }

    BsServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = GetConfig().bottom_screen_port.GetValue();

    char err[256] = "";
    g_server = bs_server_create(g_source, &cfg, err, sizeof(err));
    if (!g_server)
    {
        fprintf(stderr, "bottom_screen: %s\n", err);
        g_source->destroy(g_source->self);
        free(g_source);
        g_source = nullptr;
    }
}

void Stop()
{
    if (g_server)
    {
        bs_server_destroy(g_server);
        g_server = nullptr;
    }
    if (g_source)
    {
        g_source->destroy(g_source->self);
        free(g_source);
        g_source = nullptr;
    }
    g_tried = false;
    g_width = g_height = 0;
    g_fps = 0;
    g_rate_frames = 0;
}

bool IsRunning()
{
    return g_server != nullptr;
}

void SubmitPadView(LatteTextureView* texView)
{
    if (!GetConfig().bottom_screen_enabled || !texView)
        return;

    sint32 w = 0, h = 0;
    if (!g_renderer || !g_renderer->ReadbackViewRGBA(texView, g_pixels, w, h))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            fprintf(stderr,
                "bottom_screen: the pad view cannot be read back on this render\n"
                "               backend yet, so nothing will be streamed.\n"
                "               Set Options > General settings > Graphics > API\n"
                "               to OpenGL. Vulkan and Metal are not supported yet.\n");
        }
        return;
    }

    /*
     * The stream's size is fixed at the handshake, so a change of
     * internal resolution mid-game means a new server rather than a
     * silently mismatched picture. Rare, and cheap when it happens.
     */
    if (g_server && (w != g_width || h != g_height))
        Stop();

    if (!g_server)
    {
        g_width = w;
        g_height = h;

        /*
         * Measure the real rate, but not during the opening seconds.
         *
         * A game's first frames are its slowest -- shaders compiling,
         * assets loading, caches cold -- and a sample taken there
         * reports a third of the true rate. That is worse than assuming
         * 60: the encoder's rate control and keyframe interval are both
         * derived from this number, so a low reading spends the whole
         * bitrate budget and emits keyframes three times too often.
         *
         * So: discard the warm-up, then count over a window long enough
         * to average out a stutter. The server opens two or three
         * seconds into the game instead of half a second, which nobody
         * notices -- a phone is not connected before the game is even on
         * screen.
         */
        g_rate_frames++;
        if (g_rate_frames <= kWarmupFrames)
            return;
        if (g_rate_frames == kWarmupFrames + 1)
        {
            g_rate_first_us = bs_now_us();
            return;
        }

        const uint32 elapsed = bs_now_us() - g_rate_first_us;
        if (g_rate_frames < kWarmupFrames + kMeasureFrames || elapsed == 0)
            return;

        double fps = (double)(g_rate_frames - kWarmupFrames - 1) * 1000000.0 / (double)elapsed;
        if (fps < 5.0)   fps = 5.0;
        if (fps > 120.0) fps = 120.0;
        g_fps = (int)(fps + 0.5);

        Start();
        if (!g_server)
            return;
    }

    bs_mailbox_submit(g_source, g_pixels.data(), w * 4);
}

void ApplyInput()
{
    if (!g_server)
        return;

    BsInputState in;
    bs_mailbox_input(g_source, &in);

    auto& instance = InputManager::instance();

    /*
     * Cemu stores the pad touch in window pixels and converts it later
     * with LatteRenderTarget_getScreenImageArea. Clients send console
     * coordinates, so we walk that conversion backwards using the very
     * same function -- guessing at the geometry instead would put every
     * tap slightly off, and worse as the window is resized.
     */
    if (in.touching && g_width > 0 && g_height > 0)
    {
        sint32 ix = 0, iy = 0, iw = 0, ih = 0;
        LatteRenderTarget_getScreenImageArea(&ix, &iy, &iw, &ih, nullptr, nullptr, true);
        if (iw > 0 && ih > 0)
        {
            const float fx = (float)in.touch_x / (float)g_width;
            const float fy = (float)in.touch_y / (float)g_height;
            std::scoped_lock lock(instance.m_pad_touch.m_mutex);
            instance.m_pad_touch.position = { ix + (int)(fx * iw), iy + (int)(fy * ih) };
            instance.m_pad_touch.left_down = true;

            // Said once. "My touch does nothing" is the hardest kind of
            // report to act on, and this separates a client that never
            // sent anything from a game that ignored what arrived.
            static bool announced = false;
            if (!announced)
            {
                announced = true;
                fprintf(stderr, "bottom_screen: first touch from a client at %d,%d\n",
                        in.touch_x, in.touch_y);
            }
        }
    }
    else
    {
        std::scoped_lock lock(instance.m_pad_touch.m_mutex);
        instance.m_pad_touch.left_down = false;
    }

    uint64 held = 0;
    for (int b = 1; b <= 15; b++)
    {
        if (!(in.buttons & (1u << (b - 1))))
            continue;
        const uint32 id = buttonFor(b);
        if (id != 0 && id < 64)
            held |= (1ull << id);
    }
    g_held = held;
    for (int i = 0; i < 4; i++)
        g_axis[i] = in.axis[i];

}

bool GetStick(int index, float& x, float& y)
{
    if (!g_server || index < 0 || index > 1)
        return false;

    const int xi = (index == 0) ? BS_AXIS_LEFT_X - 1 : BS_AXIS_RIGHT_X - 1;
    const int yi = (index == 0) ? BS_AXIS_LEFT_Y - 1 : BS_AXIS_RIGHT_Y - 1;

    // Centred means "not in use": say so, and the host's own pad keeps
    // the stick rather than being pinned to zero by an idle client.
    if (g_axis[xi] == 0 && g_axis[yi] == 0)
        return false;

    x = (float)g_axis[xi] / 32767.0f;
    y = (float)g_axis[yi] / 32767.0f;

    // Said once per stick, for the same reason as the touch line: a
    // stick that does nothing could be the client, the wire, or the
    // game, and these are indistinguishable from outside.
    static bool announced[2] = {false, false};
    if (!announced[index])
    {
        announced[index] = true;
        fprintf(stderr, "bottom_screen: first %s stick from a client at %.2f,%.2f\n",
                index == 0 ? "left" : "right", x, y);
    }
    return true;
}

bool IsButtonHeld(int vpadButtonId)
{
    if (!g_server || vpadButtonId <= 0 || vpadButtonId >= 64)
        return false;
    return (g_held & (1ull << vpadButtonId)) != 0;
}

}
