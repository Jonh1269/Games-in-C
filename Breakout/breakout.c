/* ============================================================================
 * SUPER BREAKOUT (Atari 2600)
 * FEITO POR: Jonh1269/Pedro.
 * ----------------------------------------------------------------------------
 * Video: só X11 (Xlib). Audio: só ALSA (driver de som nativo do Linux).
 * Threads: pthread (POSIX). Nenhuma lib "de fora" (SDL, fontes, etc).
 *
 * Variacoes implementadas (só modo 1 jogador, conforme pedido):
 *   1 - BREAKOUT     (jogo classico)
 *   3 - DOUBLE        (2 bolas, 2 raquetes empilhadas, pontos em dobro)
 *   5 - CAVITY        (2 bolas cativas presas em bolsoes, libertadas ao
 *                       limpar os tijolos ao redor; pontos em dobro/triplo)
 *   7 - PROGRESSIVE   (a parede de tijolos desce sem parar, infinita)
 *   8 - CHILDRENS     (Breakout normal, mais lento, sem acelerar, sem
 *                       encolher a raquete)
 * Controles:
 *   - Movimento do mouse ..... move a(s) raquete(s) (unico jeito de mover)
 *   - G ...................... troca de variacao do jogo (1/3/5/7/8) e
 *                               comeca um jogo novo nessa variacao
 *   - B ...................... alterna a dificuldade (raquete grande/normal)
 *   - R ...................... reinicia o jogo atual (placar volta a 0)
 *   - P ...................... pausa / despausa
 *   - Ctrl+F ................. alterna tela cheia
 *   - ESC / fechar janela .... sai
 * ==========================================================================*/

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ---------------------------------------------------------------------- */
/* Sistema de coordenadas logico (independe do tamanho real da janela).   */
/* ---------------------------------------------------------------------- */
#define LOGICAL_W 1520.0
#define LOGICAL_H 1080.0

#define ROWS 8
#define COLS 13
#define MAX_BALLS 3
#define TURNS_LIMIT 5

#define BRICK_H       20.0
#define BRICK_GAP_Y   3.0
#define BRICK_LEFT    96.0
#define BRICK_RIGHT   1424.0
#define BRICK_GAP_X   6.0
#define BRICK_TOP     210.0
#define ROW_STEP      (BRICK_H + BRICK_GAP_Y)

#define WALL_TOP_Y0   130.0
#define WALL_TOP_Y1   204.0
#define WALL_SIDE_W   76.0
#define FIELD_BOTTOM  1050.0

#define PADDLE_Y      985.0
#define PADDLE_H      14.0
#define PADDLE_GAP    8.0
#define PADDLE_Y2     (PADDLE_Y - PADDLE_H - PADDLE_GAP)
#define PADDLE_W_EXPERT   130.0
#define PADDLE_W_BEGINNER 260.0
#define BALL_SIZE     16.0
#define BASE_SPEED    430.0

typedef enum {
    VAR_BREAKOUT    = 1,
    VAR_DOUBLE      = 3,
    VAR_CAVITY      = 5,
    VAR_PROGRESSIVE = 7,
    VAR_CHILDREN    = 8
} Variation;

static const int VARIATION_ORDER[5] = {VAR_BREAKOUT, VAR_DOUBLE, VAR_CAVITY, VAR_PROGRESSIVE, VAR_CHILDREN};

typedef struct { double r, g, b; } RGB;

typedef struct {
    double x, y, w, h;
    int alive;
    int points;
    int color_idx;
} Brick;

typedef struct {
    double x, y, vx, vy;
    int active;
    int captive;
    int hits;
    double box_x0, box_y0, box_x1, box_y1;
} Ball;

typedef struct {
    int used;
    double y;
    int value;
    int color_idx;
    int alive[COLS];
    double vanish_timer;
} PRow;
#define MAX_PROWS 28

typedef struct {
    Display *dpy;
    Window win;
    Window root;
    int screen;
    GC gc;
    Pixmap back;
    int win_w, win_h;
    Colormap cmap;
    Atom wm_delete;
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;
    int fullscreen;
} XApp;

/* ---------------------------- Estado do jogo ---------------------------- */
static Brick bricks[ROWS][COLS];
static int bricks_left;

static PRow prows[MAX_PROWS];
static double prog_scroll_speed;
static double prog_spawn_accum;
static int prog_next_idx;

static Ball balls[MAX_BALLS];
static int ball2_spawned;
static int cavity_left_released;
static int cavity_right_released;

static Variation variation = VAR_BREAKOUT;
static int beginner;
static double paddle_x, paddle_w;
static int paddle_shrunk;
static int paddle_hits;
static int hit_stage;

static int score;
static int turns_taken;
static int game_over;
static double game_over_timer; /* tempo mostrando o campo parado antes de reiniciar sozinho */
static int paused;
static int end_sound_played;

static int wall_clear_pending;
static double wall_clear_timer;

static const RGB ROW_COLORS[4] = {
    {196, 44, 44},
    {188, 102, 40},
    {172, 168, 45},
    {36, 150, 60},
};
static const int ROW_VALUES[ROWS] = {1, 1, 3, 3, 5, 5, 7, 7};

/* ========================================================================
 * AUDIO — onda quadrada sintetizada na hora, tocada via ALSA.
 * ======================================================================== */
