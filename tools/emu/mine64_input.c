/* Scripted input plugin for mupen64plus.
 *
 * Replaces mupen64plus-input-sdl with a plugin that replays a fixed timeline of
 * controller states read from a text file, so a run of the game is reproducible
 * and needs no keyboard, window focus, or desktop automation.  The script can
 * also ask the core for a screenshot at an exact point in the timeline, which is
 * what makes it useful for capturing the GUI.
 *
 * The script path comes from MINE64_INPUT_SCRIPT.  Commands, one per line:
 *
 *   wait N               neutral controller for N frames
 *   press BTN [N]        hold BTN for N frames (default 4), then release
 *   hold BTN N           same as press, explicit duration
 *   stick X Y N          analog stick at (X,Y), each in -80..80, for N frames
 *   shot [label]         capture a screenshot into --sshotdir
 *   stop                 end emulation
 *
 * BTN is one of A B Z START L R and the C/D pads as CUP CDOWN CLEFT CRIGHT /
 * DUP DDOWN DLEFT DRIGHT.  Several may be joined with '+' (e.g. "press A+Z").
 *
 * Durations count rendered frames, not GetKeys calls: the game polls the pad
 * many times per frame while it is generating a world, so a poll-based timeline
 * races through the loading screens and drops presses the game never sees.
 */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_common.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_plugin.h>
#include <mupen64plus/m64p_types.h>

#define INPUT_API_VERSION 0x020100
#define PLUGIN_VERSION    0x010000

/* One timeline entry: a controller state held for `frames`, or a core command. */
typedef struct {
    BUTTONS keys;
    int frames;
    int shot;
    int stop;
} Step;

static Step *steps = NULL;
static int stepCount = 0;
static int stepCapacity = 0;

static int cursor = 0;       /* index of the step being replayed */
static int cursorFrame = 0;  /* frames already spent on that step */

/* RenderCallback runs on the video plugin's thread; GetKeys reads the counter
 * and consumes the difference, so the timeline never advances mid-frame. */
static volatile unsigned int framesRendered = 0;
static unsigned int framesConsumed = 0;
static int sawRenderCallback = 0;
static int pollsWithoutFrame = 0;

/* Frames still to elapse before `stop` takes effect, so a screenshot requested
 * just before it has a frame to be written in.  Zero means no stop is pending. */
static int stopCountdown = 0;

/* The state reported to every poll within the current frame. */
static BUTTONS heldKeys;

static void (*debugCallback)(void *, int, const char *) = NULL;
static void *debugContext = NULL;
static ptr_CoreDoCommand coreDoCommand = NULL;

static void logf_(int level, const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (debugCallback != NULL)
        debugCallback(debugContext, level, line);
    else
        fprintf(stderr, "mine64-input: %s\n", line);
}

static Step *appendStep(void)
{
    if (stepCount == stepCapacity) {
        int grown = stepCapacity ? stepCapacity * 2 : 64;
        Step *bigger = realloc(steps, (size_t)grown * sizeof(Step));
        if (bigger == NULL)
            return NULL;
        steps = bigger;
        stepCapacity = grown;
    }
    Step *step = &steps[stepCount++];
    memset(step, 0, sizeof(*step));
    step->frames = 1;
    return step;
}

/* Set the named button in `keys`.  Returns 0 for an unknown name. */
static int applyButton(BUTTONS *keys, const char *name)
{
    static const struct { const char *name; unsigned int mask; } table[] = {
        { "A",      1u << 7  }, { "B",      1u << 6  },
        { "Z",      1u << 5  }, { "START",  1u << 4  },
        { "L",      1u << 13 }, { "R",      1u << 12 },
        { "CUP",    1u << 11 }, { "CDOWN",  1u << 10 },
        { "CLEFT",  1u << 9  }, { "CRIGHT", 1u << 8  },
        { "DUP",    1u << 3  }, { "DDOWN",  1u << 2  },
        { "DLEFT",  1u << 1  }, { "DRIGHT", 1u << 0  },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(name, table[i].name) == 0) {
            keys->Value |= table[i].mask;
            return 1;
        }
    }
    return 0;
}

/* "A+Z" -> both bits set.  Returns 0 if any component is unknown. */
static int applyButtonList(BUTTONS *keys, char *list)
{
    for (char *part = strtok(list, "+"); part != NULL; part = strtok(NULL, "+")) {
        if (!applyButton(keys, part))
            return 0;
    }
    return 1;
}

static int clampAxis(long value)
{
    if (value > 80) return 80;
    if (value < -80) return -80;
    return (int)value;
}

