#pragma once

#include <cstdint>

class LatteTextureView;

/*
 * The Cemu side of bottom_screen_server.
 *
 * Everything below this header is plain C, shared with the standalone
 * server and with the melonDS backend. This file and its .cpp are the
 * only C++ in the path, and only because Cemu's API is C++.
 *
 * Two hooks, both in code that already exists for other reasons:
 *
 *   video  LatteRenderTarget_copyToBackbuffer, right where Cemu already
 *          calls HandleScreenshotRequest for the pad view -- the one
 *          point the GamePad image passes through.
 *
 *   input  InputManager's m_pad_touch, the same fields PadViewFrame
 *          writes when someone drags a mouse across the pad window.
 *
 * Controlled from Cemu's own settings (General tab): an on/off switch,
 * on by default, and a port.
 */

namespace BottomScreen
{

/* Idempotent. Called on the first submitted frame, so nothing opens
 * until a game is actually running. */
void Start();
void Stop();
bool IsRunning();

/*
 * Reads the pad view back off the GPU and hands it to the server.
 *
 * The texture is whatever size Cemu is rendering at, which follows its
 * internal resolution -- not the GamePad's own 854x480. The stream is
 * sized from the first frame and the client scales it while keeping the
 * aspect ratio, so a higher internal resolution simply arrives sharper.
 */
void SubmitPadView(LatteTextureView* texView);

/*
 * The television picture, offered to any client that asks for it.
 *
 * Returns immediately -- before reading anything back from the GPU --
 * unless somebody is actually watching it, so a second screen nobody
 * has switched on costs the game nothing.
 */
void SubmitTvView(LatteTextureView* texView);

/*
 * Pushes what the clients are holding into Cemu's input state. Called
 * once per frame from the same place the video is submitted, so the two
 * stay on the same clock.
 */
void ApplyInput();

/*
 * True while a connected client holds the given VPADController button.
 *
 * Merged with the local mapping rather than replacing it, so a pad or a
 * keyboard on the host keeps working while someone plays from a phone.
 * Takes a VPADController::ButtonId; anything the Wii U does not have is
 * simply never reported.
 */
bool IsButtonHeld(int vpadButtonId);

/*
 * A stick a client is deflecting: 0 for the left, 1 for the right.
 *
 * Returns false when the client is leaving it centred, so the local
 * mapping keeps the stick and a pad on the host still works. Values are
 * -1..1, y positive upwards, which is what VPADController expects.
 */
bool GetStick(int index, float& x, float& y);

/*
 * Sound, as Cemu mixes it for the TV: 48 kHz, 16-bit interleaved,
 * however many channels the output device has.
 *
 * The TV mix rather than the GamePad's own: the Wii U does have a
 * separate DRC stream for the pad speaker, but Cemu leaves its volume at
 * zero by default, so that path usually does not exist. This is the
 * sound a player expects to hear, and it is always there.
 *
 * Anything above stereo is folded down to two channels, because Opus is
 * configured for two and a surround mix on a phone is not worth the
 * bitrate.
 */
/* padOutput picks which of the Wii U's two outputs this block came
 * from; the two are summed before being sent. */
void SubmitAudio(const sint16* samples, int frames, int channels,
                 bool padOutput);

}