#define SND_QUEUE_CAP 32
#define SND_RATE 44100

typedef struct { double freq; int dur_ms; double vol; } SoundReq;

static snd_pcm_t *g_pcm = NULL;
static int g_audio_ok = 0;
static pthread_t g_audio_thread;
static pthread_mutex_t g_snd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_snd_cond = PTHREAD_COND_INITIALIZER;
static SoundReq g_snd_queue[SND_QUEUE_CAP];
static int g_snd_head = 0, g_snd_tail = 0, g_snd_count = 0;
static int g_audio_quit = 0;

static void push_sound(double freq, int dur_ms, double vol) {
    if (!g_audio_ok) return;
    pthread_mutex_lock(&g_snd_mutex);
    if (g_snd_count < SND_QUEUE_CAP) {
        g_snd_queue[g_snd_tail] = (SoundReq){freq, dur_ms, vol};
        g_snd_tail = (g_snd_tail + 1) % SND_QUEUE_CAP;
        g_snd_count++;
        pthread_cond_signal(&g_snd_cond);
    }
    pthread_mutex_unlock(&g_snd_mutex);
}

static void play_tone_blocking(double freq, int dur_ms, double vol) {
    int n = (SND_RATE * dur_ms) / 1000;
    if (n <= 0) return;
    int16_t *buf = malloc((size_t)n * sizeof(int16_t));
    if (!buf) return;
    double period = SND_RATE / freq;
    int fade = n / 10 > 0 ? n / 10 : 1;
    for (int i = 0; i < n; i++) {
        double phase = fmod(i, period) / period;
        double square = (phase < 0.5) ? 1.0 : -1.0;
        double env = 1.0;
        if (i < fade) env = (double)i / fade;
        else if (i > n - fade) env = (double)(n - i) / fade;
        buf[i] = (int16_t)(square * env * vol * 9000.0);
    }
    snd_pcm_sframes_t written = 0;
    while (written < n) {
        snd_pcm_sframes_t r = snd_pcm_writei(g_pcm, buf + written, (snd_pcm_uframes_t)(n - written));
        if (r == -EPIPE || r == -ESTRPIPE) {
            snd_pcm_prepare(g_pcm);
        } else if (r < 0) {
            snd_pcm_recover(g_pcm, (int)r, 1);
            break;
        } else {
            written += r;
        }
    }
    snd_pcm_drain(g_pcm);
    snd_pcm_prepare(g_pcm);
    free(buf);
}

static void *audio_thread_func(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_snd_mutex);
        while (g_snd_count == 0 && !g_audio_quit)
            pthread_cond_wait(&g_snd_cond, &g_snd_mutex);
        if (g_audio_quit && g_snd_count == 0) {
            pthread_mutex_unlock(&g_snd_mutex);
            break;
        }
        SoundReq req = g_snd_queue[g_snd_head];
        g_snd_head = (g_snd_head + 1) % SND_QUEUE_CAP;
        g_snd_count--;
        pthread_mutex_unlock(&g_snd_mutex);
        play_tone_blocking(req.freq, req.dur_ms, req.vol);
    }
    return NULL;
}

static void init_audio(void) {
    int rc = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "Audio indisponivel (%s) - jogo continua mudo.\n", snd_strerror(rc));
        g_audio_ok = 0;
        return;
    }
    rc = snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             1, SND_RATE, 1, 50000);
    if (rc < 0) {
        fprintf(stderr, "Audio: falha ao configurar (%s) - jogo continua mudo.\n", snd_strerror(rc));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        g_audio_ok = 0;
        return;
    }
    g_audio_ok = 1;
    g_audio_quit = 0;
    pthread_create(&g_audio_thread, NULL, audio_thread_func, NULL);
}

static void cleanup_audio(void) {
    if (!g_audio_ok) return;
    pthread_mutex_lock(&g_snd_mutex);
    g_audio_quit = 1;
    pthread_cond_signal(&g_snd_cond);
    pthread_mutex_unlock(&g_snd_mutex);
    pthread_join(g_audio_thread, NULL);
    if (g_pcm) snd_pcm_close(g_pcm);
}

static void sfx_wall(void)      { push_sound(720, 30, 0.25); }
static void sfx_paddle(void)    { push_sound(480, 45, 0.35); }
static void sfx_brick(int row)  { push_sound(1300.0 - row * 90.0, 50, 0.40); }
static void sfx_bonus(void)     { push_sound(950, 40, 0.4); push_sound(1250, 70, 0.4); }
static void sfx_shrink(void)    { push_sound(300, 100, 0.35); push_sound(220, 140, 0.35); }
static void sfx_release(void)   { push_sound(600, 60, 0.4); push_sound(900, 60, 0.4); push_sound(1200, 90, 0.4); }
static void sfx_life_lost(void) { push_sound(220, 140, 0.45); push_sound(140, 180, 0.45); }
static void sfx_game_over(void) {
    push_sound(400, 120, 0.4); push_sound(320, 120, 0.4);
    push_sound(240, 120, 0.4); push_sound(160, 220, 0.4);
}
static void sfx_win(void) {
    push_sound(500, 90, 0.4); push_sound(650, 90, 0.4);
    push_sound(800, 90, 0.4); push_sound(1000, 160, 0.4);
}

