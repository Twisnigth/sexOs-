// =============================================================================
//  user/lib/libwin.c -- Client du compositeur : crée une fenêtre en mémoire
//  partagée, reçoit les événements, signale les dommages. (Ring 3, via IPC.)
// =============================================================================
#include "libwin.h"
#include "monos.h"
#include "wproto.h"

void *memset(void *, int, unsigned long);

static int      comp_pid;
static int      my_win, my_shm;
static canvas_t wcanvas;

static void spin(int n) { for (volatile int i = 0; i < n; i++) {} }

static void wait_compositor(void) {
    while (!(comp_pid = sys_comp_pid())) spin(200000);
}

canvas_t *win_create(int w, int h, const char *title) {
    wait_compositor();
    wmsg_t m; memset(&m, 0, sizeof m);
    m.type = WMSG_CREATE; m.w = w; m.h = h;
    int i = 0; while (title[i] && i < 31) { m.title[i] = title[i]; i++; } m.title[i] = 0;
    sys_ipc_send(comp_pid, &m, sizeof m);

    for (;;) {                                   // attend la réponse WMSG_CREATED
        wmsg_t r; int s;
        if (sys_ipc_recv(&r, sizeof r, &s) > 0 && r.type == WMSG_CREATED) {
            my_win = r.win; my_shm = r.shm;
            uint64_t va = 0;
            if (sys_shm_map(my_shm, &va) != 0) return 0;
            wcanvas.pixels = (uint32_t *)(uintptr_t)va;
            wcanvas.width = (uint32_t)w; wcanvas.height = (uint32_t)h; wcanvas.pitch = (uint32_t)w * 4;
            return &wcanvas;
        }
    }
}

void win_damage(void) {
    wmsg_t m; memset(&m, 0, sizeof m);
    m.type = WMSG_DAMAGE; m.win = my_win;
    sys_ipc_send(comp_pid, &m, sizeof m);
}

int win_poll(event_t *ev) {
    wmsg_t r; int s;
    int n = sys_ipc_recv(&r, sizeof r, &s);
    if (n <= 0) return 0;
    if (r.type == WMSG_CLOSE) return -1;
    if (r.type == WMSG_EVENT) { *ev = r.ev; return 1; }
    return 0;
}
