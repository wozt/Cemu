#include "Cafe/HW/Latte/Core/SystemAppletBridge.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "config/CemuConfig.h"
#include "WindowSystem.h"

#include <imgui.h>

#include <algorithm>
#include <array>
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
	constexpr uint32 kMagic = 0x334D4248; // "HBM3"
	constexpr uint32 kMaxWidth = 2560;
	constexpr uint32 kMaxHeight = 1440;
	constexpr size_t kMaxFrameBytes = (size_t)kMaxWidth * kMaxHeight * 4;
	constexpr size_t kSurfaceCount = 2;
	static_assert(std::atomic<uint32>::is_always_lock_free,
		"The system applet channel requires a lock-free 32-bit sequence counter");

	struct alignas(64) SharedSurface
	{
		std::atomic<uint32> sequence{ 0 };
		uint32 width = 0;
		uint32 height = 0;
		uint32 stride = 0;
		uint32 reserved[11]{};
	};

	struct alignas(64) SharedInput
	{
		std::atomic<uint32> sequence{ 0 };
		uint32 x = 0;
		uint32 y = 0;
		uint32 pressed = 0;
		uint32 press_generation = 0;
		uint32 reserved[11]{};
	};

	struct alignas(64) SharedFrames
	{
		uint32 magic = kMagic;
		uint32 capacity = (uint32)kMaxFrameBytes;
		uint32 surface_count = (uint32)kSurfaceCount;
		uint32 reserved[13]{};
		SharedSurface surfaces[kSurfaceCount];
		SharedInput input;
	};
	constexpr size_t kRegionSize = sizeof(SharedFrames) + kMaxFrameBytes * kSurfaceCount;

	struct Channel
	{
		std::string name;
		void* region = nullptr;
#if BOOST_OS_WINDOWS
		HANDLE mapping = nullptr;
#else
		int descriptor = -1;
#endif
		SharedFrames* header = nullptr;
		std::array<uint8*, kSurfaceCount> pixels{};
	};

	struct ConsumerSurface
	{
		uint32 last_sequence = 0;
		std::vector<uint8> rgba;
		std::vector<uint8> rgb;
		ImTextureID texture = nullptr;
		Vector2i texture_size{};
	};

	std::mutex g_consumer_mutex;
	std::mutex g_publisher_mutex;
	Channel g_consumer;
	Channel g_publisher;
	std::array<ConsumerSurface, kSurfaceCount> g_consumer_surfaces;
	std::vector<uint8> g_publish_pixels;
	bool g_consumer_touch_pressed = false;
	uint32 g_publisher_press_generation = 0;

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
		channel.pixels.fill(nullptr);
		if (channel.region)
		{
#if BOOST_OS_WINDOWS
			UnmapViewOfFile(channel.region);
#else
			munmap(channel.region, kRegionSize);
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
		channel.name.assign(name);
#if BOOST_OS_WINDOWS
		if (create)
		{
			SetLastError(ERROR_SUCCESS);
			channel.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				(DWORD)(kRegionSize >> 32), (DWORD)kRegionSize, channel.name.c_str());
			if (!channel.mapping || GetLastError() == ERROR_ALREADY_EXISTS)
				throw std::runtime_error("CreateFileMapping failed");
		}
		else
		{
			channel.mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, channel.name.c_str());
			if (!channel.mapping)
				throw std::runtime_error("OpenFileMapping failed");
		}
		channel.region = MapViewOfFile(channel.mapping, FILE_MAP_ALL_ACCESS, 0, 0, kRegionSize);
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
		if (create && ftruncate(channel.descriptor, (off_t)kRegionSize) != 0)
			throw std::runtime_error(std::string("ftruncate failed: ") + std::strerror(errno));
		channel.region = mmap(nullptr, kRegionSize, PROT_READ | PROT_WRITE, MAP_SHARED,
			channel.descriptor, 0);
		if (channel.region == MAP_FAILED)
		{
			channel.region = nullptr;
			throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
		}
