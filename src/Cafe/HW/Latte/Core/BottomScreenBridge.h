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
 * Pushes what the clients are holding into Cemu's input state. Called
 * once per frame from the same place the video is submitted, so the two
 * stay on the same clock.
 */
void ApplyInput();

}
