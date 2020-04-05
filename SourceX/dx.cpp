#include <algorithm>

#include "all.h"
#include "../3rdParty/Storm/Source/storm.h"
#include "display.h"
#include <SDL.h>

namespace dvl {

int sgdwLockCount;
BYTE *gpBuffer;
#ifdef _DEBUG
int locktbl[256];
#endif
static CCritSect sgMemCrit;
HMODULE ghDiabMod;

int refreshDelay;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;

/** Currently active palette */
SDL_Palette *palette;
unsigned int pal_surface_palette_version = 0;

/** 24-bit renderer texture surface */
SDL_Surface *renderer_texture_surface = nullptr;

/** 8-bit surface wrapper around #gpBuffer */
SDL_Surface *pal_surface;

static void dx_create_back_buffer()
{
	pal_surface = SDL_CreateRGBSurfaceWithFormat(0, BUFFER_WIDTH, BUFFER_HEIGHT, 8, SDL_PIXELFORMAT_INDEX8);
	if (pal_surface == NULL) {
		ErrSdl();
	}

	gpBuffer = (BYTE *)pal_surface->pixels;

#ifndef USE_SDL1
	// In SDL2, `pal_surface` points to the global `palette`.
	if (SDL_SetSurfacePalette(pal_surface, palette) < 0)
		ErrSdl();
#else
	// In SDL1, `pal_surface` owns its palette and we must update it every
	// time the global `palette` is changed. No need to do anything here as
	// the global `palette` doesn't have any colors set yet.
#endif

	pal_surface_palette_version = 1;
}

static void dx_create_primary_surface()
{
#ifndef USE_SDL1
	if (renderer) {
		int width, height;
		SDL_RenderGetLogicalSize(renderer, &width, &height);
		Uint32 format;
		if (SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) < 0)
			ErrSdl();
		renderer_texture_surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
	}
#endif
	if (GetOutputSurface() == nullptr) {
		ErrSdl();
	}
}

void dx_init(HWND hWnd)
{
	SDL_RaiseWindow(window);
	SDL_ShowWindow(window);

	dx_create_primary_surface();
	palette_init();
	dx_create_back_buffer();
}
static void lock_buf_priv()
{
	sgMemCrit.Enter();
	if (sgdwLockCount != 0) {
		sgdwLockCount++;
		return;
	}

	gpBufEnd += (uintptr_t)(BYTE *)pal_surface->pixels;
	gpBuffer = (BYTE *)pal_surface->pixels;
	sgdwLockCount++;
}

void lock_buf(BYTE idx)
{
#ifdef _DEBUG
	locktbl[idx]++;
#endif
	lock_buf_priv();
}

static void unlock_buf_priv()
{
	if (sgdwLockCount == 0)
		app_fatal("draw main unlock error");
	if (!gpBuffer)
		app_fatal("draw consistency error");

	sgdwLockCount--;
	if (sgdwLockCount == 0) {
		gpBufEnd -= (uintptr_t)gpBuffer;
		//gpBuffer = NULL; unable to return to menu
	}
	sgMemCrit.Leave();
}

void unlock_buf(BYTE idx)
{
#ifdef _DEBUG
	if (!locktbl[idx])
		app_fatal("Draw lock underflow: 0x%x", idx);
	locktbl[idx]--;
#endif
	unlock_buf_priv();
}

void dx_cleanup()
{
	if (ghMainWnd)
		SDL_HideWindow(window);
	sgMemCrit.Enter();
	sgdwLockCount = 0;
	gpBuffer = NULL;
	sgMemCrit.Leave();

	if (pal_surface == nullptr)
		return;
	SDL_FreeSurface(pal_surface);
	pal_surface = nullptr;
	SDL_FreePalette(palette);
	SDL_FreeSurface(renderer_texture_surface);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}

void dx_reinit()
{
#ifdef USE_SDL1
	window = SDL_SetVideoMode(0, 0, 0, window->flags ^ SDL_FULLSCREEN);
	if (window == NULL) {
		ErrSdl();
	}
#else
	Uint32 flags = 0;
	if (!fullscreen) {
		flags = renderer ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
	}
	if (SDL_SetWindowFullscreen(window, flags)) {
		ErrSdl();
	}
#endif
	fullscreen = !fullscreen;
	force_redraw = 255;
}

void InitPalette()
{
	palette = SDL_AllocPalette(256);
	if (palette == NULL) {
		ErrSdl();
	}
}

void BltFast(SDL_Rect *src_rect, SDL_Rect *dst_rect)
{
	Blit(pal_surface, src_rect, dst_rect);
}

