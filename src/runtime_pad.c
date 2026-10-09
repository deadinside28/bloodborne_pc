/* libScePad on SDL3 gamepads, the keyboard and the mouse. SDL events are pumped by
 * the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Souls-like keyboard layout (also with a gamepad connected: everything adds up):
 *   WASD left stick (move), mouse right stick (look, F1 captures/releases),
 *   arrows right stick fallback, E Cross (interact), Space Circle (dodge/sprint),
 *   Left Ctrl Square (item), R Triangle (blood vial), F L1 (transform), Left Mouse R1
 *   (light), Shift+Left Mouse R2 (heavy), Right Mouse L2 (firearm), Q or C or Middle
 *   Mouse R3 (lock-on), Enter or Esc Options, Tab / Backspace touchpads, IJKL d-pad,
 *   Z L3. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* BB_GAMEPAD (the launcher's controller choice): its SDL GUID, or part of its name. Issue #15:
 * wheels and other controllers connected for good came first. */
static const char *preferred_gamepad(void) {
    static const char *want; static int read;
    if (!read) { want=getenv("BB_GAMEPAD"); if (want && !*want) want=NULL; read=1; }
    return want;
}
static int is_preferred(SDL_JoystickID id, const char *want) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,sizeof guid);
    const char *name=SDL_GetGamepadNameForID(id);
    return !strcasecmp(guid,want) || (name && strcasestr(name,want));
}
/* The chosen gamepad, else the first while it is not connected (checked again every second, it
 * is taken as soon as it connects); called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    static int on_preferred; static uint64_t last_scan;
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    const char *want=preferred_gamepad();
    const uint64_t now=now_us();
    if (!gamepad || (want && !on_preferred && now-last_scan>1000000)) {
        last_scan=now;
        int count=0, pick=-1;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        for (int i=0; want && ids && i<count && pick<0; ++i) if (is_preferred(ids[i],want)) pick=i;
        if (pick<0 && !gamepad && ids && count>0) pick=0;
        if (pick>=0 && (!gamepad || SDL_GetGamepadID(gamepad)!=ids[pick])) {
            if (gamepad) SDL_CloseGamepad(gamepad);
            gamepad=SDL_OpenGamepad(ids[pick]);
            on_preferred=want && gamepad && is_preferred(ids[pick],want);
            if (gamepad) {
                ++connected_count;
                printf("Runtime: gamepad connected: %s%s\n",SDL_GetGamepadName(gamepad),
                       !want ? "" : on_preferred ? " (the chosen one)" : " (the chosen one is not connected)");
            }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* bbport (frame stats): the game's libc heap, read every 5 s from a game thread (it reads the pad)
 * with libc.prx's malloc_stats (export stub at libc.prx+0x1f8c0; malloc_stats_fast returns 1 in this libc, the module at image +0x56e0000):
 * in use now and at most, and what it took from the system. A heap that keeps growing is a leak. */
typedef struct { uint16_t size, version; uint32_t reserved; uint64_t max_system, system, max_in_use, in_use; } MallocManagedSize;
static void report_guest_heap(void) {
    static int enabled=-1; static uint64_t last;
    if (enabled<0) enabled=getenv("BB_FRAME_STATS")!=NULL;
    const uint64_t now=now_us();
    if (!enabled || now-last<5000000) return;
    last=now;
    const uint8_t *stub=(const uint8_t *)(0x800000000ull+0x56e0000+0x1f8c0);
    if (stub[0]!=0xff || stub[1]!=0x25) return; /* another libc */
    ABI int (*stats)(MallocManagedSize *)=(ABI int (*)(MallocManagedSize *))(uintptr_t)stub;
    MallocManagedSize m={.size=sizeof(m),.version=1};
    const int result=stats(&m);
    if (result!=0) { static int told; if (!told++) printf("Guest heap: malloc_stats returned %#x\n",(unsigned)result); return; }
    {
        printf("Guest heap: %.1f MB in use (most %.1f), %.1f MB from the system (most %.1f)\n",
               m.in_use/1048576.0,m.max_in_use/1048576.0,m.system/1048576.0,m.max_system/1048576.0);
    }
}
/* Controls: what each PS4 input is bound to. Defaults below; bbport.ini (BB_CONFIG) lines
 * key.<input>=<SDL key names>, pad.<input>=<SDL gamepad button names> and
 * mouse.<input>=<mouse button names>, comma-separated, replace an input's binding
 * (empty: unbound). Inputs: the buttons (cross ... right, touchpad =
 * a left-side click, touchpad_right), and on the keyboard the sticks: move_* (left), look_*
 * (right). Gamepad names as SDL's: a b x y back start leftstick rightstick leftshoulder
 * rightshoulder dpup dpdown dpleft dpright touchpad misc1 paddle1-4, plus lefttrigger and
 * righttrigger. Mouse names: left right middle x1 x2. Heavy attack is a chord, not a
 * binding: Shift + Left Mouse = R2 while Left Mouse alone = R1. Look sensitivity:
 * BB_MOUSE_SENS (0.1..20) overrides the bbport.ini mouse_sens line, else 2.0.
 * The keyboard works next to a gamepad (the Steam Deck always has one): its buttons
 * add to the gamepad's, a held move/look key moves the stick all the way. */