/* ---------------------------------------------------------------------- */
static double frand(void) { return (double)rand() / (double)RAND_MAX; }

static int count_free_active_balls(void) {
    int c = 0;
    for (int i = 0; i < MAX_BALLS; i++)
        if (balls[i].active && !balls[i].captive) c++;
    return c;
}

static int current_multiplier(void) {
    if (variation == VAR_DOUBLE || variation == VAR_CAVITY) {
        int c = count_free_active_balls();
        return c < 1 ? 1 : c;
    }
    return 1;
}

static void add_score(int base_pts) {
    score += base_pts * current_multiplier();
    if (score > 9999) score %= 10000;
}

/* ---------------------------------------------------------------------- */
static void build_static_wall(void) {
    double total_w = BRICK_RIGHT - BRICK_LEFT;
    double bw = (total_w - (COLS - 1) * BRICK_GAP_X) / COLS;
    bricks_left = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            Brick *b = &bricks[r][c];
            b->x = BRICK_LEFT + c * (bw + BRICK_GAP_X);
            b->y = BRICK_TOP + r * ROW_STEP;
            b->w = bw;
            b->h = BRICK_H;
            b->points = ROW_VALUES[r];
            b->color_idx = (ROW_VALUES[r] - 1) / 2;
            b->alive = 1;
        }
    }
    if (variation == VAR_CAVITY) {
        for (int r = 2; r <= 3; r++) {
            bricks[r][2].alive = 0;
            bricks[r][3].alive = 0;
            bricks[r][9].alive = 0;
            bricks[r][10].alive = 0;
        }
    }
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (bricks[r][c].alive) bricks_left++;
}

static void progressive_pattern(int idx, int *is_brick, int *value) {
    int cyc = idx % 8; if (cyc < 0) cyc += 8;
    if (cyc >= 4) { *is_brick = 0; *value = 0; return; }
    int period = idx / 8;
    static const int lowvals[4]  = {1, 1, 3, 3};
    static const int highvals[4] = {5, 5, 7, 7};
    *is_brick = 1;
    *value = ((period % 2) == 1) ? highvals[cyc] : lowvals[cyc];
}

static void spawn_progressive_row(int idx) {
    int is_brick, value;
    progressive_pattern(idx, &is_brick, &value);
    if (!is_brick) return;
    for (int i = 0; i < MAX_PROWS; i++) {
        if (!prows[i].used) {
            prows[i].used = 1;
            prows[i].y = BRICK_TOP - ROW_STEP;
            prows[i].value = value;
            prows[i].color_idx = (value - 1) / 2;
            prows[i].vanish_timer = -1.0;
            for (int c = 0; c < COLS; c++) prows[i].alive[c] = 1;
            return;
        }
    }
}

static void build_progressive_wall(void) {
    memset(prows, 0, sizeof(prows));
    prog_scroll_speed = 55.0;
    prog_spawn_accum = 0.0;
    prog_next_idx = 0;
    for (int i = 0; i < 12; i++) {
        int is_brick, value;
        progressive_pattern(i, &is_brick, &value);
        prog_next_idx = i + 1;
        if (!is_brick) continue;
        for (int s = 0; s < MAX_PROWS; s++) {
            if (!prows[s].used) {
                prows[s].used = 1;
                prows[s].y = BRICK_TOP + i * ROW_STEP;
                prows[s].value = value;
                prows[s].color_idx = (value - 1) / 2;
                prows[s].vanish_timer = -1.0;
                for (int c = 0; c < COLS; c++) prows[s].alive[c] = 1;
                break;
            }
        }
    }
    bricks_left = 1;
}

static void build_wall(void) {
    if (variation == VAR_PROGRESSIVE) build_progressive_wall();
    else build_static_wall();
    cavity_left_released = 0;
    cavity_right_released = 0;
}

/* ---------------------------------------------------------------------- */
static void clamp_paddle(void) {
    double min_x = WALL_SIDE_W;
    double max_x = LOGICAL_W - WALL_SIDE_W - paddle_w;
    if (paddle_x < min_x) paddle_x = min_x;
    if (paddle_x > max_x) paddle_x = max_x;
}

static double base_paddle_width(void) {
    return beginner ? PADDLE_W_BEGINNER : PADDLE_W_EXPERT;
}

static void launch_ball(Ball *b, double origin_x) {
    double speed = BASE_SPEED * (variation == VAR_CHILDREN ? 0.72 : 1.0);
    double angle = (200.0 + frand() * 140.0) * (M_PI / 180.0);
    b->x = origin_x - BALL_SIZE / 2.0;
    b->y = PADDLE_Y - BALL_SIZE - 4.0;
    b->vx = cos(angle) * speed;
    b->vy = -fabs(sin(angle)) * speed;
    b->active = 1;
    b->captive = 0;
    b->hits = 0;
}

