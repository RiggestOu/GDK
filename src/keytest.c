/* ============================================================
 * GDK mini 按键测试器 —— 逐个提示按键，完整记录两层键码：
 *   [SDL] KEYDOWN/KEYUP sym/scancode、JOYBUTTON、JOYHAT
 *   [EV]  /dev/input/eventN 的内核 evdev code/value（真值，不经 SDL 翻译）
 * 日志输出到 stderr（由 launch.sh 重定向到 /media/roms/apps/KeyTest.log）。
 *
 * 操作：屏幕提示「请按 XX」，按下并松开该键即自动进入下一个；
 *       全部完成（或任意时刻按 电源 以外的组合无法退出时）自动退出。
 *       每个键最多等 15 秒，超时自动跳过（防某键坏死卡住）。
 * 编译：与 epubreader 相同工具链，⛔必须 -O0（MXU 铁律）。
 * ============================================================ */
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define FONT_PATH "/usr/share/fonts/SourceHanSans-Regular-04.ttf"
#define EVDEV_MAX 8

static int g_evfd[EVDEV_MAX];
static int g_evfd_n = 0;

static void evdev_init(void) {
    char path[32], name[64];
    for (int i = 0; i < EVDEV_MAX; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        name[0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        fprintf(stderr, "[init] evdev %s = %s\n", path, name);
        g_evfd[g_evfd_n++] = fd;
        if (g_evfd_n >= EVDEV_MAX) break;
    }
    fflush(stderr);
}

/* 读空所有 evdev 事件并记录；返回：+1 有按下，-1 有抬起，0 无事件 */
static int evdev_drain(const char *tag) {
    struct input_event ie;
    int got = 0;
    for (int i = 0; i < g_evfd_n; i++) {
        while (read(g_evfd[i], &ie, sizeof ie) == (ssize_t)sizeof ie) {
            if (ie.type != EV_KEY || ie.value == 2) continue;
            fprintf(stderr, "[EV]  %-12s ev%d code=%-4d val=%d\n", tag, i, (int)ie.code, (int)ie.value);
            got = ie.value ? 1 : -1;
        }
    }
    if (got) fflush(stderr);
    return got;
}

/* 记录一条 SDL 事件；返回：+1 按下类，-1 抬起类，0 其它 */
static int log_sdl(const char *tag, const SDL_Event *ev) {
    switch (ev->type) {
        case SDL_KEYDOWN:
            fprintf(stderr, "[SDL] %-12s KEYDOWN sym=%-4d sc=%d\n", tag, (int)ev->key.keysym.sym, (int)ev->key.keysym.scancode);
            return 1;
        case SDL_KEYUP:
            fprintf(stderr, "[SDL] %-12s KEYUP   sym=%-4d sc=%d\n", tag, (int)ev->key.keysym.sym, (int)ev->key.keysym.scancode);
            return -1;
        case SDL_JOYBUTTONDOWN:
            fprintf(stderr, "[SDL] %-12s JOYBTN  b=%d DOWN\n", tag, (int)ev->jbutton.button);
            return 1;
        case SDL_JOYBUTTONUP:
            fprintf(stderr, "[SDL] %-12s JOYBTN  b=%d UP\n", tag, (int)ev->jbutton.button);
            return -1;
        case SDL_JOYHATMOTION:
            fprintf(stderr, "[SDL] %-12s JOYHAT  v=%d\n", tag, (int)ev->jhat.value);
            return ev->jhat.value ? 1 : -1;
        default:
            return 0;
    }
}

static void draw_center(SDL_Surface *scr, TTF_Font *font, const char *line1, const char *line2, const char *line3) {
    SDL_FillRect(scr, NULL, SDL_MapRGB(scr->format, 0, 0, 0));
    const char *ls[3] = { line1, line2, line3 };
    SDL_Color cols[3] = { {255,255,255,0}, {255,220,80,0}, {140,140,140,0} };
    int y = scr->h / 2 - 45;
    for (int i = 0; i < 3; i++) {
        if (!ls[i] || !ls[i][0]) { y += 30; continue; }
        if (font) {
            SDL_Surface *t = TTF_RenderUTF8_Solid(font, ls[i], cols[i]);
            if (t) {
                SDL_Rect d = { (Sint16)((scr->w - t->w) / 2), (Sint16)y, 0, 0 };
                SDL_BlitSurface(t, NULL, scr, &d);
                SDL_FreeSurface(t);
            }
        }
        y += 30;
    }
    SDL_Flip(scr);
}

int main(void) {
    if (!getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "fbcon", 1);
    if (!getenv("SDL_FBDEV"))       setenv("SDL_FBDEV", "/dev/fb0", 1);
    if (!getenv("SDL_NOMOUSE"))     setenv("SDL_NOMOUSE", "1", 1);

    fprintf(stderr, "[init] KeyTest build=" __DATE__ " " __TIME__ "\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "[init] SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_JoystickEventState(SDL_ENABLE);
    int nj = SDL_NumJoysticks();
    fprintf(stderr, "[init] joysticks=%d\n", nj);
    for (int i = 0; i < nj; i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        fprintf(stderr, "[init] joy%d=%s buttons=%d hats=%d\n", i, SDL_JoystickName(i),
                j ? SDL_JoystickNumButtons(j) : -1, j ? SDL_JoystickNumHats(j) : -1);
    }

    const SDL_VideoInfo *vi = SDL_GetVideoInfo();
    int W = (vi && vi->current_w > 0) ? vi->current_w : 320;
    int H = (vi && vi->current_h > 0) ? vi->current_h : 240;
    SDL_Surface *scr = SDL_SetVideoMode(W, H, 16, SDL_SWSURFACE);
    if (!scr) scr = SDL_SetVideoMode(320, 240, 16, SDL_SWSURFACE);
    if (!scr) { fprintf(stderr, "[init] SetVideoMode 失败: %s\n", SDL_GetError()); return 1; }
    fprintf(stderr, "[init] video %dx%d\n", scr->w, scr->h);

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        font = TTF_OpenFont(FONT_PATH, 20);
        if (!font) fprintf(stderr, "[init] 字体打开失败(%s)，仅日志无屏显\n", TTF_GetError());
    }

    evdev_init();

    static const char *keys[] = {
        "十字键 上", "十字键 下", "十字键 左", "十字键 右",
        "A 键", "B 键", "X 键", "Y 键",
        "L1", "L2", "R1", "R2",
        "START", "SELECT", "音量 +", "音量 -",
        "圆1 (左侧 1个圆点)", "圆2 (左侧 2个圆点)",
        "圆3 (START上方 左)", "圆4 (START上方 右)",
    };
    const int NK = (int)(sizeof(keys) / sizeof(keys[0]));

    for (int k = 0; k < NK; k++) {
        char l1[64], tag[32];
        snprintf(l1, sizeof l1, "(%d/%d) 请按: %s", k + 1, NK, keys[k]);
        snprintf(tag, sizeof tag, "<%s>", keys[k]);
        fprintf(stderr, "\n[PROMPT] ===== %d/%d %s =====\n", k + 1, NK, keys[k]);
        fflush(stderr);
        draw_center(scr, font, l1, "按下并松开该键", "15 秒无操作自动跳过");

        /* 丢弃上一个键的残余事件 */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) log_sdl("(残留)", &ev);
        evdev_drain("(残留)");

        Uint32 start = SDL_GetTicks();
        int pressed = 0;          /* 是否已出现按下 */
        Uint32 last_evt = 0;      /* 最后一次事件时刻 */
        for (;;) {
            int any = 0;
            while (SDL_PollEvent(&ev)) {
                int r = log_sdl(tag, &ev);
                if (r > 0) pressed = 1;
                if (r) { any = 1; last_evt = SDL_GetTicks(); }
            }
            int r2 = evdev_drain(tag);
            if (r2 > 0) pressed = 1;
            if (r2) { any = 1; last_evt = SDL_GetTicks(); }
            (void)any;
            Uint32 now = SDL_GetTicks();
            /* 完成条件：出现过按下，且 600ms 内无新事件（松开且稳定） */
            if (pressed && last_evt && (now - last_evt) > 600) break;
            /* 超时跳过 */
            if (!pressed && (now - start) > 15000) {
                fprintf(stderr, "[PROMPT] %s 超时跳过\n", keys[k]);
                break;
            }
            SDL_Delay(10);
        }
    }

    fprintf(stderr, "\n[done] 全部 %d 个键测试完成\n", NK);
    fflush(stderr);
    draw_center(scr, font, "测试完成!", "日志已写入 KeyTest.log", "3 秒后自动退出");
    SDL_Delay(3000);
    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
