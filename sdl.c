/*
 * SDL display driver
 * 
 * Copyright (c) 2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

#include <SDL2/SDL.h>

#include "sdl2-keymap.h"
#include "cutils.h"
#include "virtio.h"
#include "machine.h"

#define AMPLITUDE 8000
#define SAMPLERATE 44100

static SDL_Surface *fb_surface;
static int screen_width, screen_height, fb_width, fb_height, fb_stride;
static SDL_Cursor *sdl_cursor_hidden;
static uint8_t key_pressed[NR_KEYS];

static SDL_Window *window;
static SDL_Texture *texture;
static SDL_Renderer *renderer;


static void sdl_update_fb_surface(FBDevice *fb_dev)
{
    if (!fb_surface)
        goto force_alloc;
    if (fb_width != fb_dev->width ||
        fb_height != fb_dev->height ||
        fb_stride != fb_dev->stride) {
    force_alloc:
        if (fb_surface != NULL)
            SDL_FreeSurface(fb_surface);
        fb_width = fb_dev->width;
        fb_height = fb_dev->height;
        fb_stride = fb_dev->stride;
        fb_surface = SDL_CreateRGBSurfaceFrom(fb_dev->fb_data,
                                              fb_dev->width, fb_dev->height,
                                              32, fb_dev->stride,
                                              0x00ff0000,
                                              0x0000ff00,
                                              0x000000ff,
                                              0x00000000);
        if (!fb_surface) {
            fprintf(stderr, "Could not create SDL framebuffer surface\n");
            exit(1);
        }

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer) {
		fprintf(stderr, "Could not create renderer - exiting\n");
		exit(1);
	}

	texture = SDL_CreateTextureFromSurface(renderer, fb_surface);
	if (!texture) {
		fprintf(stderr, "Could not create texture - exiting\n");
		exit(1);
	}

    }
}

static void sdl_update(FBDevice *fb_dev, void *opaque,
                       int x, int y, int w, int h)
{
    unsigned char *start;
    SDL_Rect r;
    //    printf("sdl_update: %d %d %d %d\n", x, y, w, h);
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    start = fb_surface->pixels + fb_surface->pitch * y + x;

    SDL_UpdateTexture(texture, &r, start, fb_surface->pitch);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

#if defined(_WIN32)

static int sdl_get_keycode(const SDL_KeyboardEvent *ev)
{
    return ev->keysym.scancode;
}

#else

/* we assume Xorg is used with a PC keyboard. Return 0 if no keycode found. */
static int sdl_get_keycode(const SDL_KeyboardEvent *ev)
{
    int scancode = ev->keysym.scancode;
    if (scancode < NR_KEYS)
	    return keymap[scancode];

    return 0;
}

#endif

/* release all pressed keys */
static void sdl_reset_keys(VirtMachine *m)
{
    int i;
    
    for(i = 1; i < NR_KEYS; i++) {
        if (key_pressed[i]) {
            vm_send_key_event(m, FALSE, i);
            key_pressed[i] = FALSE;
        }
    }
}

static void sdl_handle_key_event(const SDL_KeyboardEvent *ev, VirtMachine *m)
{
    int keycode, keypress;

    keycode = sdl_get_keycode(ev);
    if (keycode) {
        if (keycode == 0x3a || keycode ==0x45) {
            /* SDL does not generate key up for numlock & caps lock */
            vm_send_key_event(m, TRUE, keycode);
            vm_send_key_event(m, FALSE, keycode);
        } else {
            keypress = (ev->type == SDL_KEYDOWN);
            if (keycode < NR_KEYS)
                key_pressed[keycode] = keypress;
            vm_send_key_event(m, keypress, keycode);
        }
    } else if (ev->type == SDL_KEYUP) {
        /* workaround to reset the keyboard state (used when changing
           desktop with ctrl-alt-x on Linux) */
        sdl_reset_keys(m);
    }
}

static void sdl_send_mouse_event(VirtMachine *m, int x1, int y1,
                                 int dz, int state, BOOL is_absolute)
{
    int buttons, x, y;

    buttons = 0;
    if (state & SDL_BUTTON(SDL_BUTTON_LEFT))
        buttons |= (1 << 0);
    if (state & SDL_BUTTON(SDL_BUTTON_RIGHT))
        buttons |= (1 << 1);
    if (state & SDL_BUTTON(SDL_BUTTON_MIDDLE))
        buttons |= (1 << 2);
    if (is_absolute) {
        x = (x1 * 32768) / screen_width;
        y = (y1 * 32768) / screen_height;
    } else {
        x = x1;
        y = y1;
    }
    vm_send_mouse_event(m, x, y, dz, buttons);
}

static void sdl_handle_mouse_motion_event(const SDL_Event *ev, VirtMachine *m)
{
    BOOL is_absolute = vm_mouse_is_absolute(m);
    int x, y;
    if (is_absolute) {
        x = ev->motion.x;
        y = ev->motion.y;
    } else {
        x = ev->motion.xrel;
        y = ev->motion.yrel;
    }
    sdl_send_mouse_event(m, x, y, 0, ev->motion.state, is_absolute);
}

void sdl_refresh(VirtMachine *m)
{
    SDL_Event ev_s, *ev = &ev_s;

    if (!m->fb_dev)
        return;
    
    sdl_update_fb_surface(m->fb_dev);

    m->fb_dev->refresh(m->fb_dev, sdl_update, NULL);
    
    while (SDL_PollEvent(ev)) {
        switch (ev->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            sdl_handle_key_event(&ev->key, m);
            break;
        case SDL_MOUSEMOTION:
            sdl_handle_mouse_motion_event(ev, m);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            break;
        case SDL_QUIT:
            exit(0);
        }
    }
}

static void sdl_hide_cursor(void)
{
    uint8_t data = 0;
    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    SDL_ShowCursor(1);
    SDL_SetCursor(sdl_cursor_hidden);
}

static int beepfreq;

void beep(int freq)
{
	SDL_LockAudio();
	beepfreq = freq;
	SDL_UnlockAudio();
}

static SDL_AudioSpec have;

void audio_callback(void *_beeper, uint8_t *_stream, int _length)
{
    static double v;
    Sint16 *stream = (void*) _stream;
    int length = _length / sizeof(*stream);

    for (int i = 0; i < length; i++) {
	    stream[i] = AMPLITUDE * sin(v * 2 * M_PI / have.freq);
	    v += beepfreq;
    }
}

static SDL_AudioDeviceID dev;

int sdl_sound_init(unsigned sample_rate)
{
	SDL_AudioSpec audiospec = {
		.freq = sample_rate,
		.format = AUDIO_S16,
		.channels = 1,
		.samples = 4096,
		.callback = audio_callback,
	};

	dev = SDL_OpenAudioDevice(NULL, 0, &audiospec, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!dev) {
		fprintf(stderr, "initialize open audio device\n");
		exit(1);
	}

	SDL_PauseAudioDevice(dev, 0);
	return 0;
}

void sdl_init(int width, int height)
{
    screen_width = width;
    screen_height = height;

    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE)) {
        fprintf(stderr, "Could not initialize SDL - exiting\n");
        exit(1);
    }

    window = SDL_CreateWindow("TinyEMU", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			      width, height, 0);
    if (!window) {
	    fprintf(stderr, "Could not open SDL display\n");
	    exit(1);
    }

    sdl_hide_cursor();

    sdl_sound_init(SAMPLERATE);
}