static void start_new_turn(void) {
    paddle_w = base_paddle_width();
    paddle_x = (LOGICAL_W - paddle_w) / 2.0;
    paddle_shrunk = 0;
    paddle_hits = 0;
    hit_stage = 0;
    ball2_spawned = 0;

    for (int i = 0; i < MAX_BALLS; i++) balls[i].active = 0;
    launch_ball(&balls[0], LOGICAL_W / 2.0);

    if (variation == VAR_CAVITY) {
        double bw_cell = (BRICK_RIGHT - BRICK_LEFT - (COLS - 1) * BRICK_GAP_X) / COLS;
        double lx0 = BRICK_LEFT + 2 * (bw_cell + BRICK_GAP_X);
        double lx1 = BRICK_LEFT + 4 * (bw_cell + BRICK_GAP_X) - BRICK_GAP_X;
        double ly0 = BRICK_TOP + 2 * ROW_STEP;
        double ly1 = BRICK_TOP + 4 * ROW_STEP - BRICK_GAP_Y;
        if (!cavity_left_released) {
            balls[1].captive = 1;
            balls[1].active = 1;
            balls[1].box_x0 = lx0; balls[1].box_x1 = lx1 - BALL_SIZE;
            balls[1].box_y0 = ly0; balls[1].box_y1 = ly1 - BALL_SIZE;
            balls[1].x = (lx0 + lx1) / 2.0 - BALL_SIZE / 2.0;
            balls[1].y = (ly0 + ly1) / 2.0 - BALL_SIZE / 2.0;
            balls[1].vx = 140.0; balls[1].vy = -140.0;
        } else {
            /* essa bola ja tinha sido libertada antes: continua livre no novo turno */
            launch_ball(&balls[1], LOGICAL_W / 2.0 - 80.0);
        }
        double rx0 = BRICK_LEFT + 9 * (bw_cell + BRICK_GAP_X);
        double rx1 = BRICK_LEFT + 11 * (bw_cell + BRICK_GAP_X) - BRICK_GAP_X;
        if (!cavity_right_released) {
            balls[2].captive = 1;
            balls[2].active = 1;
            balls[2].box_x0 = rx0; balls[2].box_x1 = rx1 - BALL_SIZE;
            balls[2].box_y0 = ly0; balls[2].box_y1 = ly1 - BALL_SIZE;
            balls[2].x = (rx0 + rx1) / 2.0 - BALL_SIZE / 2.0;
            balls[2].y = (ly0 + ly1) / 2.0 - BALL_SIZE / 2.0;
            balls[2].vx = -140.0; balls[2].vy = 140.0;
        } else {
            launch_ball(&balls[2], LOGICAL_W / 2.0 + 80.0);
        }
    }
}

static void new_game(void) {
    score = 0;
    turns_taken = 0;
    game_over = 0;
    game_over_timer = 0.0;
    paused = 0;
    end_sound_played = 0;
    wall_clear_pending = 0;
    wall_clear_timer = 0.0;
    build_wall();
    start_new_turn();
}

static void reset_wall_keep_score(void) {
    build_wall();
    start_new_turn();
    wall_clear_pending = 0;
    end_sound_played = 0;
}

static void change_variation(Variation v) {
    variation = v;
    new_game();
}

/* ---------------------------------------------------------------------- */
static unsigned long xcolor(XApp *ap, RGB c) {
    XColor xc;
    xc.red   = (unsigned short)(c.r / 255.0 * 65535.0);
    xc.green = (unsigned short)(c.g / 255.0 * 65535.0);
    xc.blue  = (unsigned short)(c.b / 255.0 * 65535.0);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(ap->dpy, ap->cmap, &xc);
    return xc.pixel;
}

static void logical_to_real(double scale, double ox, double oy,
                             double lx, double ly, double lw, double lh,
                             int *rx, int *ry, int *rw, int *rh) {
    *rx = (int)lround(ox + lx * scale);
    *ry = (int)lround(oy + ly * scale);
    *rw = (int)lround(lw * scale);
    *rh = (int)lround(lh * scale);
    if (*rw < 1) *rw = 1;
    if (*rh < 1) *rh = 1;
}

static void fill_rect(XApp *ap, double scale, double ox, double oy,
                       double x, double y, double w, double h, RGB color) {
    int rx, ry, rw, rh;
    logical_to_real(scale, ox, oy, x, y, w, h, &rx, &ry, &rw, &rh);
    XSetForeground(ap->dpy, ap->gc, xcolor(ap, color));
    XFillRectangle(ap->dpy, ap->back, ap->gc, rx, ry, (unsigned)rw, (unsigned)rh);
}

static void draw_digit(XApp *ap, double scale, double ox, double oy,
                        double x, double y, double size, int digit, RGB color) {
    static const unsigned char SEG[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    if (digit < 0 || digit > 9) return;
    unsigned char s = SEG[digit];
    double th = size * 0.16;
    double w = size * 0.62;
    double h = size;
    if (s & 0x01) fill_rect(ap, scale, ox, oy, x, y, w, th, color);
    if (s & 0x02) fill_rect(ap, scale, ox, oy, x + w - th, y, th, h/2, color);
    if (s & 0x04) fill_rect(ap, scale, ox, oy, x + w - th, y + h/2, th, h/2, color);
    if (s & 0x08) fill_rect(ap, scale, ox, oy, x, y + h - th, w, th, color);
    if (s & 0x10) fill_rect(ap, scale, ox, oy, x, y + h/2, th, h/2, color);
    if (s & 0x20) fill_rect(ap, scale, ox, oy, x, y, th, h/2, color);
    if (s & 0x40) fill_rect(ap, scale, ox, oy, x, y + h/2 - th/2, w, th, color);
}

static void draw_number(XApp *ap, double scale, double ox, double oy,
                         double x, double y, double size, int value, int min_digits,
                         RGB color) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)strlen(buf);
    int pad = min_digits - len;
    double gap = size * 0.22;
    double cursor = x;
    if (pad > 0) cursor += pad * (size * 0.62 + gap);
    for (int i = 0; i < len; i++) {
        draw_digit(ap, scale, ox, oy, cursor, y, size, buf[i] - '0', color);
        cursor += size * 0.62 + gap;
    }
}