#endif
		channel.header = static_cast<SharedFrames*>(channel.region);
		uint8* pixels = reinterpret_cast<uint8*>(channel.header + 1);
		for (size_t surface = 0; surface < kSurfaceCount; ++surface)
			channel.pixels[surface] = pixels + surface * kMaxFrameBytes;
	}

	bool ReadLatestFrame(size_t surface_index, uint32& width, uint32& height)
	{
		if (!g_consumer.header)
			return false;
		SharedSurface& shared = g_consumer.header->surfaces[surface_index];
		ConsumerSurface& local = g_consumer_surfaces[surface_index];

		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const uint32 before = shared.sequence.load(std::memory_order_acquire);
			if ((before & 1) || before == local.last_sequence)
				return false;

			width = shared.width;
			height = shared.height;
			const uint32 stride = shared.stride;
			if (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight ||
				stride < width * 4 || (size_t)stride * height > kMaxFrameBytes)
				return false;

			local.rgba.resize((size_t)width * height * 4);
			for (uint32 row = 0; row < height; ++row)
				std::memcpy(local.rgba.data() + (size_t)row * width * 4,
					g_consumer.pixels[surface_index] + (size_t)row * stride, (size_t)width * 4);

			std::atomic_thread_fence(std::memory_order_acquire);
			const uint32 after = shared.sequence.load(std::memory_order_acquire);
			if (before == after && !(after & 1))
			{
				local.last_sequence = after;
				return true;
			}
		}
		return false;
	}

	void DeleteTexture(ConsumerSurface& surface)
	{
		if (surface.texture && g_renderer)
			g_renderer->DeleteTexture(surface.texture);
		surface.texture = nullptr;
		surface.texture_size = {};
	}
}

