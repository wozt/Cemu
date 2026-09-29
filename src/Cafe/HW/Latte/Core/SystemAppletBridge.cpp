#include "Cafe/HW/Latte/Core/SystemAppletBridge.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "config/CemuConfig.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

#if BOOST_OS_WINDOWS
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SystemAppletBridge
{
namespace
{
	constexpr uint32 kMagic = 0x314D4248; // "HBM1"
	constexpr uint32 kMaxWidth = 2560;
	constexpr uint32 kMaxHeight = 1440;
	constexpr size_t kMaxFrameBytes = (size_t)kMaxWidth * kMaxHeight * 4;
	static_assert(std::atomic<uint32>::is_always_lock_free,
		"The system applet channel requires a lock-free 32-bit sequence counter");

	struct alignas(64) SharedFrame
	{
		uint32 magic = kMagic;
		uint32 capacity = (uint32)kMaxFrameBytes;
		std::atomic<uint32> sequence{ 0 };
		uint32 width = 0;
		uint32 height = 0;
		uint32 stride = 0;
		uint32 reserved = 0;
	};

	struct Channel
	{
		std::string name;
		void* region = nullptr;
#if BOOST_OS_WINDOWS
		HANDLE mapping = nullptr;
#else
		int descriptor = -1;
#endif
		SharedFrame* header = nullptr;
		uint8* pixels = nullptr;
	};

	std::mutex g_consumer_mutex;
	std::mutex g_publisher_mutex;
	Channel g_consumer;
	Channel g_publisher;
	uint32 g_last_sequence = 0;
	std::vector<uint8> g_rgba;
	std::vector<uint8> g_rgb;
	std::vector<uint8> g_publish_pixels;
	ImTextureID g_texture = nullptr;
	Vector2i g_texture_size{};

	std::string NativeName(std::string_view name)
	{
#if BOOST_OS_WINDOWS
		return std::string(name);
#else
		return "/" + std::string(name);
#endif
	}

	void CloseChannel(Channel& channel, bool remove)
	{
		const std::string name = channel.name;
		channel.header = nullptr;
		channel.pixels = nullptr;
		if (channel.region)
		{
#if BOOST_OS_WINDOWS
			UnmapViewOfFile(channel.region);
#else
			munmap(channel.region, sizeof(SharedFrame) + kMaxFrameBytes);
#endif
			channel.region = nullptr;
		}
#if BOOST_OS_WINDOWS
		if (channel.mapping)
			CloseHandle(channel.mapping);
		channel.mapping = nullptr;
#else
		if (channel.descriptor >= 0)
			close(channel.descriptor);
		channel.descriptor = -1;
#endif
		channel.name.clear();
		if (remove && !name.empty())
		{
#if !BOOST_OS_WINDOWS
			const std::string native_name = NativeName(name);
			shm_unlink(native_name.c_str());
#endif
		}
	}

	void OpenChannel(Channel& channel, std::string_view name, bool create)
	{
		const size_t region_size = sizeof(SharedFrame) + kMaxFrameBytes;
		channel.name.assign(name);
#if BOOST_OS_WINDOWS
		if (create)
		{
			SetLastError(ERROR_SUCCESS);
			channel.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				(DWORD)(region_size >> 32), (DWORD)region_size, channel.name.c_str());
			if (!channel.mapping || GetLastError() == ERROR_ALREADY_EXISTS)
				throw std::runtime_error("CreateFileMapping failed");
		}
		else
		{
			channel.mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, channel.name.c_str());
			if (!channel.mapping)
				throw std::runtime_error("OpenFileMapping failed");
		}
		channel.region = MapViewOfFile(channel.mapping, FILE_MAP_ALL_ACCESS, 0, 0, region_size);
		if (!channel.region)
			throw std::runtime_error("MapViewOfFile failed");
#else
		const std::string native_name = NativeName(name);
		if (create)
			shm_unlink(native_name.c_str());
		channel.descriptor = shm_open(native_name.c_str(),
			create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR, 0600);
		if (channel.descriptor < 0)
			throw std::runtime_error(std::string("shm_open failed: ") + std::strerror(errno));
		if (create && ftruncate(channel.descriptor, (off_t)region_size) != 0)
			throw std::runtime_error(std::string("ftruncate failed: ") + std::strerror(errno));
		channel.region = mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			channel.descriptor, 0);
		if (channel.region == MAP_FAILED)
		{
			channel.region = nullptr;
			throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
		}
#endif
		channel.header = static_cast<SharedFrame*>(channel.region);
		channel.pixels = reinterpret_cast<uint8*>(channel.header + 1);
	}

	bool ReadLatestFrame(uint32& width, uint32& height)
	{
		if (!g_consumer.header)
			return false;

		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const uint32 before = g_consumer.header->sequence.load(std::memory_order_acquire);
			if ((before & 1) || before == g_last_sequence)
				return false;

			width = g_consumer.header->width;
			height = g_consumer.header->height;
			const uint32 stride = g_consumer.header->stride;
			if (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight ||
				stride < width * 4 || (size_t)stride * height > kMaxFrameBytes)
				return false;

			g_rgba.resize((size_t)width * height * 4);
			for (uint32 row = 0; row < height; ++row)
				std::memcpy(g_rgba.data() + (size_t)row * width * 4,
					g_consumer.pixels + (size_t)row * stride, (size_t)width * 4);

			std::atomic_thread_fence(std::memory_order_acquire);
			const uint32 after = g_consumer.header->sequence.load(std::memory_order_acquire);
			if (before == after && !(after & 1))
			{
				g_last_sequence = after;
				return true;
			}
		}
		return false;
	}

	void DeleteTexture()
	{
		if (g_texture && g_renderer)
			g_renderer->DeleteTexture(g_texture);
		g_texture = nullptr;
		g_texture_size = {};
	}
}

