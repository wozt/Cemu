#pragma once

#include <string>
#include <string_view>

class LatteTextureView;

namespace SystemAppletBridge
{
	// The foreground Cemu owns the channel and composites the frames.
	bool StartConsumer(std::string_view name);
	void StopConsumer();

	// The isolated system-title Cemu publishes its TV surface here.
	bool StartPublisher(std::string_view name);
	void StopPublisher();
	bool IsPublisher();
	void PublishFrame(LatteTextureView* texture_view, bool pad_view);

	// Called only on the Latte thread of the foreground Cemu.
	void RenderConsumerFrames();
	void RendererShutdown();
}
