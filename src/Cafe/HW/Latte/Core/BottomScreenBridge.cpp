#include "Cafe/HW/Latte/Core/BottomScreenBridge.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <algorithm>

#include "config/CemuConfig.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
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

    /*
     * The television picture, which is the other thing a Wii U draws and
     * usually the one worth watching.
     *
     * Built the first time somebody asks for it and never taken down
     * again while the game runs -- the server stops encoding it on its
     * own when the last viewer leaves, and the readback below is skipped
     * entirely in that case, so what is left costs nothing but the
     * memory. Its own pixel buffer: the two views are read back one
     * after the other on the same thread, and sharing one would work
     * only for as long as that stays true.
     */
    /* Which view is counting frames towards opening the server. */
    bool              g_rate_from_pad = true;

    BsSource*         g_top_source = nullptr;
    std::vector<uint8> g_top_pixels;
    int               g_top_width  = 0;
    int               g_top_height = 0;
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

    // The variable wins over the setting: a scripted launch should be
    // able to turn this off without editing a config file somebody else
    // owns -- and Cemu rewrites its settings.xml on exit, so an edit
    // made while it is running would be lost anyway. Without one, the
    // setting decides. melonDS and Azahar read the same two names.
    if (const char* off = getenv("BOTTOM_SCREEN"); off && !strcmp(off, "0"))
        return;
    if (!GetConfig().bottom_screen_enabled)
        return;
    if (g_width <= 0 || g_height <= 0 || g_fps <= 0)
    {
        // Nothing has been measured yet, so there is nothing honest to
        // announce. Wait rather than guess.
        g_tried = false;
        return;
    }

    g_source = bs_mailbox_create(BS_CONSOLE_WIIU, g_width, g_height, g_fps,
                                 BS_PIXFMT_RGBA, 48000, 2);
    if (!g_source)
    {
        fprintf(stderr, "bottom_screen: cannot create the frame mailbox\n");
        return;
    }

    int port = GetConfig().bottom_screen_port.GetValue();
    if (const char* p = getenv("BOTTOM_SCREEN_PORT"))
    {
        const int v = atoi(p);
        if (v > 0 && v < 65536)
            port = v;
    }

    BsServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = (uint16_t)port;

    char err[256] = "";
    g_server = bs_server_create(g_source, &cfg, err, sizeof(err));
    /* Said now, produced later: the television picture is only read
     * back while somebody is watching it, and nobody may ask for a
     * screen the server has not admitted to. */
    if (g_server)
        bs_server_offer_top(g_server);
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
    /* After the server, which is what was reading from it. */
    if (g_top_source)
    {
        g_top_source->destroy(g_top_source->self);
        free(g_top_source);
        g_top_source = nullptr;
    }
    g_top_width = g_top_height = 0;
    g_tried = false;
    g_width = g_height = 0;
    g_fps = 0;
    g_rate_frames = 0;
}

bool IsRunning()
{
    return g_server != nullptr;
}

/*
 * Counts frames until the rate is worth believing, then opens the
 * server. Returns true once it is running.
 *
 * Either view may drive it, but only one of them: both are drawn once
 * per frame, so letting both count would report twice the real rate --
 * and the encoder's bitrate and keyframe interval are both derived from
 * that number. The first to arrive claims it; the other waits, which
 * costs it nothing because the server it is waiting for is the same one.
 */
static bool MeasureRateAndStart(bool fromPad)
{
    if (g_server)
        return true;
    if (g_rate_frames > 0 && g_rate_from_pad != fromPad)
        return false;
    g_rate_from_pad = fromPad;

    g_rate_frames++;
    if (g_rate_frames <= kWarmupFrames)
        return false;
    if (g_rate_frames == kWarmupFrames + 1)
    {
        g_rate_first_us = bs_now_us();
        return false;
    }

    const uint32 elapsed = bs_now_us() - g_rate_first_us;
    if (g_rate_frames < kWarmupFrames + kMeasureFrames || elapsed == 0)
        return false;

    double fps = (double)(g_rate_frames - kWarmupFrames - 1) * 1000000.0 / (double)elapsed;
    if (fps < 5.0)   fps = 5.0;
    if (fps > 120.0) fps = 120.0;
    g_fps = (int)(fps + 0.5);

    Start();
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
                "               backend, so nothing will be streamed. OpenGL and\n"
                "               Vulkan both work.\n");
        }
        return;
    }

    /*
     * A graphic pack or a resolution setting can change the pad view's
     * size mid-game. The connection survives it: the server renegotiates
     * with whoever is watching rather than dropping them over a setting.
     */
    if (g_server && (w != g_width || h != g_height))
    {
        if (bs_mailbox_resize(g_source, w, h))
        {
            g_width = w;
            g_height = h;
        }
    }

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
        if (!MeasureRateAndStart(true))
            return;
    }

    bs_mailbox_submit(g_source, g_pixels.data(), w * 4);
}

