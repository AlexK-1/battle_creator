// #include <stdlib.h>

// typedef struct {
//     int *items;
//     int front, rear, size;
//     size_t max_len;
// } Queue;

// static inline void clear_queue(Queue *q) {
//     q->front = 0;
//     q->rear = -1;
//     q->size = 0;
// }

// static inline void *init_queue(Queue *q, size_t len) {
//     clear_queue(q);
//     q->items = (int*)calloc(len, sizeof(*q->items));
//     return q->items;
// }

// static inline void enqueue(Queue *q, int value) {
//     q->rear = (q->rear + 1) % q->max_len;
//     q->items[q->rear] = value;
//     q->size++;
// }

// static inline void dequeue(Queue *q, int *value) {
//     *value = q->items[q->front];
//     q->front = (q->front + 1) % q->max_len;
//     q->size--;
// }

#define clear_queue(q)                                   \
    (q).front = 0;                                       \
    (q).rear = -1;                                       \
    (q).size = 0;                                        \

#define init_queue(q, len)                               \
    clear_queue(q);                                      \
    (q).max_len = len;                                   \
    (q).items = calloc(len, sizeof(*(q).items));         \

#define is_queue_empty(q) ((q).size == 0)

#define is_queue_full(q) ((q).size == (q).max_len)

#define queue_front(q) ((q).items[(q).front])

#define enqueue(q, value)                                \
    (q).rear = ((q).rear + 1) % (q).max_len;             \
    (q).items[(q).rear] = (value);                       \
    (q).size++;

#define dequeue(q, value)                                \
    (value) = (q).items[(q).front];                     \
    (q).front = ((q).front + 1) % (q).max_len;           \
    (q).size--;
