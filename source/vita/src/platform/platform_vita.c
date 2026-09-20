#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <vitaGL.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "pad_bits.h"
#include "gamepad_buttons.h"

#define VITA_SCREEN_W        960
#define VITA_SCREEN_H        544
#define VITA_DATA_DIR        "ux0:data/sonicr/"
#define VITA_BUNDLED_DIR     "app0:DATA"
#define VITA_INSTALL_DIR     "ux0:data/sonicr"
#define VITA_NET_MEM_SIZE    (1024 * 1024)

#define MAX_GAMEPADS         4
#define MAX_CTRL_PORTS       5
#define JOY_BUTTONS_PER_SLOT 80
#define JOY_CFG_MAX          32
#define STICK_THRESHOLD      77
#define STICK_CENTER         128
#define TRIGGER_THRESHOLD    64
#define RESCAN_INTERVAL      120

int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

unsigned char s_keystate[256];

extern unsigned char g_keyPressState[320];
extern short g_joystickConfigWords[];
extern char g_joystickSlots[4][282];
extern char g_joystickDeviceNames[4][260];
extern short g_joystickDeviceFlags[8];
extern int g_initFeatureC;
extern void SyncJoystickSlots(void);

typedef struct {
    int port;
    int type;
} PadSlot;

static PadSlot s_pads[MAX_GAMEPADS];
static int s_padCount = 0;
static int s_rescanCounter = 0;
static int s_quitRequested = 0;
static int s_netInitialized = 0;
static char s_netMem[VITA_NET_MEM_SIZE] __attribute__((aligned(16)));

static const char *pad_type_name(int type)
{
    switch (type) {
        case SCE_CTRL_TYPE_DS4:  return "DualShock 4";
        case SCE_CTRL_TYPE_DS3:  return "DualShock 3";
        case SCE_CTRL_TYPE_VIRT: return "PS TV Controller";
        default:                 return "PS Vita";
    }
}

static void publish_pad_name(int slot, const char *name)
{
    char *dst = g_joystickDeviceNames[slot];
    size_t n = strlen(name);
    if (n > 258) {
        n = 258;
    }
    memcpy(dst, name, n);
    dst[n] = '\0';
}

static int enumerate_pads(PadSlot *out)
{
    SceCtrlPortInfo info;
    int count = 0;

    memset(&info, 0, sizeof(info));
    if (sceCtrlGetControllerPortInfo(&info) < 0) {
        memset(&info, 0, sizeof(info));
    }

    out[count].port = 0;
    out[count].type = info.port[0] ? info.port[0] : SCE_CTRL_TYPE_PHY;
    count++;

    for (int p = 1; p < MAX_CTRL_PORTS && count < MAX_GAMEPADS; p++) {
        if (info.port[p] != SCE_CTRL_TYPE_UNPAIRED) {
            out[count].port = p;
            out[count].type = info.port[p];
            count++;
        }
    }
    return count;
}

static int pads_changed(const PadSlot *fresh, int freshCount)
{
    if (freshCount != s_padCount) {
        return 1;
    }
    for (int i = 0; i < freshCount; i++) {
        if (fresh[i].port != s_pads[i].port || fresh[i].type != s_pads[i].type) {
            return 1;
        }
    }
    return 0;
}

static void apply_pads(const PadSlot *fresh, int freshCount)
{
    for (int i = 0; i < freshCount; i++) {
        s_pads[i] = fresh[i];
        publish_pad_name(i, pad_type_name(fresh[i].type));
        g_joystickDeviceFlags[i] = (short)(GC_BUTTON_COUNT < JOY_SLOT_CFG_WORDS
                                           ? GC_BUTTON_COUNT : JOY_SLOT_CFG_WORDS);
    }
    for (int i = freshCount; i < MAX_GAMEPADS; i++) {
        s_pads[i].port = -1;
        s_pads[i].type = 0;
        memset(&g_keyPressState[i * JOY_BUTTONS_PER_SLOT], 0, JOY_BUTTONS_PER_SLOT);
    }
    s_padCount = freshCount;
    g_initFeatureC = s_padCount;
}

static void rescan_pads(void)
{
    PadSlot fresh[MAX_GAMEPADS];
    int freshCount = enumerate_pads(fresh);
    if (pads_changed(fresh, freshCount)) {
        apply_pads(fresh, freshCount);
        SyncJoystickSlots();
    }
}

static void read_pad(int port, SceCtrlData *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->lx = STICK_CENTER;
    pad->ly = STICK_CENTER;
    sceCtrlPeekBufferPositiveExt2(port, pad, 1);
}