enum {
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_OPTIONS, IN_TOUCHPAD, IN_TOUCHPAD_RIGHT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_MOVE_UP, IN_MOVE_DOWN, IN_MOVE_LEFT, IN_MOVE_RIGHT, IN_LOOK_UP, IN_LOOK_DOWN, IN_LOOK_LEFT,
    IN_LOOK_RIGHT, IN_COUNT
};
static const char *const input_names[IN_COUNT]={
    "cross","circle","square","triangle","l1","r1","l2","r2","l3","r3","options","touchpad",
    "touchpad_right","up","down","left","right","move_up","move_down","move_left","move_right",
    "look_up","look_down","look_left","look_right",
};
static const uint32_t input_buttons[IN_COUNT]={
    BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,
    BTN_OPTIONS,BTN_TOUCHPAD,0,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT,
};
#define MAX_BIND 4
enum { PAD_LEFT_TRIGGER=SDL_GAMEPAD_BUTTON_COUNT, PAD_RIGHT_TRIGGER }; /* triggers as buttons */
enum { MOUSE_NONE=0, MOUSE_LEFT=SDL_BUTTON_LEFT, MOUSE_MIDDLE=SDL_BUTTON_MIDDLE,
       MOUSE_RIGHT=SDL_BUTTON_RIGHT, MOUSE_X1=SDL_BUTTON_X1, MOUSE_X2=SDL_BUTTON_X2 };
typedef struct {
    int key_count, pad_count, mouse_count;
    SDL_Scancode keys[MAX_BIND]; int pad[MAX_BIND]; int mouse[MAX_BIND];
} Binding;
static Binding bindings[IN_COUNT];
static int bindings_ready;

