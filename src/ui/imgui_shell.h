// license:BSD-3-Clause
//
// Shared Dear ImGui plumbing for every window that paints the panel: the
// main windows (window_sdl.cpp / window_win.cpp / window_mac.mm), the
// plug-in windows (vst3/view_win.cpp / vst3/view_mac.mm) and the headless
// shot (ui/shot.h). Contexts and GPU targets stay per window (they cannot
// be shared); only the code is. The PC editor windows (ui/pc_window*)
// keep their own older setup and are untouched.
//
// The SDL family lives in imgui_shell_sdl.h next to this file: macOS
// plug-ins must not gain an SDL dependency through this header.

#ifndef S_MU2000_UI_IMGUI_SHELL_H
#define S_MU2000_UI_IMGUI_SHELL_H

#pragma once

#include "ui/draw_imgui.h"

#include "imgui.h"

#include <functional>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <d3d11.h>
#include <windows.h>
#elif defined(__APPLE__)
#include "backends/imgui_impl_metal.h"
#include <CoreText/CoreText.h>
#else
#include <fontconfig/fontconfig.h>
#endif

#ifdef __OBJC__
@class NSView;
@class CAMetalLayer;
@protocol CAMetalDrawable;
@protocol MTLCommandQueue;
#endif

namespace ui {
namespace imshell {

// ---- fonts ---------------------------------------------------------------
//
// The panel's CJK font in all three slots at 16 px, found the way each
// platform finds it (never a hard-coded path). Falls back to the embedded
// font (slots stay null: English only). Adds to the current context, so
// call it after SetCurrentContext (new_context below does both).
inline im::fonts panel_fonts()
{
	im::fonts f{};
#if defined(_WIN32)
	static const char *const NAMES[] = {
		"C:\\Windows\\Fonts\\YuGothM.ttc",
		"C:\\Windows\\Fonts\\meiryo.ttc",
		"C:\\Windows\\Fonts\\msgothic.ttc",
	};
	for (const char *path : NAMES) {
		if (ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16.0f)) {
			f.label = f.small = f.tiny = font;
			break;
		}
	}
#elif defined(__APPLE__)
	static const char *const NAMES[] = {
		"Hiragino Sans",
		"Hiragino Kaku Gothic ProN",
		"Hiragino Kaku Gothic Pro",
		"Hiragino Sans GB",
		"Osaka",
	};
	for (const char *name : NAMES) {
		CFStringRef family = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
		if (!family)
			continue;
		const void *keys[]   = { kCTFontFamilyNameAttribute };
		const void *values[] = { family };
		CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 1,
		                                           &kCFTypeDictionaryKeyCallBacks,
		                                           &kCFTypeDictionaryValueCallBacks);
		CFRelease(family);
		if (!attrs)
			continue;
		CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
		CFRelease(attrs);
		if (!desc)
			continue;
		CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
		CFRelease(desc);
		std::string path;
		if (url) {
			char buf[1024] = {};
			if (CFURLGetFileSystemRepresentation(url, true, (UInt8 *)buf, sizeof(buf)))
				path = buf;
			CFRelease(url);
		}
		if (!path.empty()) {
			if (ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 16.0f)) {
				f.label = f.small = f.tiny = font;
				break;
			}
		}
	}
#else
	FcPattern *pat = FcPatternCreate();
	if (pat) {
		FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8 *>("Noto Sans CJK JP"));
		FcPatternAddDouble(pat, FC_SIZE, 16.0);
		FcConfigSubstitute(nullptr, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);
		FcResult res = FcResultNoMatch;
		FcPattern *m = FcFontMatch(nullptr, pat, &res);
		if (m) {
			FcChar8 *file = nullptr;
			if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file) {
				if (ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
				        reinterpret_cast<const char *>(file), 16.0f))
					f.label = f.small = f.tiny = font;
			}
			FcPatternDestroy(m);
		}
		FcPatternDestroy(pat);
	}
#endif
	if (!f.label)
		ImGui::GetIO().Fonts->AddFontDefault();   // the atlas needs a font
	return f;
}

// A context for one more panel window, current on return.
inline ImGuiContext *new_context()
{
	IMGUI_CHECKVERSION();
	ImGuiContext *ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;    // no imgui.ini beside the ROMs or project
	return ctx;
}

#ifdef _WIN32

// ---- Direct3D 11 family: main window, plug-in window, headless shot -------

struct dx11_state {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IDXGISwapChain *swap = nullptr;      // null for the headless shot
	ID3D11RenderTargetView *rtv = nullptr;
	ImGuiContext *imgui = nullptr;
	im::fonts fonts{};
	int resize_w = 0, resize_h = 0;
};