bool StartConsumer(std::string_view name)
{
	std::lock_guard lock(g_consumer_mutex);
	CloseChannel(g_consumer, true);
	try
	{
		OpenChannel(g_consumer, name, true);
		g_consumer.header = new (g_consumer.region) SharedFrame();
		g_last_sequence = 0;
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to create the system applet compositor: {}", ex.what());
		CloseChannel(g_consumer, true);
		return false;
	}
}

void StopConsumer()
{
	std::lock_guard lock(g_consumer_mutex);
	CloseChannel(g_consumer, true);
	g_last_sequence = 0;
	g_rgba.clear();
	g_rgb.clear();
}

bool StartPublisher(std::string_view name)
{
	std::lock_guard lock(g_publisher_mutex);
	CloseChannel(g_publisher, false);
	try
	{
		OpenChannel(g_publisher, name, false);
		if (g_publisher.header->magic != kMagic || g_publisher.header->capacity != kMaxFrameBytes)
			throw std::runtime_error("incompatible shared frame header");
		g_publisher.pixels = reinterpret_cast<uint8*>(g_publisher.header + 1);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to connect the system applet compositor: {}", ex.what());
		CloseChannel(g_publisher, false);
		return false;
	}
}

void StopPublisher()
{
	std::lock_guard lock(g_publisher_mutex);
	CloseChannel(g_publisher, false);
	g_publish_pixels.clear();
}

bool IsPublisher()
{
	std::lock_guard lock(g_publisher_mutex);
	return g_publisher.header != nullptr;
}

void PublishTvFrame(LatteTextureView* texture_view)
{
	std::lock_guard lock(g_publisher_mutex);
	if (!g_publisher.header || !g_renderer || !texture_view)
		return;

	sint32 width = 0;
	sint32 height = 0;
	if (!g_renderer->ReadbackViewRGBA(texture_view, g_publish_pixels, width, height) ||
		width <= 0 || height <= 0 || width > (sint32)kMaxWidth || height > (sint32)kMaxHeight)
		return;

	const size_t bytes = (size_t)width * height * 4;
	if (g_publish_pixels.size() < bytes)
		return;

	uint32 sequence = g_publisher.header->sequence.load(std::memory_order_relaxed);
	if (sequence & 1)
		++sequence;
	g_publisher.header->sequence.store(sequence + 1, std::memory_order_release);
	g_publisher.header->width = (uint32)width;
	g_publisher.header->height = (uint32)height;
	g_publisher.header->stride = (uint32)width * 4;
	std::memcpy(g_publisher.pixels, g_publish_pixels.data(), bytes);
	std::atomic_thread_fence(std::memory_order_release);
	g_publisher.header->sequence.store(sequence + 2, std::memory_order_release);
}

void RenderConsumerFrame()
{
	std::lock_guard lock(g_consumer_mutex);
	if (!g_consumer.header)
	{
		DeleteTexture();
		return;
	}

	uint32 width = 0;
	uint32 height = 0;
	if (!ReadLatestFrame(width, height) || !g_renderer)
		return;

	g_rgb.resize((size_t)width * height * 3);
	for (size_t source = 0, destination = 0; source < g_rgba.size(); source += 4, destination += 3)
	{
		g_rgb[destination + 0] = g_rgba[source + 0];
		g_rgb[destination + 1] = g_rgba[source + 1];
		g_rgb[destination + 2] = g_rgba[source + 2];
	}

	const Vector2i size{ (sint32)width, (sint32)height };
	if (g_texture && g_texture_size != size)
		DeleteTexture();

	if (!g_renderer->BeginFrame(true))
		return;
	if (!g_texture)
	{
		g_texture = g_renderer->GenerateTexture(g_rgb, size);
		g_texture_size = size;
	}
	else
	{
		g_renderer->UpdateTexture(g_texture, g_rgb, size);
	}

	if (!g_renderer->ImguiBegin(true))
		return;

	if (g_texture)
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImVec2 image_min{ 0.0f, 0.0f };
		ImVec2 image_max = display;
		if (GetConfig().fullscreen_scaling == kKeepAspectRatio && width && height)
		{
			const float scale = std::min(display.x / (float)width, display.y / (float)height);
			const ImVec2 image_size{ width * scale, height * scale };
			image_min = { (display.x - image_size.x) * 0.5f, (display.y - image_size.y) * 0.5f };
			image_max = { image_min.x + image_size.x, image_min.y + image_size.y };
		}
		ImGui::GetBackgroundDrawList()->AddImage(g_texture, image_min, image_max);
	}
	g_renderer->ImguiEnd();
	g_renderer->SwapBuffers(true, false);
}

void RendererShutdown()
{
	std::lock_guard lock(g_consumer_mutex);
	DeleteTexture();
}
}