static void bind_defaults(void) {
    static const struct { int input; SDL_Scancode key; } keys[]={
        {IN_CROSS,SDL_SCANCODE_E}, {IN_CIRCLE,SDL_SCANCODE_SPACE},
        {IN_SQUARE,SDL_SCANCODE_LCTRL},
        {IN_TRIANGLE,SDL_SCANCODE_R},
        {IN_L1,SDL_SCANCODE_F},
        {IN_L3,SDL_SCANCODE_Z}, {IN_R3,SDL_SCANCODE_Q}, {IN_R3,SDL_SCANCODE_C},
        {IN_OPTIONS,SDL_SCANCODE_RETURN}, {IN_OPTIONS,SDL_SCANCODE_ESCAPE},
        {IN_TOUCHPAD,SDL_SCANCODE_TAB},
        {IN_TOUCHPAD_RIGHT,SDL_SCANCODE_BACKSPACE},
        {IN_UP,SDL_SCANCODE_I}, {IN_DOWN,SDL_SCANCODE_K}, {IN_LEFT,SDL_SCANCODE_J}, {IN_RIGHT,SDL_SCANCODE_L},
        {IN_MOVE_UP,SDL_SCANCODE_W}, {IN_MOVE_DOWN,SDL_SCANCODE_S}, {IN_MOVE_LEFT,SDL_SCANCODE_A},
        {IN_MOVE_RIGHT,SDL_SCANCODE_D}, {IN_LOOK_UP,SDL_SCANCODE_UP}, {IN_LOOK_DOWN,SDL_SCANCODE_DOWN},
        {IN_LOOK_LEFT,SDL_SCANCODE_LEFT}, {IN_LOOK_RIGHT,SDL_SCANCODE_RIGHT},
    };
    static const struct { int input; int button; } mice[]={
        {IN_R1,MOUSE_LEFT}, {IN_L2,MOUSE_RIGHT}, {IN_R3,MOUSE_MIDDLE},
    };
    static const struct { int input, button; } pads[]={
        {IN_CROSS,SDL_GAMEPAD_BUTTON_SOUTH}, {IN_CIRCLE,SDL_GAMEPAD_BUTTON_EAST},
        {IN_SQUARE,SDL_GAMEPAD_BUTTON_WEST}, {IN_TRIANGLE,SDL_GAMEPAD_BUTTON_NORTH},
        {IN_L1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {IN_R1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {IN_L2,PAD_LEFT_TRIGGER}, {IN_R2,PAD_RIGHT_TRIGGER},
        {IN_L3,SDL_GAMEPAD_BUTTON_LEFT_STICK}, {IN_R3,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {IN_OPTIONS,SDL_GAMEPAD_BUTTON_START},
        {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_BACK}, {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {IN_UP,SDL_GAMEPAD_BUTTON_DPAD_UP}, {IN_DOWN,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {IN_LEFT,SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {IN_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    memset(bindings,0,sizeof bindings);
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        Binding *b=&bindings[keys[i].input]; b->keys[b->key_count++]=keys[i].key;
    }
    for (size_t i=0;i<sizeof(pads)/sizeof(*pads);++i) {
        Binding *b=&bindings[pads[i].input]; b->pad[b->pad_count++]=pads[i].button;
    }
    for (size_t i=0;i<sizeof(mice)/sizeof(*mice);++i) {
        Binding *b=&bindings[mice[i].input]; b->mouse[b->mouse_count++]=mice[i].button;
    }
}
static int pad_button_from_name(const char *name) {
    if (!SDL_strcasecmp(name,"lefttrigger")) return PAD_LEFT_TRIGGER;
    if (!SDL_strcasecmp(name,"righttrigger")) return PAD_RIGHT_TRIGGER;
    const SDL_GamepadButton b=SDL_GetGamepadButtonFromString(name);
    return b==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}
static int mouse_button_from_name(const char *name) {
    if (!SDL_strcasecmp(name,"left")) return MOUSE_LEFT;
    if (!SDL_strcasecmp(name,"right")) return MOUSE_RIGHT;
    if (!SDL_strcasecmp(name,"middle")) return MOUSE_MIDDLE;
    if (!SDL_strcasecmp(name,"x1") || !SDL_strcasecmp(name,"side1")) return MOUSE_X1;
    if (!SDL_strcasecmp(name,"x2") || !SDL_strcasecmp(name,"side2")) return MOUSE_X2;
    return -1;
}
/* Look sensitivity: BB_MOUSE_SENS overrides bbport.ini mouse_sens, else 2.0. */
static float mouse_ini_sens;
static float mouse_sens(void) {
    const char *env=getenv("BB_MOUSE_SENS");
    if (env && *env) {
        float f=(float)atof(env);
        if (f<0.1f) f=0.1f; else if (f>20.0f) f=20.0f;
        return f;
    }
    if (mouse_ini_sens>=0.1f) return mouse_ini_sens;
    return 2.0f;
}
/* key.<input>= / pad.<input>= / mouse.<input>= lines of the settings file, plus a
 * global mouse_sens=<0.1..20> line. */
static void load_bindings(void) {
    bind_defaults();
    mouse_ini_sens=0;
    const char *path=getenv("BB_CONFIG");
    FILE *f=path ? fopen(path,"r") : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line,sizeof line,f)) {
        char *eq=strchr(line,'=');
        if (!eq) continue;
        *eq=0;
        char *key=line;
        while (*key==' ') ++key;
        for (char *end=key+strlen(key); end>key && end[-1]==' ';) *--end=0;
        if (!strcmp(key,"mouse_sens")) {
            float v=(float)atof(eq+1);
            if (v>=0.1f && v<=20.0f) mouse_ini_sens=v;
            else printf("Runtime: controls: mouse_sens out of range (0.1..20)\n");
            continue;
        }
        const int keyboard=!strncmp(key,"key.",4), pad=!strncmp(key,"pad.",4), mouse=!strncmp(key,"mouse.",6);
        if ((!keyboard && !pad && !mouse)) continue;
        const char *iname=keyboard ? key+4 : pad ? key+4 : key+6;
        int input=-1;
        for (int i=0;i<IN_COUNT;++i) if (!strcmp(iname,input_names[i])) input=i;
        if (input<0 || ((pad || mouse) && input>=IN_MOVE_UP)) { printf("Runtime: controls: unknown input %s\n",key); continue; }
        Binding *b=&bindings[input];
        if (keyboard) b->key_count=0; else if (pad) b->pad_count=0; else b->mouse_count=0;
        for (char *name=strtok(eq+1,",\r\n"); name; name=strtok(NULL,",\r\n")) {
            while (*name==' ') ++name;
            for (char *end=name+strlen(name); end>name && end[-1]==' ';) *--end=0;
            if (!*name) continue;
            if (keyboard) {
                const SDL_Scancode s=SDL_GetScancodeFromName(name);
                if (s==SDL_SCANCODE_UNKNOWN) printf("Runtime: controls: unknown key \"%s\" for %s\n",name,iname);
                else if (b->key_count<MAX_BIND) b->keys[b->key_count++]=s;
            } else if (pad) {
                const int button=pad_button_from_name(name);
                if (button<0) printf("Runtime: controls: unknown gamepad button \"%s\" for %s\n",name,iname);
                else if (b->pad_count<MAX_BIND) b->pad[b->pad_count++]=button;
            } else {
                const int button=mouse_button_from_name(name);
                if (button<0) printf("Runtime: controls: unknown mouse button \"%s\" for %s\n",name,iname);
                else if (b->mouse_count<MAX_BIND) b->mouse[b->mouse_count++]=button;
            }
        }
    }
    fclose(f);
    printf("Runtime: controls: mouse sens %.2f (BB_MOUSE_SENS or mouse_sens)\n",mouse_sens());
}
static int key_down(const bool *k, int input) {
    for (int i=0;i<bindings[input].key_count;++i) if (k[bindings[input].keys[i]]) return 1;
    return 0;
}
/* The bound gamepad buttons' state; triggers as their analog value. */
static int pad_value(SDL_Gamepad *g, int input) {
    int value=0;
    for (int i=0;i<bindings[input].pad_count;++i) {
        const int b=bindings[input].pad[i];
        const int v=b==PAD_LEFT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
                  : b==PAD_RIGHT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                  : SDL_GetGamepadButton(g,(SDL_GamepadButton)b) ? 255 : 0;
        if (v>value) value=v;
    }
    return value;
}

/* The bound mouse buttons' state (SDL_GetMouseState flags). */
static int mouse_down(SDL_MouseButtonFlags flags, int input) {
    for (int i=0;i<bindings[input].mouse_count;++i)
        if (flags & SDL_BUTTON_MASK(bindings[input].mouse[i])) return 1;
    return 0;
}
static int mouse_button_bound(int input, int button) {
    for (int i=0;i<bindings[input].mouse_count;++i) if (bindings[input].mouse[i]==button) return 1;
    return 0;
}
/* Souls-like mouse buttons. Shift + Left = R2 (heavy); plain Left = R1 (light) and
 * never both, so a heavy never double-fires a light. Follows the R1 binding: if the
 * user unbinds Left from R1 the chord goes away with it. */
static void apply_mouse_buttons(PadData *d, SDL_MouseButtonFlags flags, const bool *k) {
    const int shift=k && (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]);
    const int left=(flags & SDL_BUTTON_LMASK)!=0;
    const int heavy=shift && left && mouse_button_bound(IN_R1,MOUSE_LEFT);
    for (int i=IN_CROSS;i<=IN_RIGHT;++i) {
        if (i==IN_R1 && heavy) continue; /* suppressed while the chord fires R2 */
        if (i!=IN_TOUCHPAD && i!=IN_TOUCHPAD_RIGHT && mouse_down(flags,i)) d->buttons|=input_buttons[i];
    }
    if (heavy) d->buttons|=BTN_R2;
    if (mouse_down(flags,IN_TOUCHPAD)) touch_click(d,0);
    if (mouse_down(flags,IN_TOUCHPAD_RIGHT)) touch_click(d,1);
    if (mouse_down(flags,IN_L2) || heavy) d->l2=255;
    if (mouse_down(flags,IN_R2) || heavy) d->r2=255;
}

/* key held for the negative / positive direction: the stick all the way, else the gamepad's. */
static uint8_t key_axis(uint8_t value, int negative, int positive) {
    return negative || positive ? (uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0)) : value;
}
static void apply_keyboard(PadData *d, const bool *k) {
    for (int i=IN_CROSS;i<=IN_RIGHT;++i)
        if (i!=IN_TOUCHPAD && i!=IN_TOUCHPAD_RIGHT && key_down(k,i)) d->buttons|=input_buttons[i];
    if (key_down(k,IN_TOUCHPAD)) touch_click(d,0);
    if (key_down(k,IN_TOUCHPAD_RIGHT)) touch_click(d,1);
    if (key_down(k,IN_L2)) d->l2=255;
    if (key_down(k,IN_R2)) d->r2=255;
    d->left_x=key_axis(d->left_x,key_down(k,IN_MOVE_LEFT),key_down(k,IN_MOVE_RIGHT));
    d->left_y=key_axis(d->left_y,key_down(k,IN_MOVE_UP),key_down(k,IN_MOVE_DOWN));
    d->right_x=key_axis(d->right_x,key_down(k,IN_LOOK_LEFT),key_down(k,IN_LOOK_RIGHT));
    d->right_y=key_axis(d->right_y,key_down(k,IN_LOOK_UP),key_down(k,IN_LOOK_DOWN));
}

/* Mouse look: virtual right stick driven by mouse deltas while captured, decaying to
 * center when the mouse is still. While captured it overrides the gamepad/arrow-key
 * right stick; release (F1) to use those again. Sensitivity from mouse_sens(). */
#define MOUSE_TAU_US 80000ull /* decay time: ~80 ms to ~37%, ~200 ms to center */
static float mouse_rx=0, mouse_ry=0;
static uint64_t mouse_last_us=0;
static void apply_mouse(PadData *d, double dx, double dy, uint64_t now, int captured) {
    if (!captured) { mouse_rx=0; mouse_ry=0; mouse_last_us=now; return; }
    uint64_t dt=now>=mouse_last_us ? now-mouse_last_us : 0;
    if (!mouse_last_us || dt>100000) dt=16000;
    mouse_last_us=now;
    const float sens=mouse_sens();
    float factor=1.0f-(float)dt/(float)MOUSE_TAU_US;
    if (factor<0.0f) factor=0.0f; else if (factor>1.0f) factor=1.0f;
    mouse_rx=mouse_rx*factor+(float)(dx*sens);
    mouse_ry=mouse_ry*factor+(float)(dy*sens);
    if (mouse_rx<-128.0f) mouse_rx=-128.0f; else if (mouse_rx>127.0f) mouse_rx=127.0f;
    if (mouse_ry<-128.0f) mouse_ry=-128.0f; else if (mouse_ry>127.0f) mouse_ry=127.0f;
    int rx=128+(int)mouse_rx, ry=128+(int)mouse_ry;
    if (rx<0) rx=0; else if (rx>255) rx=255;
    if (ry<0) ry=0; else if (ry>255) ry=255;
    d->right_x=(uint8_t)rx; d->right_y=(uint8_t)ry;
    if (dx!=0 || dy!=0) printf("Runtime: mouse look dx=%.1f dy=%.1f rx=%d ry=%d\n",dx,dy,rx,ry);
}

static void sample_host(PadData *d) {
    report_guest_heap();
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (!bindings_ready) { load_bindings(); bindings_ready=1; }
    double mouse_dx=0, mouse_dy=0;
    bbgpu_mouse_delta(&mouse_dx,&mouse_dy);
    int mouse_on=bbgpu_mouse_is_captured();
    if (bbgpu_overlay_captures_input()) {
        /* Menu/IME open: neutral input, capture already released window-side;
         * drop deltas and park the virtual stick at center. */
        mouse_rx=0; mouse_ry=0; mouse_last_us=d->timestamp;
        return;
    }
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    const SDL_MouseButtonFlags mflags=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetMouseState(NULL,NULL) : 0;
    if (g) {
        int touch_right=0;
        for (int i=IN_CROSS;i<=IN_RIGHT;++i) {
            const int v=pad_value(g,i);
            if (i==IN_L2) d->l2=(uint8_t)v;
            if (i==IN_R2) d->r2=(uint8_t)v;
            if (i==IN_TOUCHPAD_RIGHT) touch_right=v>30;
            else if (v>30) d->buttons|=input_buttons[i];
        }
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        if (touch_right) touch_click(d,1);
    }
    if (k) apply_keyboard(d,k);
    if (mflags) apply_mouse_buttons(d,mflags,k);
    apply_mouse(d,mouse_dx,mouse_dy,d->timestamp,mouse_on);
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Touches as the DualShock 4 reports them: every finger that goes down gets a new id (1..127, kept
 * while it stays down) and the time since the first one went down. With id 0 and no hold time
 * the game ignored touchpad presses: no gesture menu from the touchpad, Back or Tab. */
static void touch_ids(PadData *d) {
    static uint8_t next_id=1, ids[2]; static int down[2]; static uint64_t since;
    for (int i=0;i<2;++i) {
        const int now=i<d->touch_count;
        if (now && !down[i]) { ids[i]=next_id; next_id=next_id==127 ? 1 : next_id+1; }
        down[i]=now;
        if (now) d->touches[i].id=ids[i];
    }
    if (!d->touch_count) since=0;
    else if (!since) since=d->timestamp;
    d->touch_held_time=d->touch_count ? (uint32_t)(d->timestamp-since) : 0;
}
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) return;
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    touch_ids(d);
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
