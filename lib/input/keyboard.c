#include "input/keyboard.h"

#define KEYBOARD_QUEUE_CAPACITY 64u
#define KEYBOARD_QUEUE_MASK (KEYBOARD_QUEUE_CAPACITY - 1u)

_Static_assert((KEYBOARD_QUEUE_CAPACITY & KEYBOARD_QUEUE_MASK) == 0,
               "keyboard queue size must be a power of two");

static struct key_event queue[KEYBOARD_QUEUE_CAPACITY];
static volatile uint8_t queue_head;
static volatile uint8_t queue_tail;
static volatile uint32_t dropped_events;

int keyboard_event_push(const struct key_event *event)
{
    if (!event)
        return 0;

    uint8_t head = queue_head;
    uint8_t next = (uint8_t)((head + 1u) & KEYBOARD_QUEUE_MASK);
    if (next == queue_tail) {
        dropped_events++;
        return 0;
    }

    queue[head] = *event;
    __asm__ volatile ("" ::: "memory");
    queue_head = next;
    return 1;
}

int keyboard_event_pop(struct key_event *event)
{
    if (!event)
        return 0;

    uint8_t tail = queue_tail;
    if (tail == queue_head)
        return 0;

    __asm__ volatile ("" ::: "memory");
    *event = queue[tail];
    queue_tail = (uint8_t)((tail + 1u) & KEYBOARD_QUEUE_MASK);
    return 1;
}

uint32_t keyboard_event_dropped(void)
{
    return dropped_events;
}
