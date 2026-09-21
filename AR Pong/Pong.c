#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#define PADDLE_W 15
#define PADDLE_H 90
#define BALL_SIZE 15
#define MOUSE_SENSITIVITY 35
#define FPS 60

int win_w = 800;
int win_h = 500;

unsigned char numeros[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x1, 0x1, 0x1, 0x1, 0x1}, 
    {0x7, 0x1, 0x7, 0x4, 0x7}, {0x7, 0x1, 0x7, 0x1, 0x7},
    {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1}, 
    {0x7, 0x5, 0x7, 0x5, 0x7}, {0x7, 0x5, 0x7, 0x1, 0x7}
};

void som_impacto(int freq) {
    char cmd[128];
    sprintf(cmd, "head -c 100 /dev/urandom | aplay -q -r %d -f U8 2>/dev/null &", freq);
    system(cmd);
}

void toggle_fullscreen(Display* dpy, Window win) {
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = win;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 2; 
    xev.xclient.data.l[1] = fullscreen;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask | SubstructureRedirectMask, &xev);
}

void calcular_colisao(float *ball_dx, float *ball_dy, float ball_y, float paddle_y, float v_total, float direcao) {
    float impacto = ((ball_y + BALL_SIZE/2.0f) - (paddle_y + PADDLE_H/2.0f)) / (PADDLE_H/2.0f);
    *ball_dy = impacto * (v_total * 0.85f); 
    *ball_dx = direcao * sqrtf(fmaxf(1.0f, (v_total * v_total) - ((*ball_dy) * (*ball_dy))));
    som_impacto(5000);
}

void desenhar_placar(Display *d, Drawable dr, GC gc, int x, int y, int n) {
    int escala = 10, espaco = 45;
    int digs[] = {(n / 10) % 10, n % 10};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 3; j++)
                if ((numeros[digs[k]][i] >> (2 - j)) & 1)
                    XFillRectangle(d, dr, gc, x + (k * espaco) + (j * escala), y + (i * escala), escala, escala);
}