static void ensure_backbuffer(XApp *ap) {
    XWindowAttributes wa;
    XGetWindowAttributes(ap->dpy, ap->win, &wa);
    if (wa.width == ap->win_w && wa.height == ap->win_h && ap->back != 0) return;
    if (ap->back) XFreePixmap(ap->dpy, ap->back);
    ap->win_w = wa.width;
    ap->win_h = wa.height;
    ap->back = XCreatePixmap(ap->dpy, ap->win, ap->win_w, ap->win_h,
                              DefaultDepth(ap->dpy, ap->screen));
}

static void render_frame(XApp *ap) {
    ensure_backbuffer(ap);
    double scale = fmin(ap->win_w / LOGICAL_W, ap->win_h / LOGICAL_H);
    double ox = (ap->win_w - LOGICAL_W * scale) / 2.0;
    double oy = (ap->win_h - LOGICAL_H * scale) / 2.0;

    RGB black = {0, 0, 0};
    RGB gray = {142, 142, 142};
    RGB green_edge = {26, 140, 60};
    RGB red_edge = {196, 44, 44};

    XSetForeground(ap->dpy, ap->gc, xcolor(ap, black));
    XFillRectangle(ap->dpy, ap->back, ap->gc, 0, 0, ap->win_w, ap->win_h);

    fill_rect(ap, scale, ox, oy, 0, WALL_TOP_Y0, LOGICAL_W, WALL_TOP_Y1 - WALL_TOP_Y0, gray);
    fill_rect(ap, scale, ox, oy, 0, WALL_TOP_Y0, WALL_SIDE_W, FIELD_BOTTOM - WALL_TOP_Y0, gray);
    fill_rect(ap, scale, ox, oy, LOGICAL_W - WALL_SIDE_W, WALL_TOP_Y0, WALL_SIDE_W, FIELD_BOTTOM - WALL_TOP_Y0, gray);
    fill_rect(ap, scale, ox, oy, 0, FIELD_BOTTOM - 20, WALL_SIDE_W, 20, green_edge);
    fill_rect(ap, scale, ox, oy, LOGICAL_W - WALL_SIDE_W, FIELD_BOTTOM - 20, WALL_SIDE_W, 20, red_edge);

    draw_number(ap, scale, ox, oy, 160, 40, 84, score, 4, gray);
    draw_number(ap, scale, ox, oy, 860, 40, 84, turns_taken, 1, gray);
    draw_number(ap, scale, ox, oy, 1280, 40, 84, (int)variation, 1, gray);

    if (variation == VAR_PROGRESSIVE) {
        double bw = (BRICK_RIGHT - BRICK_LEFT - (COLS - 1) * BRICK_GAP_X) / COLS;
        for (int i = 0; i < MAX_PROWS; i++) {
            if (!prows[i].used) continue;
            for (int c = 0; c < COLS; c++) {
                if (!prows[i].alive[c]) continue;
                double bx = BRICK_LEFT + c * (bw + BRICK_GAP_X);
                fill_rect(ap, scale, ox, oy, bx, prows[i].y, bw, BRICK_H, ROW_COLORS[prows[i].color_idx]);
            }
        }
    } else {
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                Brick *b = &bricks[r][c];
                if (!b->alive) continue;
                fill_rect(ap, scale, ox, oy, b->x, b->y, b->w, b->h, ROW_COLORS[b->color_idx]);
            }
    }

    fill_rect(ap, scale, ox, oy, paddle_x, PADDLE_Y, paddle_w, PADDLE_H, red_edge);
    if (variation == VAR_DOUBLE || variation == VAR_CAVITY)
        fill_rect(ap, scale, ox, oy, paddle_x, PADDLE_Y2, paddle_w, PADDLE_H, red_edge);

    for (int i = 0; i < MAX_BALLS; i++)
        if (balls[i].active)
            fill_rect(ap, scale, ox, oy, balls[i].x, balls[i].y, BALL_SIZE, BALL_SIZE, red_edge);

    if (paused && !game_over) {
        fill_rect(ap, scale, ox, oy, LOGICAL_W/2 - 60, LOGICAL_H/2 - 70, 40, 140, gray);
        fill_rect(ap, scale, ox, oy, LOGICAL_W/2 + 20, LOGICAL_H/2 - 70, 40, 140, gray);
    }

    XCopyArea(ap->dpy, ap->back, ap->win, ap->gc, 0, 0, ap->win_w, ap->win_h, 0, 0);
    XFlush(ap->dpy);
}

