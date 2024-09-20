#ifndef _ringbuffer_h
#define _ringbuffer_h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include <limits.h>  // for uint_max
#include <stdint.h>
#include <unistd.h>

typedef struct ringbuffer_s buffer_t;

buffer_t * buffer_new(uint32_t sz);

uint32_t buffer_len(buffer_t *r);


void buffer_free(buffer_t *r);

int buffer_add(buffer_t *r, const void *data, uint32_t sz);

int buffer_remove(buffer_t *r, void *data, uint32_t sz);

int buffer_drain(buffer_t *r, uint32_t sz);

int buffer_search(buffer_t *r, const char* sep, const int seplen);

uint8_t * buffer_write_atmost(buffer_t *r);

#endif