/*
 * The television picture, for a client that asked for it.
 *
 * The first line is the whole reason this is cheap. Reading a texture
 * back out of the GPU every frame is not free, and a second screen that
 * nobody is watching should not cost the game anything at all -- so
 * unless somebody has actually asked, this returns before touching the
 * renderer. Turn the option off in every client and Cemu is back to
 * exactly the work it did before this existed.
 *
 * The server is never started from here. The GamePad view is what
 * measures the frame rate and opens the port; this attaches to a server
 * that is already up, or does nothing.
 */
void SubmitTvView(LatteTextureView* texView)
{
    if (!GetConfig().bottom_screen_enabled || !texView)
        return;

    if (!g_server)
    {
        /*
         * The GamePad window may never be opened, and until now nothing
         * was served until it was: the server was started from the pad
         * view's own path, so a game running on the television alone
         * had no port to connect to at all. That is a condition worth
         * removing rather than documenting.
         *
         * The GamePad's own size stands in until its view turns up and
         * says otherwise -- the mailbox renegotiates, which is the same
         * thing that happens when a graphic pack changes it. A client
         * watching the bottom screen before that window exists sees
         * nothing, which is the truth: Cemu is not drawing it.
         */
        if (g_width <= 0 || g_height <= 0)
        {
            g_width = BS_WIIU_WIDTH;
            g_height = BS_WIIU_HEIGHT;
        }
        if (!MeasureRateAndStart(false))
            return;
    }
    if (!bs_server_wants_screen(g_server, BS_SCREEN_TOP))
        return;

    sint32 w = 0, h = 0;
    if (!g_renderer || !g_renderer->ReadbackViewRGBA(texView, g_top_pixels, w, h))
        return;   // the pad view has already said so, once, for both

    if (!g_top_source)
    {
        /*
         * The same frame rate as the GamePad view, because it is the
         * same game: both are drawn from the one render loop, and
         * measuring it twice would only produce two answers to one
         * question.
         */
        g_top_source = bs_mailbox_create(BS_CONSOLE_WIIU, w, h, g_fps,
                                         BS_PIXFMT_RGBA, 0, 0);
        if (!g_top_source)
        {
            fprintf(stderr, "bottom_screen: cannot create the TV mailbox\n");
            return;
        }
        g_top_width = w;
        g_top_height = h;
        bs_server_set_top_source(g_server, g_top_source);
        fprintf(stderr, "bottom_screen: the TV picture is %dx%d\n", w, h);
    }
    else if (w != g_top_width || h != g_top_height)
    {
        /* A graphic pack or a resolution setting moved it. Same answer
         * as for the GamePad view: renegotiate, do not disconnect. */
        if (bs_mailbox_resize(g_top_source, w, h))
        {
            g_top_width = w;
            g_top_height = h;
        }
    }

    bs_mailbox_submit(g_top_source, g_top_pixels.data(), w * 4);
}

/*
 * The Wii U's own keyboard, typed into from wherever you are watching.
 *
 * Cemu draws this keyboard into the picture itself, so a client already
 * sees it -- what it could not do was type. The characters had to come
 * from a keyboard attached to this machine, which from a phone in
 * another room is the same as no keyboard at all and a game that waits
 * for ever.
 *
 * So the question goes to the clients, and the answer is fed back one
 * character at a time the way a keyboard would deliver it: swkbd takes
 * a character code, with 8 for backspace and 13 for return, and 13 is
 * what closes it.
 *
 * Nobody watching means bs_server_prompt returns 0 and none of this
 * happens, which leaves the keyboard exactly as it was: waiting for a
 * key from this machine.
 */