void Blit(SDL_Surface *src, SDL_Rect *src_rect, SDL_Rect *dst_rect)
{
	SDL_Surface *dst = GetOutputSurface();
#ifndef USE_SDL1
	if (SDL_BlitSurface(src, src_rect, dst, dst_rect) < 0)
			ErrSdl();
		return;
#else
	if (!OutputRequiresScaling()) {
		if (SDL_BlitSurface(src, src_rect, dst, dst_rect) < 0)
			ErrSdl();
		return;
	}

	SDL_Rect scaled_dst_rect;
	if (dst_rect != nullptr) {
		scaled_dst_rect = *dst_rect;
		ScaleOutputRect(&scaled_dst_rect);
		dst_rect = &scaled_dst_rect;
	}

	// Same pixel format: We can call BlitScaled directly.
	if (SDLBackport_PixelFormatFormatEq(src->format, dst->format)) {
		if (SDL_BlitScaled(src, src_rect, dst, dst_rect) < 0)
			ErrSdl();
		return;
	}

	// If the surface has a color key, we must stretch first and can then call BlitSurface.
	if (SDL_HasColorKey(src)) {
		SDL_Surface *stretched = SDL_CreateRGBSurface(SDL_SWSURFACE, dst_rect->w, dst_rect->h, src->format->BitsPerPixel,
		    src->format->Rmask, src->format->Gmask, src->format->BitsPerPixel, src->format->Amask);
		SDL_SetColorKey(stretched, SDL_SRCCOLORKEY, src->format->colorkey);
		if (src->format->palette != nullptr)
			SDL_SetPalette(stretched, SDL_LOGPAL, src->format->palette->colors, 0, src->format->palette->ncolors);
		SDL_Rect stretched_rect = { 0, 0, dst_rect->w, dst_rect->h };
		if (SDL_SoftStretch(src, src_rect, stretched, &stretched_rect) < 0
		    || SDL_BlitSurface(stretched, &stretched_rect, dst, dst_rect) < 0) {
			SDL_FreeSurface(stretched);
			ErrSdl();
		}
		SDL_FreeSurface(stretched);
		return;
	}

	// A surface with a non-output pixel format but without a color key needs scaling.
	// We can convert the format and then call BlitScaled.
	SDL_Surface *converted = SDL_ConvertSurface(src, dst->format, 0);
	if (SDL_BlitScaled(converted, src_rect, dst, dst_rect) < 0) {
		SDL_FreeSurface(converted);
		ErrSdl();
	}
	SDL_FreeSurface(converted);
#endif
}

/**
 * @brief Limit FPS to avoid high CPU load, use when v-sync isn't available
 */
void LimitFrameRate()
{
	static uint32_t frameDeadline;
	uint32_t tc = SDL_GetTicks() * 1000;
	uint32_t v = 0;
	if (frameDeadline > tc) {
		v = tc % refreshDelay;
		SDL_Delay(v / 1000 + 1); // ceil
	}
	frameDeadline = tc + v + refreshDelay;
}

void RenderPresent()
{
	SDL_Surface *surface = GetOutputSurface();
	assert(!SDL_MUSTLOCK(surface));

	if (!gbActive) {
		LimitFrameRate();
		return;
	}

#ifndef USE_SDL1
	if (renderer) {
		if (SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch) <= -1) { //pitch is 2560
			// We should supposedly handle SDL_RENDER_DEVICE_RESET, but I tried and it doesn't arrive
			if (!strcmp(SDL_GetError(), "LockRect(): INVALIDCALL")) {
				SDL_DestroyTexture(texture);
				SDL_DestroyRenderer(renderer);

				renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
				if (renderer == NULL) {
					ErrSdl();
				}

				texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
				if (texture == NULL) {
					ErrSdl();
				}

				if (SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT) <= -1) {
					ErrSdl();
				}

				if (SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch) <= -1) { //pitch is 2560
					ErrSdl();
				}
			} else {
				ErrSdl();
			}
		}

		// Clear buffer to avoid artifacts in case the window was resized
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) { // TODO only do this if window was resized
			ErrSdl();
		}

		if (SDL_RenderClear(renderer) <= -1) {
			ErrSdl();
		}

		if (SDL_RenderCopy(renderer, texture, NULL, NULL) <= -1) {
			ErrSdl();
		}
		SDL_RenderPresent(renderer);
	} else {
		if (SDL_UpdateWindowSurface(window) <= -1) {
			ErrSdl();
		}
		LimitFrameRate();
	}
#else
	if (SDL_Flip(surface) <= -1) {
		ErrSdl();
	}
	LimitFrameRate();
#endif
}

void PaletteGetEntries(DWORD dwNumEntries, SDL_Color *lpEntries)
{
	for (DWORD i = 0; i < dwNumEntries; i++) {
		lpEntries[i] = system_palette[i];
	}
}
} // namespace dvl
