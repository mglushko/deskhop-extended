#pragma once
/* Stand-in for the SDK's queue. Nothing here runs one - the tests that reach a queue stub
   the try_ functions - so this is only the type device_t is built out of. */

#include <stdbool.h>
#include <stdint.h>

typedef struct { void *data; uint16_t wptr, rptr; } queue_t;

bool queue_try_add(queue_t *queue, const void *value);
bool queue_try_peek(queue_t *queue, void *value);
bool queue_try_remove(queue_t *queue, void *value);