bool StartConsumer(std::string_view name)
{
	std::lock_guard lock(g_consumer_mutex);
	CloseChannel(g_consumer, true);
	try
	{
		OpenChannel(g_consumer, name, true);
		g_consumer.header = new (g_consumer.region) SharedFrames();
		g_consumer_touch_pressed = false;
		for (ConsumerSurface& surface : g_consumer_surfaces)
			surface.last_sequence = 0;
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
	g_consumer_touch_pressed = false;
	for (ConsumerSurface& surface : g_consumer_surfaces)
	{
		surface.last_sequence = 0;
		surface.rgba.clear();
		surface.rgb.clear();
	}
}

bool SubmitPadTouch(sint32 x, sint32 y, bool pressed)
{
	std::lock_guard lock(g_consumer_mutex);
	if (!g_consumer.header)
		return false;

	const Vector2i source_size = g_consumer_surfaces[1].texture_size;
	int window_width = 0;
	int window_height = 0;
	WindowSystem::GetPadWindowPhysSize(window_width, window_height);
	if (source_size.x <= 0 || source_size.y <= 0 || window_width <= 0 || window_height <= 0)
		return true;

	float image_x = 0.0f;
	float image_y = 0.0f;
	float image_width = (float)window_width;
	float image_height = (float)window_height;
	if (GetConfig().fullscreen_scaling == kKeepAspectRatio)
	{
		const float scale = std::min(image_width / (float)source_size.x,
			image_height / (float)source_size.y);
		image_width = source_size.x * scale;
		image_height = source_size.y * scale;
		image_x = (window_width - image_width) * 0.5f;
		image_y = (window_height - image_height) * 0.5f;
	}

	const float normalized_x = std::clamp((x - image_x) / image_width, 0.0f, 1.0f);
	const float normalized_y = std::clamp((y - image_y) / image_height, 0.0f, 1.0f);
	SharedInput& input = g_consumer.header->input;
	uint32 sequence = input.sequence.load(std::memory_order_relaxed);
	if (sequence & 1)
		++sequence;
	input.sequence.store(sequence + 1, std::memory_order_release);
	input.x = (uint32)(normalized_x * 65535.0f + 0.5f);
	input.y = (uint32)(normalized_y * 65535.0f + 0.5f);
	input.pressed = pressed ? 1 : 0;
	if (pressed && !g_consumer_touch_pressed)
		++input.press_generation;
	g_consumer_touch_pressed = pressed;
	std::atomic_thread_fence(std::memory_order_release);
	input.sequence.store(sequence + 2, std::memory_order_release);
	return true;
}

bool StartPublisher(std::string_view name)
{
	std::lock_guard lock(g_publisher_mutex);
	CloseChannel(g_publisher, false);
	try
	{
		OpenChannel(g_publisher, name, false);
		if (g_publisher.header->magic != kMagic ||
			g_publisher.header->capacity != kMaxFrameBytes ||
			g_publisher.header->surface_count != kSurfaceCount)
			throw std::runtime_error("incompatible shared frame header");
		g_publisher_press_generation = g_publisher.header->input.press_generation;
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to connect the system applet compositor: {}", ex.what());
		CloseChannel(g_publisher, false);
		return false;
	}
}

bool GetPadTouch(float& x, float& y)
{
	std::lock_guard lock(g_publisher_mutex);
	if (!g_publisher.header)
		return false;

	SharedInput& input = g_publisher.header->input;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		const uint32 before = input.sequence.load(std::memory_order_acquire);
		if (before & 1)
			continue;
		const uint32 shared_x = input.x;
		const uint32 shared_y = input.y;
		const bool pressed = input.pressed != 0;
		const uint32 press_generation = input.press_generation;
		std::atomic_thread_fence(std::memory_order_acquire);
		const uint32 after = input.sequence.load(std::memory_order_acquire);
		if (before != after || (after & 1))
			continue;

		x = shared_x / 65535.0f;
		y = shared_y / 65535.0f;
		const bool missed_press = press_generation != g_publisher_press_generation;
		g_publisher_press_generation = press_generation;
		return pressed || missed_press;
	}
	return false;
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

void PublishFrame(LatteTextureView* texture_view, bool pad_view)
{
	std::lock_guard lock(g_publisher_mutex);
	if (!g_publisher.header || !g_renderer || !texture_view)
		return;
	const size_t surface_index = pad_view ? 1 : 0;
	SharedSurface& shared = g_publisher.header->surfaces[surface_index];

	sint32 width = 0;
	sint32 height = 0;
	if (!g_renderer->ReadbackViewRGBA(texture_view, g_publish_pixels, width, height) ||
		width <= 0 || height <= 0 || width > (sint32)kMaxWidth || height > (sint32)kMaxHeight)
		return;

	const size_t bytes = (size_t)width * height * 4;
	if (g_publish_pixels.size() < bytes)
		return;

	uint32 sequence = shared.sequence.load(std::memory_order_relaxed);
	if (sequence & 1)
		++sequence;
	shared.sequence.store(sequence + 1, std::memory_order_release);
	shared.width = (uint32)width;
	shared.height = (uint32)height;
	shared.stride = (uint32)width * 4;
	std::memcpy(g_publisher.pixels[surface_index], g_publish_pixels.data(), bytes);
	std::atomic_thread_fence(std::memory_order_release);
	shared.sequence.store(sequence + 2, std::memory_order_release);
}

namespace
{
	void RenderConsumerSurface(size_t surface_index, bool main_window)
	{
		ConsumerSurface& surface = g_consumer_surfaces[surface_index];
		uint32 width = 0;
		uint32 height = 0;
		if (!ReadLatestFrame(surface_index, width, height) || !g_renderer)
			return;

		surface.rgb.resize((size_t)width * height * 3);
		for (size_t source = 0, destination = 0; source < surface.rgba.size(); source += 4, destination += 3)
		{
			surface.rgb[destination + 0] = surface.rgba[source + 0];
			surface.rgb[destination + 1] = surface.rgba[source + 1];
			surface.rgb[destination + 2] = surface.rgba[source + 2];
		}

		const Vector2i size{ (sint32)width, (sint32)height };
		if (surface.texture && surface.texture_size != size)
			DeleteTexture(surface);

		if (!g_renderer->BeginFrame(main_window))
			return;
		if (!surface.texture)
		{
			surface.texture = g_renderer->GenerateTexture(surface.rgb, size);
			surface.texture_size = size;
		}
		else
		{
			g_renderer->UpdateTexture(surface.texture, surface.rgb, size);
		}

		if (!g_renderer->ImguiBegin(main_window))
			return;

		if (surface.texture)
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
			ImGui::GetBackgroundDrawList()->AddImage(surface.texture, image_min, image_max);
		}
		g_renderer->ImguiEnd();
		g_renderer->SwapBuffers(main_window, !main_window);
	}
}

void RenderConsumerFrames()
{
	std::lock_guard lock(g_consumer_mutex);
	if (!g_consumer.header)
	{
		for (ConsumerSurface& surface : g_consumer_surfaces)
			DeleteTexture(surface);
		return;
	}

	RenderConsumerSurface(0, true);
	RenderConsumerSurface(1, false);
}

void RendererShutdown()
{
	std::lock_guard lock(g_consumer_mutex);
	for (ConsumerSurface& surface : g_consumer_surfaces)
		DeleteTexture(surface);
}
}