/* ---------------------------------------------------------------------- */
static void register_paddle_hit(void) {
    paddle_hits++;
    int new_stage = hit_stage;
    if (paddle_hits >= 48) new_stage = 3;
    else if (paddle_hits >= 16) new_stage = 2;
    else if (paddle_hits >= 8) new_stage = 1;
    if (new_stage != hit_stage && variation != VAR_CHILDREN) {
        hit_stage = new_stage;
        double bump = hit_stage == 1 ? 1.08 : hit_stage == 2 ? 1.06 : 1.10;
        for (int i = 0; i < MAX_BALLS; i++) {
            if (!balls[i].active || balls[i].captive) continue;
            double sp = hypot(balls[i].vx, balls[i].vy) * bump;
            double ang = atan2(balls[i].vy, balls[i].vx);
            balls[i].vx = cos(ang) * sp;
            balls[i].vy = sin(ang) * sp;
        }
    }
}

static int try_bounce_on_rect(Ball *b, double px, double py, double pw, double ph) {
    if (b->vy <= 0) return 0;
    if (b->y + BALL_SIZE < py) return 0;
    if (b->y + BALL_SIZE > py + ph + 24) return 0;
    if (b->x + BALL_SIZE < px || b->x > px + pw) return 0;
    double center = px + pw / 2.0;
    double hit = ((b->x + BALL_SIZE / 2.0) - center) / (pw / 2.0);
    if (hit < -1) hit = -1;
    if (hit > 1) hit = 1;
    double max_angle_deg = 50.0 + hit_stage * 5.0;
    double speed = hypot(b->vx, b->vy);
    double angle = hit * (max_angle_deg * M_PI / 180.0);
    b->vx = sin(angle) * speed;
    b->vy = -cos(angle) * speed;
    b->y = py - BALL_SIZE;
    b->hits++;
    register_paddle_hit();
    sfx_paddle();
    return 1;
}

static int collide_static_bricks(Ball *b) {
    double bx0 = b->x, bx1 = b->x + BALL_SIZE;
    double by0 = b->y, by1 = b->y + BALL_SIZE;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            Brick *br = &bricks[r][c];
            if (!br->alive) continue;
            if (bx1 < br->x || bx0 > br->x + br->w || by1 < br->y || by0 > br->y + br->h) continue;
            br->alive = 0;
            bricks_left--;
            add_score(br->points);
            sfx_brick(r);

            double overlap_left   = bx1 - br->x;
            double overlap_right  = (br->x + br->w) - bx0;
            double overlap_top    = by1 - br->y;
            double overlap_bottom = (br->y + br->h) - by0;
            double min_overlap = fmin(fmin(overlap_left, overlap_right), fmin(overlap_top, overlap_bottom));
            if (min_overlap == overlap_left || min_overlap == overlap_right) b->vx = -b->vx;
            else b->vy = -b->vy;

            if (variation != VAR_CHILDREN && r >= ROWS - 4) {
                double sp = hypot(b->vx, b->vy) * 1.05;
                double ang = atan2(b->vy, b->vx);
                b->vx = cos(ang) * sp;
                b->vy = sin(ang) * sp;
            }

            if (variation == VAR_CAVITY) {
                int left_clear = 1, right_clear = 1;
                for (int rr = 2; rr <= 3 && left_clear; rr++)
                    for (int cc = 0; cc <= 5; cc++)
                        if (bricks[rr][cc].alive) { left_clear = 0; break; }
                for (int rr = 2; rr <= 3 && right_clear; rr++)
                    for (int cc = 7; cc <= 12; cc++)
                        if (bricks[rr][cc].alive) { right_clear = 0; break; }
                if (left_clear && !cavity_left_released) {
                    cavity_left_released = 1;
                    balls[1].captive = 0;
                    balls[1].vx = (frand() - 0.5) * 200.0;
                    balls[1].vy = -BASE_SPEED * 0.85;
                    sfx_release();
                }
                if (right_clear && !cavity_right_released) {
                    cavity_right_released = 1;
                    balls[2].captive = 0;
                    balls[2].vx = (frand() - 0.5) * 200.0;
                    balls[2].vy = -BASE_SPEED * 0.85;
                    sfx_release();
                }
            }
            return 1;
        }
    }
    return 0;
}

static int collide_progressive_bricks(Ball *b) {
    double bx0 = b->x, bx1 = b->x + BALL_SIZE;
    double by0 = b->y, by1 = b->y + BALL_SIZE;
    double bw = (BRICK_RIGHT - BRICK_LEFT - (COLS - 1) * BRICK_GAP_X) / COLS;
    for (int i = 0; i < MAX_PROWS; i++) {
        if (!prows[i].used) continue;
        PRow *row = &prows[i];
        if (by1 < row->y || by0 > row->y + BRICK_H) continue;
        for (int c = 0; c < COLS; c++) {
            if (!row->alive[c]) continue;
            double bxp = BRICK_LEFT + c * (bw + BRICK_GAP_X);
            if (bx1 < bxp || bx0 > bxp + bw) continue;
            row->alive[c] = 0;
            add_score(row->value);
            sfx_brick((row->value - 1) / 2);
            if (row->vanish_timer >= 0.0) { sfx_bonus(); add_score(15); }

            double overlap_left   = bx1 - bxp;
            double overlap_right  = (bxp + bw) - bx0;
            double overlap_top    = by1 - row->y;
            double overlap_bottom = (row->y + BRICK_H) - by0;
            double min_overlap = fmin(fmin(overlap_left, overlap_right), fmin(overlap_top, overlap_bottom));
            if (min_overlap == overlap_left || min_overlap == overlap_right) b->vx = -b->vx;
            else b->vy = -b->vy;
            return 1;
        }
    }
    return 0;
}