int main() {
    srand(time(NULL));
    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;

    int sc = DefaultScreen(display);
    Window win = XCreateSimpleWindow(display, RootWindow(display, sc), 0, 0, win_w, win_h, 1, WhitePixel(display, sc), BlackPixel(display, sc));
    XSelectInput(display, win, ExposureMask | ButtonPressMask | StructureNotifyMask | KeyPressMask);
    XStoreName(display, win, "Pong");
    XMapWindow(display, win);

    GC gc = XCreateGC(display, win, 0, NULL);
    unsigned long branco = WhitePixel(display, sc), preto = BlackPixel(display, sc);

    float v_total = 8.0f; 
    float ball_x = 400, ball_y = 250, ball_dx = 8.0f, ball_dy = 0.0f;
    float p1_y = 200, p2_y = 200;
    int score1 = 0, score2 = 0, pausado = 0, nivel = 1;
    
    // Variável para fixar o erro da IA e parar a "dança"
    float ia_offset = 0;

    Pixmap buffer = XCreatePixmap(display, win, win_w, win_h, DefaultDepth(display, sc));

    while (1) {
        while (XPending(display)) {
            XEvent ev; XNextEvent(display, &ev);
            if (ev.type == ConfigureNotify) {
                win_w = ev.xconfigure.width; win_h = ev.xconfigure.height;
                XFreePixmap(display, buffer);
                buffer = XCreatePixmap(display, win, win_w, win_h, DefaultDepth(display, sc));
            }
            if (ev.type == KeyPress) {
                KeySym k = XLookupKeysym(&ev.xkey, 0);
                if (k == XK_1) { nivel=1; v_total=8.0f;  score1=score2=0; ball_dx=v_total; XStoreName(display, win, "Pong Nivel 1"); }
                if (k == XK_2) { nivel=2; v_total=11.0f; score1=score2=0; ball_dx=v_total; XStoreName(display, win, "Pong Nivel 2"); }
                if (k == XK_3) { nivel=3; v_total=15.0f; score1=score2=0; ball_dx=v_total; XStoreName(display, win, "Pong Nivel 3"); }
                if (k == XK_p || k == XK_P) pausado = !pausado;
                if (k == XK_r || k == XK_R) { score1=0; score2=0; ball_x=win_w/2; ball_y=win_h/2; ball_dx=v_total; ball_dy=0; }
                if ((k == XK_f || k == XK_F) && (ev.xkey.state & ControlMask)) toggle_fullscreen(display, win);
            }
            if (ev.type == ButtonPress && !pausado) {
                if (ev.xbutton.button == 4) p1_y -= MOUSE_SENSITIVITY;
                if (ev.xbutton.button == 5) p1_y += MOUSE_SENSITIVITY;
            }
        }

        if (!pausado) {
            // Lógica da IA Estabilizada
            float centro_ia = p2_y + (PADDLE_H / 2.0f);
            
            if (ball_dx > 0) {
                // A IA só recalcula o erro quando a bola começa a vir para ela
                // Isso evita o tremor (dança) constante
                float alvo_y = ball_y + (BALL_SIZE / 2.0f) + ia_offset;
                float diff_y = alvo_y - centro_ia;
                float max_vel_ia = (nivel == 3) ? v_total * 0.95f : v_total * 0.70f;

                if (fabs(diff_y) > 2.0f) { // Margem morta para evitar micro-ajustes
                    p2_y += (diff_y > 0 ? 1 : -1) * fminf(fabs(diff_y), max_vel_ia);
                }
            } else {
                // Quando a bola vai para o Player 1, a IA reseta o erro para a próxima jogada
                float erro_max = (nivel == 1) ? 45.0f : (nivel == 2 ? 25.0f : 5.0f);
                ia_offset = ((rand() % 100) / 100.0f - 0.5f) * erro_max;
                
                // IA volta para o centro suavemente
                p2_y += ((win_h/2 - PADDLE_H/2) - p2_y) * 0.02f;
            }

            ball_x += ball_dx; ball_y += ball_dy;

            // Paredes
            if (ball_y <= 0) { ball_y = 0; ball_dy = fabsf(ball_dy); som_impacto(8000); }
            if (ball_y >= win_h - BALL_SIZE) { ball_y = win_h - BALL_SIZE; ball_dy = -fabsf(ball_dy); som_impacto(8000); }

            // Colisões
            if (ball_dx < 0 && ball_x <= 55 && ball_x >= 40 && ball_y + BALL_SIZE >= p1_y && ball_y <= p1_y + PADDLE_H) {
                ball_x = 56; calcular_colisao(&ball_dx, &ball_dy, ball_y, p1_y, v_total, 1.0f);
            }
            if (ball_dx > 0 && ball_x >= win_w - 55 - BALL_SIZE && ball_x <= win_w - 40 && ball_y + BALL_SIZE >= p2_y && ball_y <= p2_y + PADDLE_H) {
                ball_x = win_w - 56 - BALL_SIZE; calcular_colisao(&ball_dx, &ball_dy, ball_y, p2_y, v_total, -1.0f);
            }

            // Gols
            if (ball_x < 0 || ball_x > win_w) {
                if (ball_x < 0) score2++; else score1++;
                som_impacto(2000);
                ball_x = win_w/2; ball_y = win_h/2;
                ball_dx = (ball_dx < 0) ? v_total : -v_total; ball_dy = (rand()%6)-3; 
            }

            if (p1_y < 0) p1_y = 0; if (p1_y > win_h - PADDLE_H) p1_y = win_h - PADDLE_H;
            if (p2_y < 0) p2_y = 0; if (p2_y > win_h - PADDLE_H) p2_y = win_h - PADDLE_H;
        }

        // Renderização
        XSetForeground(display, gc, preto);
        XFillRectangle(display, buffer, gc, 0, 0, win_w, win_h);
        XSetForeground(display, gc, branco);
        
        for(int i = 0; i < win_h; i += 40) 
            XFillRectangle(display, buffer, gc, win_w/2 - 2, i, 4, 20);
        
        desenhar_placar(display, buffer, gc, win_w/4 - 40, 40, score1);
        desenhar_placar(display, buffer, gc, (3*win_w)/4 - 40, 40, score2);
        XFillRectangle(display, buffer, gc, 40, (int)p1_y, PADDLE_W, PADDLE_H);
        XFillRectangle(display, buffer, gc, win_w - 40 - PADDLE_W, (int)p2_y, PADDLE_W, PADDLE_H);
        XFillRectangle(display, buffer, gc, (int)ball_x, (int)ball_y, BALL_SIZE, BALL_SIZE);
        
        XCopyArea(display, buffer, win, gc, 0, 0, win_w, win_h, 0, 0);
        XFlush(display);
        usleep(1000000 / FPS);
    }
    return 0;
}
