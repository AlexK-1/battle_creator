// #include <stdlib.h>

#ifndef QUEUE_H
#define QUEUE_H

/* QUEUE
Example:
struct ExampleQueue {
    int *items;
    int front, rear, size, max_len;
};
*/

#define clear_queue(q)                                   \
    do {                                                 \
        (q).front = 0;                                   \
        (q).rear = -1;                                   \
        (q).size = 0;                                    \
    } while (0)

#define init_queue(q, len)                               \
    do {                                                 \
        clear_queue(q);                                  \
        (q).max_len = len;                               \
        (q).items = calloc(len, sizeof(*(q).items));     \
    } while (0)

#define is_queue_empty(q) ((q).size == 0)

#define is_queue_full(q) ((q).size == (q).max_len)

#define queue_front(q) ((q).items[(q).front])

#define enqueue(q, value)                                \
    do {                                                 \
        (q).rear = ((q).rear + 1) % (q).max_len;         \
        (q).items[(q).rear] = (value);                   \
        (q).size++;                                      \
    } while (0)

#define dequeue(q, value)                                \
    do {                                                 \
        (value) = (q).items[(q).front];                  \
        (q).front = ((q).front + 1) % (q).max_len;       \
        (q).size--;                                      \
    } while (0)

/* CYCLIC STACK
Example:
struct ExampleCStack {
    int *items;
    int front, rear, size, max_len;
};
*/

#define clear_cstack(s)                                  \
    do {                                                 \
        (s).front = -1;                                  \
        (s).rear = 0;                                    \
        (s).size = 0;                                    \
    } while (0)

#define init_cstack(s, len)                              \
    do {                                                 \
        clear_cstack(s);                                 \
        (s).max_len = len;                               \
        (s).items = calloc(len, sizeof(*(s).items));     \
    } while (0)

#define is_cstack_empty(s) ((s).size == 0)

#define is_cstack_full(s) ((s).size == (s).max_len)

#define cstack_push(s, value)                            \
    do {                                                 \
        if (is_cstack_full(s)) {                         \
            (s).front = (s).rear;                        \
            (s).rear = ((s).rear + 1) % (s).max_len;     \
            (s).items[(s).front] = (value);              \
        } else {                                         \
            (s).front = ((s).front + 1) % (s).max_len;   \
            (s).items[(s).front] = (value);              \
            (s).size++;                                  \
        }                                                \
    } while (0)

#endif // QUEUE_H