void PollKeyboard()
{
    static uint16 asked = 0;
    static bool was_open = false;

    const bool open = swkbd_hasKeyboardInputHook();
    if (!g_server) {
        was_open = open;
        return;
    }

    if (open && !was_open && asked == 0) {
        asked = bs_server_prompt(g_server, BS_PROMPT_TEXT,
                                 "The Wii U is asking for some text",
                                 nullptr, 0, 255, 0);
    } else if (!open && asked != 0) {
        /* Closed by something else -- the game gave up, or somebody
         * typed here. A question nobody is waiting on is a box somebody
         * is still typing into. */
        bs_server_prompt_cancel(g_server, asked);
        asked = 0;
    }
    was_open = open;

    if (asked == 0)
        return;

    char text[256] = "";
    const int state = bs_server_prompt_poll(g_server, asked, text, sizeof(text), nullptr);
    if (state == 0)
        return;
    if (state == 1) {
        for (const char* c = text; *c; c++) {
            /* Only what swkbd itself accepts; anything else it drops on
             * the floor, and sending it is noise. */
            swkbd_keyInput(static_cast<uint32>(static_cast<unsigned char>(*c)));
        }
        swkbd_keyInput(13);   // return, which is what closes it
    }
    asked = 0;
}

void ApplyInput()
{
    if (!g_server)
        return;

    /* Once a frame, on the render thread -- the same one swkbd is drawn
     * from, so its state is as valid here as it is there. */
    PollKeyboard();

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

namespace
{
    // Fold to stereo. Mono is duplicated; more than two channels keep
    // the front pair, which is where the game's own mix puts almost
    // everything that matters on a handheld.
    void FoldToStereo(const sint16* samples, int frames, int channels,
                      std::vector<int16_t>& out)
    {
        out.resize((size_t)frames * 2);
        for (int i = 0; i < frames; i++)
        {
            const sint16 l = samples[(size_t)i * channels];
            const sint16 r = channels > 1 ? samples[(size_t)i * channels + 1] : l;
            out[(size_t)i * 2]     = l;
            out[(size_t)i * 2 + 1] = r;
        }
    }

    /*
     * A Wii U has two outputs and a game uses both: the television and
     * the GamePad's own speakers. Sending only the television loses
     * whatever a title puts on the pad, which on this project is the
     * screen you are actually looking at.
     *
     * They arrive as separate DMA blocks of the same length, so one of
     * each is held and the pair is summed. If a title drives only one of
     * them the held block is sent on its own when the next one of its
     * kind arrives, which costs 3ms and never waits for something that
     * is not coming.
     */
    std::mutex           g_mixLock;
    std::vector<int16_t> g_pending[2];   // 0 = TV, 1 = GamePad
    bool                 g_have[2] = { false, false };

    void SendMixed()
    {
        std::vector<int16_t>& a = g_pending[0];
        std::vector<int16_t>& b = g_pending[1];

        // Whoever is listening decides which of the two they want.
        const int want = bs_server_audio_source(g_server);
        if (want == BS_AUDIO_TV)
        {
            if (g_have[0])
                bs_mailbox_submit_audio(g_source, a.data(), (int)(a.size() / 2));
            g_have[0] = g_have[1] = false;
            return;
        }
        if (want == BS_AUDIO_PAD)
        {
            if (g_have[1])
                bs_mailbox_submit_audio(g_source, b.data(), (int)(b.size() / 2));
            g_have[0] = g_have[1] = false;
            return;
        }

        if (g_have[0] && g_have[1])
        {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; i++)
            {
                int v = (int)a[i] + (int)b[i];
                a[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            bs_mailbox_submit_audio(g_source, a.data(), (int)(a.size() / 2));
        }
        else if (g_have[0])
            bs_mailbox_submit_audio(g_source, a.data(), (int)(a.size() / 2));
        else if (g_have[1])
            bs_mailbox_submit_audio(g_source, b.data(), (int)(b.size() / 2));

        g_have[0] = g_have[1] = false;
    }
}

void SubmitAudio(const sint16* samples, int frames, int channels, bool padOutput)
{
    if (!g_server || !samples || frames <= 0 || channels < 1)
        return;

    const int which = padOutput ? 1 : 0;
    std::lock_guard<std::mutex> lock(g_mixLock);

    // Already holding one of these and its partner never came: send it
    // rather than drop it, then take its place.
    if (g_have[which])
        SendMixed();

    FoldToStereo(samples, frames, channels, g_pending[which]);
    g_have[which] = true;

    if (g_have[0] && g_have[1])
        SendMixed();
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
    const bool held = (g_held & (1ull << vpadButtonId)) != 0;

    // Said once, like the touch and stick lines. This one also proves
    // the emulated controller exists at all: without one Cemu never
    // calls this, and a client's buttons vanish with nothing to show
    // for it.
    static bool announced = false;
    if (held && !announced)
    {
        announced = true;
        fprintf(stderr, "bottom_screen: first button from a client (id %d)\n",
                vpadButtonId);
    }
    return held;
}

}