// A windowed device for hwnd, or a WARP device when hwnd is null (shot).
// The shot sets its own target; windows get theirs from the swap chain.
inline bool dx11_start(dx11_state &st, HWND hwnd)
{
	if (hwnd) {
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount       = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow      = hwnd;
		sd.SampleDesc.Count  = 1;
		sd.Windowed          = TRUE;
		sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		                                           levels, 2, D3D11_SDK_VERSION,
		                                           &sd, &st.swap, &st.dev, &got, &st.ctx);
		if (hr == DXGI_ERROR_UNSUPPORTED)    // no GPU: software rasterizer
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			                                   levels, 2, D3D11_SDK_VERSION,
			                                   &sd, &st.swap, &st.dev, &got, &st.ctx);
		if (FAILED(hr))
			return false;
		ID3D11Texture2D *back = nullptr;
		if (SUCCEEDED(st.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
			st.dev->CreateRenderTargetView(back, nullptr, &st.rtv);
			back->Release();
		}
	} else {
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		                             levels, 2, D3D11_SDK_VERSION, &st.dev, &got, &st.ctx)))
			return false;
	}

	st.imgui = new_context();
	st.fonts = panel_fonts();
	if (hwnd)
		ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(st.dev, st.ctx);
	return true;
}

inline void dx11_stop(dx11_state &st)
{
	if (st.imgui) {
		ImGui::SetCurrentContext(st.imgui);
		ImGui_ImplDX11_Shutdown();
		if (st.swap)
			ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(st.imgui);
		st.imgui = nullptr;
	}
	if (st.rtv) { st.rtv->Release(); st.rtv = nullptr; }
	if (st.swap) { st.swap->Release(); st.swap = nullptr; }
	if (st.ctx) { st.ctx->Release(); st.ctx = nullptr; }
	if (st.dev) { st.dev->Release(); st.dev = nullptr; }
	st.fonts = im::fonts{};
}

// Applies a pending resize, paints the caller's callback into the
// background list, submits. Presents windowed; headless leaves the
// caller-set target for readback.
inline void dx11_paint(dx11_state &st, int w, int h,
                       const std::function<void(ImDrawList *)> &paint)
{
	ImGui::SetCurrentContext(st.imgui);
	if (st.swap && st.resize_w) {
		if (st.rtv) { st.rtv->Release(); st.rtv = nullptr; }
		st.swap->ResizeBuffers(0, st.resize_w, st.resize_h, DXGI_FORMAT_UNKNOWN, 0);
		st.resize_w = st.resize_h = 0;
		ID3D11Texture2D *back = nullptr;
		if (SUCCEEDED(st.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
			st.dev->CreateRenderTargetView(back, nullptr, &st.rtv);
			back->Release();
		}
	}
	ImGui_ImplDX11_NewFrame();
	if (st.swap)
		ImGui_ImplWin32_NewFrame();
	else
		ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
	ImGui::NewFrame();
	paint(ImGui::GetBackgroundDrawList());
	ImGui::Render();
	const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	st.ctx->OMSetRenderTargets(1, &st.rtv, nullptr);
	st.ctx->ClearRenderTargetView(st.rtv, clear);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	if (st.swap)
		st.swap->Present(0, 0);   // no wait: the 30 Hz timers decide the pace
}

#endif // defined(_WIN32)

#ifdef __OBJC__

// ---- Metal family: main view, plug-in view (.mm only) ----------------------
//
// The hosted layer (not +layerClass: AppKit's backing layer does not always
// honour the override) with its device and queue. Returns the layer, or nil.
inline CAMetalLayer *metal_attach(NSView *view, id<MTLDevice> __strong &dev,
                                  id<MTLCommandQueue> __strong &queue)
{
	dev = MTLCreateSystemDefaultDevice();
	if (!dev)
		return nil;
	queue = [dev newCommandQueue];
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = dev;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	[view setLayer:layer];
	[view setWantsLayer:YES];
	return layer;
}

inline void metal_sync(CAMetalLayer *layer, NSView *view)
{
	if (!layer)
		return;
	const NSRect b = [view bounds];
	const CGFloat s = [view.window backingScaleFactor];
	layer.drawableSize = CGSizeMake(b.size.width * s, b.size.height * s);
}

// One frame at the view's size: the caller's paint into the background
// list, then submit + present. Quietly skips when no drawable is ready.
inline void metal_paint(ImGuiContext *ctx, CAMetalLayer *layer,
                        id<MTLCommandQueue> queue, float w, float h, float scale,
                        const std::function<void(ImDrawList *)> &paint)
{
	if (!ctx || !layer)
		return;
	ImGui::SetCurrentContext(ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(w, h);
	io.DisplayFramebufferScale = ImVec2(scale, scale);
	id<CAMetalDrawable> drawable = [layer nextDrawable];
	if (!drawable)
		return;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	ImGui_ImplMetal_NewFrame(pass);
	ImGui::NewFrame();
	paint(ImGui::GetBackgroundDrawList());
	ImGui::Render();
	id<MTLCommandBuffer> buf = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf presentDrawable:drawable];
	[buf commit];
}

inline void metal_stop(ImGuiContext *&ctx)
{
	if (!ctx)
		return;
	ImGui::SetCurrentContext(ctx);
	ImGui_ImplMetal_Shutdown();
	ImGui::DestroyContext(ctx);
	ctx = nullptr;
}

#endif // defined(__OBJC__)

} // namespace imshell
} // namespace ui

#endif // S_MU2000_UI_IMGUI_SHELL_H
