/* A window to play in: the PSP's screen at any size, the keyboard or a gamepad as its pad.
 * Included by psp_runtime.c when built with -DPSP_WINDOW. Game time follows
 * real time, shown a frame at each flip. */
#include <SDL.h>
#if defined(PSP_METAL) || defined(PSP_GL)
#define PSP_GPU             /* drawn on the GPU through psp_metal.h, by Metal or OpenGL */
#endif

void psp_ge_rgba(u32 addr, int stride, int fmt, u32 *out);

static SDL_Window *win;
#ifdef PSP_GPU
#include "psp_metal.h"
#else
static SDL_Renderer *ren;
static SDL_Texture *tex;
#endif
static u32 pad_buttons, tapped;
static u8 pad_x = 128, pad_y = 128;
static SDL_GameController *gamepad;
static u64 real_start;
static int window_scale = 2;

static const struct { SDL_Scancode key; u32 button; } keys[] = {
    { SDL_SCANCODE_UP, 0x0010 }, { SDL_SCANCODE_RIGHT, 0x0020 }, { SDL_SCANCODE_DOWN, 0x0040 }, { SDL_SCANCODE_LEFT, 0x0080 },
    { SDL_SCANCODE_Z, 0x4000 }, { SDL_SCANCODE_X, 0x2000 }, { SDL_SCANCODE_A, 0x8000 }, { SDL_SCANCODE_S, 0x1000 },
    { SDL_SCANCODE_Q, 0x0100 }, { SDL_SCANCODE_W, 0x0200 }, { SDL_SCANCODE_RETURN, 0x0008 }, { SDL_SCANCODE_TAB, 0x0001 },
};

static const struct { SDL_GameControllerButton pad; u32 button; } pad_keys[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0010 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0020 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0040 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0080 },
    { SDL_CONTROLLER_BUTTON_A, 0x4000 }, { SDL_CONTROLLER_BUTTON_B, 0x2000 }, { SDL_CONTROLLER_BUTTON_X, 0x8000 }, { SDL_CONTROLLER_BUTTON_Y, 0x1000 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200 }, { SDL_CONTROLLER_BUTTON_START, 0x0008 }, { SDL_CONTROLLER_BUTTON_BACK, 0x0001 },
};

static u8 stick(SDL_GameControllerAxis axis) {
    const int v = SDL_GameControllerGetAxis(gamepad, axis);
    return abs(v) < 6000 ? 128 : (u8)((v + 32768) >> 8);
}

static void window_sound(void *unused, Uint8 *out, int bytes) { audio_drain((s16 *)out, bytes / 4); }

static void window_open(const char *title) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO);
    SDL_AudioSpec want = { .freq = 44100, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024, .callback = window_sound };
    const SDL_AudioDeviceID speakers = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (speakers) SDL_PauseAudioDevice(speakers, 0);
#ifdef PSP_GPU
    int w = 480 * window_scale, h = 272 * window_scale;
    if (getenv("PSP_SIZE")) sscanf(getenv("PSP_SIZE"), "%dx%d", &w, &h);
#ifdef PSP_GL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    const Uint32 api = SDL_WINDOW_OPENGL;
#else
    const Uint32 api = SDL_WINDOW_METAL;
#endif
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | api);
    SDL_SetWindowMinimumSize(win, 240, 136);
    if (getenv("PSP_FULLSCREEN")) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    mtl_open(win, getenv("PSP_SCALE") ? atoi(getenv("PSP_SCALE")) : 4, getenv("PSP_MSAA") ? atoi(getenv("PSP_MSAA")) : 4);
#else
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480 * window_scale, 272 * window_scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, 480, 272);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
#endif
    SDL_RaiseWindow(win);
    real_start = SDL_GetTicks64();
}

static void window_flip(u32 addr, int stride, int fmt) {
    static u32 px[480 * 272];
#ifdef PSP_GPU
    const u32 at = addr & 0x0FFFFFFF;
    if (!mtl_drawn(at)) psp_ge_rgba(addr, stride, fmt, px);
    mtl_present(at, mtl_drawn(at) ? NULL : px);
#else
    psp_ge_rgba(addr, stride, fmt, px);
    SDL_UpdateTexture(tex, NULL, px, 480 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
#endif
    SDL_Event e;
    pad_buttons = tapped;       /* a press between two flips still counts, for one */
    tapped = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) stop_reason = "the window closed";
        else if (e.type == SDL_CONTROLLERDEVICEADDED && !gamepad) { gamepad = SDL_GameControllerOpen(e.cdevice.which); fprintf(stderr, "gamepad: %s\n", SDL_GameControllerName(gamepad)); }
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED && gamepad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))) { SDL_GameControllerClose(gamepad); gamepad = NULL; pad_x = pad_y = 128; }
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f && (e.key.keysym.mod & KMOD_GUI)) SDL_SetWindowFullscreen(win, SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        else if (e.type == SDL_KEYDOWN) { for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (e.key.keysym.scancode == keys[i].key) tapped |= keys[i].button; }
        else if (e.type == SDL_CONTROLLERBUTTONDOWN) { for (size_t i = 0; i < sizeof pad_keys / sizeof *pad_keys; i++) if (e.cbutton.button == pad_keys[i].pad) tapped |= pad_keys[i].button; }
    }
    pad_buttons |= tapped;
    const Uint8 *held = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (held[keys[i].key]) pad_buttons |= keys[i].button;
    if (gamepad) {
        for (size_t i = 0; i < sizeof pad_keys / sizeof *pad_keys; i++) if (SDL_GameControllerGetButton(gamepad, pad_keys[i].pad)) pad_buttons |= pad_keys[i].button;
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) pad_buttons |= 0x0100;
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) pad_buttons |= 0x0200;
        pad_x = stick(SDL_CONTROLLER_AXIS_LEFTX);
        pad_y = stick(SDL_CONTROLLER_AXIS_LEFTY);
    }
    const u64 real = SDL_GetTicks64() - real_start, game = now_us / 1000;
    if (game > real) SDL_Delay((Uint32)(game - real));
    else now_us = real * 1000;        /* time lost to the display is the game's time too, or its sound falls behind */
}