static int stick_left(const SceCtrlData *d)  { return d->lx < STICK_CENTER - STICK_THRESHOLD; }
static int stick_right(const SceCtrlData *d) { return d->lx > STICK_CENTER + STICK_THRESHOLD; }
static int stick_up(const SceCtrlData *d)    { return d->ly < STICK_CENTER - STICK_THRESHOLD; }
static int stick_down(const SceCtrlData *d)  { return d->ly > STICK_CENTER + STICK_THRESHOLD; }

static int pad_button_held(const SceCtrlData *d, int b)
{
    unsigned int bt = d->buttons;

    switch (b) {
        case GCBTN_A:             return (bt & SCE_CTRL_CROSS) != 0;
        case GCBTN_B:             return (bt & SCE_CTRL_CIRCLE) != 0;
        case GCBTN_X:             return (bt & SCE_CTRL_SQUARE) != 0;
        case GCBTN_Y:             return (bt & SCE_CTRL_TRIANGLE) != 0;
        case GCBTN_BACK:          return (bt & SCE_CTRL_SELECT) != 0;
        case GCBTN_GUIDE:         return 0;
        case GCBTN_START:         return (bt & SCE_CTRL_START) != 0;
        case GCBTN_LEFTSTICK:     return (bt & SCE_CTRL_L3) != 0;
        case GCBTN_RIGHTSTICK:    return (bt & SCE_CTRL_R3) != 0;
        case GCBTN_LEFTSHOULDER:  return (bt & SCE_CTRL_L1) != 0;
        case GCBTN_RIGHTSHOULDER: return (bt & SCE_CTRL_R1) != 0;
        case GCBTN_DPAD_UP:       return (bt & SCE_CTRL_UP) != 0;
        case GCBTN_DPAD_DOWN:     return (bt & SCE_CTRL_DOWN) != 0;
        case GCBTN_DPAD_LEFT:     return (bt & SCE_CTRL_LEFT) != 0;
        case GCBTN_DPAD_RIGHT:    return (bt & SCE_CTRL_RIGHT) != 0;
        case GCBTN_TRIGGER_LEFT:  return (bt & SCE_CTRL_L2) != 0 || d->lt > TRIGGER_THRESHOLD;
        case GCBTN_TRIGGER_RIGHT: return (bt & SCE_CTRL_R2) != 0 || d->rt > TRIGGER_THRESHOLD;
        default:                  return 0;
    }
}

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)width;
    (void)height;
    (void)fullscreen;
    (void)title;

    memset(s_keystate, 0, sizeof(s_keystate));
    s_quitRequested = 0;

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        fprintf(stderr, "Mix_OpenAudio failed: %s\n", Mix_GetError());
        return -1;
    }

    vglUseTripleBuffering(GL_FALSE);
    vglInitExtended(0, VITA_SCREEN_W, VITA_SCREEN_H, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);

    return 0;
}

void platform_shutdown(void)
{
    Mix_CloseAudio();
    SDL_Quit();
    platform_net_shutdown();
}

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return;
    }

    static unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            break;
        }
    }
    fclose(out);
    fclose(in);
}

static void install_dir(const char *src, const char *dst)
{
    sceIoMkdir(dst, 0777);

    SceUID dir = sceIoDopen(src);
    if (dir < 0) {
        return;
    }

    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dir, &ent) > 0) {
        char srcPath[256];
        char dstPath[256];
        snprintf(srcPath, sizeof(srcPath), "%s/%s", src, ent.d_name);
        snprintf(dstPath, sizeof(dstPath), "%s/%s", dst, ent.d_name);

        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            install_dir(srcPath, dstPath);
        }
        else {
            FILE *existing = fopen(dstPath, "rb");
            if (existing != NULL) {
                fclose(existing);
            }
            else {
                copy_file(srcPath, dstPath);
            }
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dir);
}

static void install_bundled_data(void)
{
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;

    sceIoMkdir("ux0:data", 0777);
    install_dir(VITA_BUNDLED_DIR, VITA_INSTALL_DIR);
}

const char *platform_base_path(void)
{
    install_bundled_data();
    return VITA_DATA_DIR;
}

static void maybe_rescan(void)
{
    if (++s_rescanCounter >= RESCAN_INTERVAL) {
        s_rescanCounter = 0;
        rescan_pads();
    }
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    maybe_rescan();

    int copySize = keystateSize < 256 ? keystateSize : 256;
    memcpy(keystateOut, s_keystate, (size_t)copySize);

    return s_quitRequested;
}

void platform_pump_events(void)
{
    maybe_rescan();
}