static void update_ball_common(Ball *b, double dt) {
    b->x += b->vx * dt;
    b->y += b->vy * dt;

    if (b->x <= WALL_SIDE_W) { b->x = WALL_SIDE_W; b->vx = fabs(b->vx); sfx_wall(); }
    if (b->x + BALL_SIZE >= LOGICAL_W - WALL_SIDE_W) {
        b->x = LOGICAL_W - WALL_SIDE_W - BALL_SIZE; b->vx = -fabs(b->vx); sfx_wall();
    }
    if (b->y <= WALL_TOP_Y1) {
        b->y = WALL_TOP_Y1; b->vy = fabs(b->vy); sfx_wall();
        if (!paddle_shrunk && variation != VAR_CHILDREN) {
            paddle_shrunk = 1;
            paddle_w = base_paddle_width() / 2.0;
            clamp_paddle();
            sfx_shrink();
        }
    }

    try_bounce_on_rect(b, paddle_x, PADDLE_Y, paddle_w, PADDLE_H);
    if (variation == VAR_DOUBLE || variation == VAR_CAVITY)
        try_bounce_on_rect(b, paddle_x, PADDLE_Y2, paddle_w, PADDLE_H);

    if (variation == VAR_PROGRESSIVE) collide_progressive_bricks(b);
    else collide_static_bricks(b);
}

static void update_captive_ball(Ball *b, double dt) {
    b->x += b->vx * dt;
    b->y += b->vy * dt;
    if (b->x < b->box_x0) { b->x = b->box_x0; b->vx = fabs(b->vx); }
    if (b->x > b->box_x1) { b->x = b->box_x1; b->vx = -fabs(b->vx); }
    if (b->y < b->box_y0) { b->y = b->box_y0; b->vy = fabs(b->vy); }
    if (b->y > b->box_y1) { b->y = b->box_y1; b->vy = -fabs(b->vy); }
}

static void update_progressive_scroll(double dt) {
    prog_scroll_speed += dt * 0.9;
    double dy = prog_scroll_speed * dt;
    prog_spawn_accum += dy;

    for (int i = 0; i < MAX_PROWS; i++) {
        if (!prows[i].used) continue;
        prows[i].y += dy;
        double bottom_zone_y = PADDLE_Y - 60.0;
        if (prows[i].y + BRICK_H >= bottom_zone_y && prows[i].vanish_timer < 0.0)
            prows[i].vanish_timer = 0.0;
        if (prows[i].vanish_timer >= 0.0) {
            prows[i].vanish_timer += dt;
            if (prows[i].vanish_timer >= 1.2) {
                for (int c = 0; c < COLS; c++) prows[i].alive[c] = 0;
            }
        }
        if (prows[i].y > FIELD_BOTTOM) prows[i].used = 0;
    }
    while (prog_spawn_accum >= ROW_STEP) {
        prog_spawn_accum -= ROW_STEP;
        spawn_progressive_row(prog_next_idx);
        prog_next_idx++;
    }
}

static void handle_turn_end_check(void) {
    if (count_free_active_balls() > 0) return;
    turns_taken++;
    if (turns_taken >= TURNS_LIMIT) {
        game_over = 1;
    } else {
        sfx_life_lost();
        start_new_turn();
    }
}

static void update(double dt) {
    if (game_over) {
        if (!end_sound_played) { sfx_game_over(); end_sound_played = 1; }
        game_over_timer += dt;
        if (game_over_timer >= 2.5) new_game(); /* reinicia sozinho: placar, turnos e parede do zero */
        return;
    }
    if (wall_clear_pending) {
        if (!end_sound_played) { sfx_win(); end_sound_played = 1; }
        wall_clear_timer += dt;
        if (wall_clear_timer >= 1.8) reset_wall_keep_score();
        return;
    }
    if (paused) return;

    for (int i = 0; i < MAX_BALLS; i++) {
        if (!balls[i].active) continue;
        if (balls[i].captive) update_captive_ball(&balls[i], dt);
        else update_ball_common(&balls[i], dt);
    }

    if (variation == VAR_DOUBLE && !ball2_spawned && balls[0].hits >= 1) {
        launch_ball(&balls[1], LOGICAL_W / 2.0 + 60.0);
        ball2_spawned = 1;
    }

    if (variation == VAR_PROGRESSIVE) update_progressive_scroll(dt);

    for (int i = 0; i < MAX_BALLS; i++)
        if (balls[i].active && !balls[i].captive && balls[i].y > FIELD_BOTTOM)
            balls[i].active = 0;

    handle_turn_end_check();

    if (variation != VAR_PROGRESSIVE && bricks_left <= 0 && !wall_clear_pending) {
        wall_clear_pending = 1;
        wall_clear_timer = 0.0;
        end_sound_played = 0;
    }
}