static void loadScript(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        logf_(M64MSG_ERROR, "cannot open script '%s'", path);
        return;
    }

    char line[256];
    int lineNumber = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        lineNumber++;
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';

        char verb[64], arg[64];
        long a = 0, b = 0, c = 0;
        int fields = sscanf(line, "%63s %63s %ld %ld", verb, arg, &b, &c);
        if (fields < 1)
            continue;

        Step *step = appendStep();
        if (step == NULL) {
            logf_(M64MSG_ERROR, "out of memory reading script");
            break;
        }

        if (strcasecmp(verb, "wait") == 0 && fields >= 2) {
            step->frames = (int)strtol(arg, NULL, 10);
        } else if ((strcasecmp(verb, "press") == 0 || strcasecmp(verb, "hold") == 0) && fields >= 2) {
            if (!applyButtonList(&step->keys, arg))
                logf_(M64MSG_WARNING, "line %d: unknown button '%s'", lineNumber, arg);
            step->frames = (fields >= 3) ? (int)b : 4;
        } else if (strcasecmp(verb, "stick") == 0 && fields >= 4) {
            a = strtol(arg, NULL, 10);
            step->keys.X_AXIS = clampAxis(a);
            step->keys.Y_AXIS = clampAxis(b);
            step->frames = (int)c;
        } else if (strcasecmp(verb, "shot") == 0) {
            step->shot = 1;
        } else if (strcasecmp(verb, "stop") == 0) {
            step->stop = 1;
        } else {
            logf_(M64MSG_WARNING, "line %d: unrecognised command '%s'", lineNumber, verb);
            stepCount--;
            continue;
        }

        if (step->frames < 1)
            step->frames = 1;
    }

    fclose(file);
    logf_(M64MSG_INFO, "loaded %d steps from %s", stepCount, path);
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle coreHandle, void *context,
                                     void (*callback)(void *, int, const char *))
{
    debugContext = context;
    debugCallback = callback;
    coreDoCommand = (ptr_CoreDoCommand)dlsym(coreHandle, "CoreDoCommand");

    const char *path = getenv("MINE64_INPUT_SCRIPT");
    if (path != NULL && path[0] != '\0')
        loadScript(path);
    else
        logf_(M64MSG_WARNING, "MINE64_INPUT_SCRIPT is unset; controller stays neutral");

    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    free(steps);
    steps = NULL;
    stepCount = stepCapacity = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version,
                                        int *apiVersion, const char **name, int *capabilities)
{
    if (type != NULL)         *type = M64PLUGIN_INPUT;
    if (version != NULL)      *version = PLUGIN_VERSION;
    if (apiVersion != NULL)   *apiVersion = INPUT_API_VERSION;
    if (name != NULL)         *name = "Mine64 scripted input";
    if (capabilities != NULL) *capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT void CALL InitiateControllers(CONTROL_INFO controlInfo)
{
    /* Only controller 1 is plugged in; the game treats 2-4 as co-op players. */
    controlInfo.Controls[0].Present = 1;
    controlInfo.Controls[0].RawData = 0;
    controlInfo.Controls[0].Plugin = PLUGIN_NONE;
    for (int i = 1; i < 4; i++)
        controlInfo.Controls[i].Present = 0;
}

/* Move the timeline on by one rendered frame and return the controller state to
 * report for it. */
static BUTTONS stepFrame(void)
{
    BUTTONS keys;
    keys.Value = 0;

    if (stopCountdown > 0 && --stopCountdown == 0 && coreDoCommand != NULL)
        coreDoCommand(M64CMD_STOP, 0, NULL);

    while (cursor < stepCount) {
        Step *step = &steps[cursor];

        if (step->shot) {
            /* Requesting the capture and then spending this frame on it lets
             * the core write out the frame the script meant to photograph. */
            if (coreDoCommand != NULL)
                coreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
            cursor++;
            cursorFrame = 0;
            return keys;
        }

        if (step->stop) {
            if (stopCountdown == 0)
                stopCountdown = 5;
            return keys;
        }

        if (cursorFrame < step->frames) {
            cursorFrame++;
            return step->keys;
        }

        cursor++;
        cursorFrame = 0;
    }

    return keys;
}

EXPORT void CALL GetKeys(int control, BUTTONS *keys)
{
    keys->Value = 0;
    if (control != 0)
        return;

    unsigned int rendered = framesRendered;
    if (rendered != framesConsumed) {
        sawRenderCallback = 1;
        pollsWithoutFrame = 0;
        while (framesConsumed != rendered) {
            framesConsumed++;
            heldKeys = stepFrame();
        }
    } else if (!sawRenderCallback && ++pollsWithoutFrame > 600) {
        /* No video plugin is feeding RenderCallback; fall back to counting
         * polls so a script still runs to completion instead of hanging. */
        heldKeys = stepFrame();
    }

    *keys = heldKeys;
}

/* The core calls these unconditionally; nothing here needs them. */
EXPORT void CALL ControllerCommand(int control, unsigned char *command) { }
EXPORT void CALL ReadController(int control, unsigned char *command) { }
EXPORT int  CALL RomOpen(void) { return 1; }
EXPORT void CALL SDL_KeyDown(int keymod, int keysym) { }
EXPORT void CALL SDL_KeyUp(int keymod, int keysym) { }

EXPORT void CALL RomClosed(void)
{
    cursor = cursorFrame = 0;
    framesRendered = framesConsumed = 0;
    sawRenderCallback = pollsWithoutFrame = stopCountdown = 0;
    heldKeys.Value = 0;
}

/* The core routes this through the video plugin once per rendered frame. */
EXPORT void CALL RenderCallback(void) { framesRendered++; }
EXPORT void CALL SendVRUWord(uint16_t length, uint16_t *word, uint8_t lang) { }
EXPORT void CALL SetMicState(int state) { }
EXPORT void CALL ReadVRUResults(uint16_t *errorFlags, uint16_t *numResults, uint16_t *micLevel,
                                uint16_t *voiceLevel, uint16_t *voiceLength, uint16_t *matches) { }
EXPORT void CALL ClearVRUWords(uint8_t length) { }
EXPORT void CALL SetVRUWordMask(uint8_t length, uint8_t *mask) { }
