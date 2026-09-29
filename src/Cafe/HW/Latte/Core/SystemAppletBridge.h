#pragma once

#include <string>
#include <string_view>

class LatteTextureView;

namespace SystemAppletBridge
{
	// The foreground Cemu owns the channel and composites the frames.
	bool StartConsumer(std::string_view name);
	void StopConsumer();
	// Routes a physical GamePad-window touch to the isolated system applet.
	// Returns false when no applet is active, so the caller can handle it normally.
	bool SubmitPadTouch(sint32 x, sint32 y, bool pressed);

	// The isolated system-title Cemu publishes its TV and GamePad surfaces here.
	bool StartPublisher(std::string_view name);
	void StopPublisher();
	bool IsPublisher();
	void PublishFrame(LatteTextureView* texture_view, bool pad_view);
	// Reads the applet touch in normalized GamePad coordinates.
	bool GetPadTouch(float& x, float& y);

	// Called only on the Latte thread of the foreground Cemu.
	void RenderConsumerFrames();
	void RendererShutdown();
}