int platform_init_gamepads(void)
{
    PadSlot fresh[MAX_GAMEPADS];
    int freshCount = enumerate_pads(fresh);
    apply_pads(fresh, freshCount);
    return s_padCount;
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    int count = s_padCount;
    if (count > maxSlots) {
        count = maxSlots;
    }

    for (int i = 0; i < count; i++) {
        unsigned char *pressBase = &g_keyPressState[i * JOY_BUTTONS_PER_SLOT];
        SceCtrlData pad;
        unsigned short bits = 0;

        read_pad(s_pads[i].port, &pad);

        if (stick_left(&pad)) {
            bits |= PAD_LEFT;
        }
        if (stick_right(&pad)) {
            bits |= PAD_RIGHT;
        }
        if (stick_up(&pad)) {
            bits |= PAD_UP;
        }
        if (stick_down(&pad)) {
            bits |= PAD_DOWN;
        }

        const short *slotCfg = (const short *)&g_joystickSlots[i][0x104];
        for (int b = 0; b < GC_BUTTON_COUNT; b++) {
            int held = pad_button_held(&pad, b);
            pressBase[b] = held ? 0x80 : 0x00;
            if (!held) {
                continue;
            }
            short cfg;
            if (b < JOY_SLOT_CFG_WORDS) {
                cfg = slotCfg[b];
            } else if (b < JOY_CFG_MAX) {
                cfg = g_joystickConfigWords[b];
            } else {
                cfg = 0;
            }
            bits |= (unsigned short)cfg;
        }
        for (int b = GC_BUTTON_COUNT; b < JOY_BUTTONS_PER_SLOT; b++) {
            pressBase[b] = 0x00;
        }

        joySlotState[i] = bits;
    }

    for (int i = count; i < maxSlots; i++) {
        joySlotState[i] = 0;
    }
    for (int i = count; i < MAX_GAMEPADS; i++) {
        memset(&g_keyPressState[i * JOY_BUTTONS_PER_SLOT], 0, JOY_BUTTONS_PER_SLOT);
    }

    return count;
}

unsigned int platform_menu_buttons(void)
{
    unsigned int out = 0;

    for (int i = 0; i < s_padCount; i++) {
        SceCtrlData pad;
        unsigned int bt;

        read_pad(s_pads[i].port, &pad);
        bt = pad.buttons;

        if (bt & SCE_CTRL_CROSS)    out |= MENUBTN_A;
        if (bt & SCE_CTRL_CIRCLE)   out |= MENUBTN_B;
        if (bt & SCE_CTRL_SQUARE)   out |= MENUBTN_X;
        if (bt & SCE_CTRL_TRIANGLE) out |= MENUBTN_Y;
        if (bt & SCE_CTRL_START)    out |= MENUBTN_START;
        if ((bt & SCE_CTRL_L1) || (bt & SCE_CTRL_L2) || pad.lt > TRIGGER_THRESHOLD) {
            out |= MENUBTN_L;
        }
        if ((bt & SCE_CTRL_R1) || (bt & SCE_CTRL_R2) || pad.rt > TRIGGER_THRESHOLD) {
            out |= MENUBTN_R;
        }
        if ((bt & SCE_CTRL_UP) || stick_up(&pad))       out |= MENUBTN_UP;
        if ((bt & SCE_CTRL_DOWN) || stick_down(&pad))   out |= MENUBTN_DOWN;
        if ((bt & SCE_CTRL_LEFT) || stick_left(&pad))   out |= MENUBTN_LEFT;
        if ((bt & SCE_CTRL_RIGHT) || stick_right(&pad)) out |= MENUBTN_RIGHT;
    }

    return out;
}

uint32_t platform_get_time_ms(void)
{
    return (uint32_t)(sceKernelGetProcessTimeWide() / 1000ULL);
}

void platform_sleep_ms(int ms)
{
    if (ms > 0) {
        sceKernelDelayThread((SceUInt32)ms * 1000u);
    }
}

int platform_audio_init(void)
{
    return 0;
}

void platform_audio_shutdown(void)
{
}

void platform_gl_swap(void)
{
    vglSwapBuffers(GL_FALSE);
}

void platform_get_drawable_size(int *w, int *h)
{
    *w = VITA_SCREEN_W;
    *h = VITA_SCREEN_H;
}

int platform_net_init(void)
{
    int state = 0;

    if (!s_netInitialized) {
        SceNetInitParam param;

        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
            return -1;
        }
        param.memory = s_netMem;
        param.size = sizeof(s_netMem);
        param.flags = 0;
        if (sceNetInit(&param) < 0) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            return -1;
        }
        if (sceNetCtlInit() < 0) {
            sceNetTerm();
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            return -1;
        }
        s_netInitialized = 1;
    }

    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        return -1;
    }
    return 0;
}

void platform_net_shutdown(void)
{
    if (s_netInitialized) {
        sceNetCtlTerm();
        sceNetTerm();
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        s_netInitialized = 0;
    }
}

int platform_net_is_modem(void)
{
    return 0;
}

int platform_get_region(void)
{
    return 0;
}