/* ---------------------------------------------------------------------- */
static void toggle_fullscreen(XApp *ap) {
    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type = ClientMessage;
    xev.xclient.window = ap->win;
    xev.xclient.message_type = ap->net_wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 2;
    xev.xclient.data.l[1] = (long)ap->net_wm_state_fullscreen;
    xev.xclient.data.l[2] = 0;
    xev.xclient.data.l[3] = 1;
    XSendEvent(ap->dpy, ap->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    ap->fullscreen = !ap->fullscreen;
    XFlush(ap->dpy);
}

static void hide_cursor(XApp *ap) {
    char data[1] = {0};
    Pixmap blank = XCreateBitmapFromData(ap->dpy, ap->win, data, 1, 1);
    XColor black;
    memset(&black, 0, sizeof(black));
    Cursor invisible = XCreatePixmapCursor(ap->dpy, blank, blank, &black, &black, 0, 0);
    XDefineCursor(ap->dpy, ap->win, invisible);
    XFreeCursor(ap->dpy, invisible);
    XFreePixmap(ap->dpy, blank);
}

static void sync_paddle_to_pointer(XApp *ap) {
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(ap->dpy, ap->win, &root_ret, &child_ret,
                        &root_x, &root_y, &win_x, &win_y, &mask))
        return; /* ponteiro fora da tela/janela: mantem posicao atual */
    double scale = fmin(ap->win_w / LOGICAL_W, ap->win_h / LOGICAL_H);
    double ox = (ap->win_w - LOGICAL_W * scale) / 2.0;
    double lx = (win_x - ox) / scale;
    paddle_x = lx - paddle_w / 2.0;
    clamp_paddle();
}

/* ---------------------------------------------------------------------- */
int main(void) {
    srand((unsigned)time(NULL));

    XApp ap;
    memset(&ap, 0, sizeof(ap));
    ap.dpy = XOpenDisplay(NULL);
    if (!ap.dpy) {
        fprintf(stderr, "Nao foi possivel conectar ao servidor X (sem DISPLAY?).\n");
        return 1;
    }
    ap.screen = DefaultScreen(ap.dpy);
    ap.root = RootWindow(ap.dpy, ap.screen);
    ap.cmap = DefaultColormap(ap.dpy, ap.screen);

    int init_w = 1140, init_h = 810;

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = BlackPixel(ap.dpy, ap.screen);
    swa.event_mask = KeyPressMask | PointerMotionMask | ExposureMask | StructureNotifyMask;

    ap.win = XCreateWindow(ap.dpy, ap.root, 0, 0, init_w, init_h, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWEventMask, &swa);

    XStoreName(ap.dpy, ap.win, "Super Breakout");

    ap.wm_delete = XInternAtom(ap.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ap.dpy, ap.win, &ap.wm_delete, 1);
    ap.net_wm_state = XInternAtom(ap.dpy, "_NET_WM_STATE", False);
    ap.net_wm_state_fullscreen = XInternAtom(ap.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    ap.fullscreen = 0;

    ap.gc = XCreateGC(ap.dpy, ap.win, 0, NULL);
    ap.back = 0;
    ap.win_w = init_w;
    ap.win_h = init_h;

    XMapWindow(ap.dpy, ap.win);
    XFlush(ap.dpy);
    hide_cursor(&ap);

    init_audio();
    new_game();

    struct timespec prev, now;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    int running = 1;
    while (running) {
        while (XPending(ap.dpy)) {
            XEvent ev;
            XNextEvent(ap.dpy, &ev);
            switch (ev.type) {
                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == ap.wm_delete) running = 0;
                    break;
                case ConfigureNotify:
                    break;
                case MotionNotify: {
                    double scale = fmin(ap.win_w / LOGICAL_W, ap.win_h / LOGICAL_H);
                    double ox = (ap.win_w - LOGICAL_W * scale) / 2.0;
                    double lx = (ev.xmotion.x - ox) / scale;
                    paddle_x = lx - paddle_w / 2.0;
                    clamp_paddle();
                    break;
                }
                case KeyPress: {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Escape) {
                        running = 0;
                    } else if (ks == XK_r || ks == XK_R) {
                        new_game();
                    } else if (ks == XK_p || ks == XK_P) {
                        if (!game_over) paused = !paused;
                    } else if (ks == XK_b || ks == XK_B) {
                        beginner = !beginner;
                        paddle_w = paddle_shrunk ? base_paddle_width() / 2.0 : base_paddle_width();
                        clamp_paddle();
                    } else if (ks == XK_g || ks == XK_G) {
                        int idx = 0;
                        for (int i = 0; i < 5; i++) if (VARIATION_ORDER[i] == (int)variation) idx = i;
                        idx = (idx + 1) % 5;
                        change_variation((Variation)VARIATION_ORDER[idx]);
                    } else if ((ks == XK_f || ks == XK_F) && (ev.xkey.state & ControlMask)) {
                        toggle_fullscreen(&ap);
                    } else if (ks == XK_space && wall_clear_pending) {
                        reset_wall_keep_score();
                    }
                    break;
                }
                default:
                    break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - prev.tv_sec) + (now.tv_nsec - prev.tv_nsec) / 1e9;
        if (dt > 0.033) dt = 0.033;
        prev = now;

        sync_paddle_to_pointer(&ap); /* raquete sempre segue o mouse de verdade, todo quadro */
        update(dt);
        render_frame(&ap);

        struct timespec ts = {0, 16 * 1000000L};
        nanosleep(&ts, NULL);
    }

    cleanup_audio();
    if (ap.back) XFreePixmap(ap.dpy, ap.back);
    XFreeGC(ap.dpy, ap.gc);
    XDestroyWindow(ap.dpy, ap.win);
    XCloseDisplay(ap.dpy);
    return 0;
}
