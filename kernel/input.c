// =============================================================================
//  kernel/input.c -- File circulaire d'événements d'entrée
// =============================================================================
#include "input.h"
#include "io.h"
#include "sched.h"

#define QUEUE_SIZE 256

static event_t queue[QUEUE_SIZE];
static volatile uint32_t head, tail;
static task_t *waiter;            // compositeur à réveiller quand un événement arrive

void input_init(void) { head = tail = 0; }

void input_set_waiter(struct task *t) { waiter = (task_t *)t; }
bool input_pending(void) { return head != tail; }

// Appelé depuis les ISR clavier/souris : on désactive brièvement les
// interruptions pour protéger les indices.
void input_push(const event_t *e) {
    uint32_t next = (head + 1) % QUEUE_SIZE;
    if (next == tail) return;       // file pleine : on jette l'événement
    queue[head] = *e;
    head = next;
    if (waiter) sched_wake(waiter); // réveille le compositeur s'il dort
}

bool input_poll(event_t *out) {
    if (tail == head) return false;
    *out = queue[tail];
    tail = (tail + 1) % QUEUE_SIZE;
    return true;
}
