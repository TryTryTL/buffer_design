
#include <string.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "buffer.h"


struct buf_chain_s {
    struct buf_chain_s *next;
    uint32_t buffer_len;
    uint32_t misalign;
    uint32_t off;
    uint8_t *buffer;
};

struct buffer_s {
    buf_chain_t *first;
    buf_chain_t *last;
    buf_chain_t **last_with_datap;
    uint32_t total_len;
    uint32_t last_read_pos; // for sep read
};

#define CHAIN_SPACE_LEN(ch) ((ch)->buffer_len - ((ch)->misalign + (ch)->off))
#define MIN_BUFFER_SIZE 1024
#define MAX_TO_COPY_IN_EXPAND 4096
#define BUFFER_CHAIN_MAX_AUTO_SIZE 4096
#define MAX_TO_REALIGN_IN_EXPAND 2048
#define BUFFER_CHAIN_MAX 16*1024*1024  // 16M
#define BUFFER_CHAIN_EXTRA(t, c) (t *)((buf_chain_t *)(c) + 1)
#define BUFFER_CHAIN_SIZE sizeof(buf_chain_t)

uint32_t
buffer_len(buffer_t *buf) {
    return buf->total_len;
}

buffer_t *
buffer_new(uint32_t sz) {
    (void)sz;
    buffer_t * buf = (buffer_t *) malloc(sizeof(buffer_t));
    if (!buf) {
        return NULL;
    }
    memset(buf, 0, sizeof(*buf));
    buf->last_with_datap = &buf->first;
    return buf;
}

static buf_chain_t *
buf_chain_new(uint32_t size) {
    buf_chain_t *chain;
    uint32_t to_alloc;
    if (size > BUFFER_CHAIN_MAX - BUFFER_CHAIN_SIZE)
        return (NULL);
    size += BUFFER_CHAIN_SIZE;
    
    if (size < BUFFER_CHAIN_MAX / 2) {
        to_alloc = MIN_BUFFER_SIZE;
        while (to_alloc < size) {
            to_alloc <<= 1;
        }
    } else {
        to_alloc = size;
    }
    if ((chain = malloc(to_alloc)) == NULL)
        return (NULL);
    memset(chain, 0, BUFFER_CHAIN_SIZE);
    chain->buffer_len = to_alloc - BUFFER_CHAIN_SIZE;
    chain->buffer = BUFFER_CHAIN_EXTRA(uint8_t, chain);
    return (chain);
}

static void 
buf_chain_free_all(buf_chain_t *chain) {
    buf_chain_t *next;
    for (; chain; chain = next) {
        next = chain->next;
        free(chain);
    }
}

void
buffer_free(buffer_t *buf) {
    buf_chain_free_all(buf->first);
}


static buf_chain_t **
free_empty_chains(buffer_t *buf) {
    buf_chain_t **ch = buf->last_with_datap;
    while ((*ch) && (*ch)->off != 0)
        ch = &(*ch)->next;
    if (*ch) {
        buf_chain_free_all(*ch);
        *ch = NULL;
    }
    return ch;
}

static void
buf_chain_insert(buffer_t *buf, buf_chain_t *chain) {
    if (*buf->last_with_datap == NULL) {
        buf->first = buf->last = chain;
    } else {
        buf_chain_t **chp;
        chp = free_empty_chains(buf);
        *chp = chain;
        if (chain->off)
            buf->last_with_datap = chp;
        buf->last = chain;
    }
    buf->total_len += chain->off;
}

static inline buf_chain_t *
buf_chain_insert_new(buffer_t *buf, uint32_t datlen) {
    buf_chain_t *chain;
    if ((chain = buf_chain_new(datlen)) == NULL)
        return NULL;
    buf_chain_insert(buf, chain);
    return chain;
}

static int
buf_chain_should_realign(buf_chain_t *chain, uint32_t datlen)
{
    return chain->buffer_len - chain->off >= datlen &&
        (chain->off < chain->buffer_len / 2) &&
        (chain->off <= MAX_TO_REALIGN_IN_EXPAND);
}

static void
buf_chain_align(buf_chain_t *chain) {
    memmove(chain->buffer, chain->buffer + chain->misalign, chain->off);
    chain->misalign = 0;
}

int buffer_add(buffer_t *buf, const void *data_in, uint32_t datlen) {
    buf_chain_t *chain, *tmp;
    const uint8_t *data = data_in;
    uint32_t remain, to_alloc;
    int result = -1;
    if (datlen > BUFFER_CHAIN_MAX - buf->total_len) {
        goto done;
    }

    if (*buf->last_with_datap == NULL) {
        chain = buf->last;
    } else {
        chain = *buf->last_with_datap;
    }

    if (chain == NULL) {
        chain = buf_chain_insert_new(buf, datlen);
        if (!chain)
            goto done;
    }

    remain = chain->buffer_len - chain->misalign - chain->off;
    if (remain >= datlen) {
        memcpy(chain->buffer + chain->misalign + chain->off, data, datlen);
        chain->off += datlen;
        buf->total_len += datlen;
        // buf->n_add_for_cb += datlen;
        goto out;
    } else if (buf_chain_should_realign(chain, datlen)) {
        buf_chain_align(chain);

        memcpy(chain->buffer + chain->off, data, datlen);
        chain->off += datlen;
        buf->total_len += datlen;
        // buf->n_add_for_cb += datlen;
        goto out;
    }
    to_alloc = chain->buffer_len;
    if (to_alloc <= BUFFER_CHAIN_MAX_AUTO_SIZE/2)
        to_alloc <<= 1;
    if (datlen > to_alloc)
        to_alloc = datlen;
    tmp = buf_chain_new(to_alloc);
    if (tmp == NULL)
        goto done;
    if (remain) {
        memcpy(chain->buffer + chain->misalign + chain->off, data, remain);
        chain->off += remain;
        buf->total_len += remain;
        // buf->n_add_for_cb += remain;
    }

    data += remain;
    datlen -= remain;

    memcpy(tmp->buffer, data, datlen);
    tmp->off = datlen;
    buf_chain_insert(buf, tmp);
    // buf->n_add_for_cb += datlen;
out:
    result = 0;
done:
    return result;
}

static uint32_t
buf_copyout(buffer_t *buf, void *data_out, uint32_t datlen) {
    buf_chain_t *chain;
    char *data = data_out;
    uint32_t nread;
    chain = buf->first;
    if (datlen > buf->total_len)
        datlen = buf->total_len;
    if (datlen == 0)
        return 0;
    nread = datlen;

    while (datlen && datlen >= chain->off) {
        uint32_t copylen = chain->off;
        memcpy(data,
            chain->buffer + chain->misalign,
            copylen);
        data += copylen;
        datlen -= copylen;

        chain = chain->next;
    }
    if (datlen) {
        memcpy(data, chain->buffer + chain->misalign, datlen);
    }

    return nread;
}

static inline void
ZERO_CHAIN(buffer_t *dst) {
    dst->first = NULL;
    dst->last = NULL;
    dst->last_with_datap = &(dst)->first;
    dst->total_len = 0;
}

int buffer_drain(buffer_t *buf, uint32_t len) {
    buf_chain_t *chain, *next;
    uint32_t remaining, old_len;
    old_len = buf->total_len;
    if (old_len == 0)
        return 0;

    if (len >= old_len) {
        len = old_len;
        for (chain = buf->first; chain != NULL; chain = next) {
            next = chain->next;
            free(chain);
        }
        ZERO_CHAIN(buf);
    } else {
        buf->total_len -= len;
        remaining = len;
        for (chain = buf->first; remaining >= chain->off; chain = next) {
            next = chain->next;
            remaining -= chain->off;

            if (chain == *buf->last_with_datap) {
                buf->last_with_datap = &buf->first;
            }
            if (&chain->next == buf->last_with_datap)
                buf->last_with_datap = &buf->first;

            free(chain);
        }

        buf->first = chain;
        chain->misalign += remaining;
        chain->off -= remaining;
    }
    
    // buf->n_del_for_cb += len;
    return len;
}

int buffer_remove(buffer_t *buf, void *data_out, uint32_t datlen) {
    uint32_t n = buf_copyout(buf, data_out, datlen);
    if (n > 0) {
        if (buffer_drain(buf, n) < 0)
            n = -1;
    }
    return (int)n;
}

static bool
check_sep(buf_chain_t * chain, int from, const char *sep, int seplen) {
    for (;;) {
        int sz = chain->off - from;
        if (sz >= seplen) {
            return memcmp(chain->buffer + chain->misalign + from, sep, seplen) == 0;
        }
        if (sz > 0) {
            if (memcmp(chain->buffer + chain->misalign + from, sep, sz)) {
                return false;
            }
        }
        chain = chain->next;
        sep += sz;
        seplen -= sz;
        from = 0;
    }
}

int buffer_search(buffer_t *buf, const char* sep, const int seplen) {
    buf_chain_t *chain;
    int i;
    chain = buf->first;
    if (chain == NULL)
        return 0;
    int bytes = chain->off;
    while (bytes <= buf->last_read_pos) {
        chain = chain->next;
        if (chain == NULL)
            return 0;
        bytes += chain->off;
    }
    bytes -= buf->last_read_pos;
    int from = chain->off - bytes;
    for (i = buf->last_read_pos; i <= buf->total_len - seplen; i++) {
        if (check_sep(chain, from, sep, seplen)) {
            buf->last_read_pos = 0;
            return i+seplen;
        }
        ++from;
        --bytes;
        if (bytes == 0) {
            chain = chain->next;
            from = 0;
            if (chain == NULL)
                break;
            bytes = chain->off;
        }
    }
    buf->last_read_pos = i;
    return 0;
}

uint8_t * buffer_write_atmost(buffer_t *p) {
    buf_chain_t *chain, *next, *tmp, *last_with_data;
    uint8_t *buffer;
    uint32_t remaining;
    int removed_last_with_data = 0;
    int removed_last_with_datap = 0;

    chain = p->first;
    uint32_t size = p->total_len;

    if (chain->off >= size) {
        return chain->buffer + chain->misalign;
    }

    remaining = size - chain->off;
    for (tmp=chain->next; tmp; tmp=tmp->next) {
        if (tmp->off >= (size_t)remaining)
            break;
        remaining -= tmp->off;
    }
    if (chain->buffer_len - chain->misalign >= (size_t)size) {
        /* already have enough space in the first chain */
        size_t old_off = chain->off;
        buffer = chain->buffer + chain->misalign + chain->off;
        tmp = chain;
        tmp->off = size;
        size -= old_off;
        chain = chain->next;
    } else {
        if ((tmp = buf_chain_new(size)) == NULL) {
            return NULL;
        }
        buffer = tmp->buffer;
        tmp->off = size;
        p->first = tmp;
    }

    last_with_data = *p->last_with_datap;
    for (; chain != NULL && (size_t)size >= chain->off; chain = next) {
        next = chain->next;

        if (chain->buffer) {
            memcpy(buffer, chain->buffer + chain->misalign, chain->off);
            size -= chain->off;
            buffer += chain->off;
        }
        if (chain == last_with_data)
            removed_last_with_data = 1;
        if (&chain->next == p->last_with_datap)
            removed_last_with_datap = 1;

        free(chain);
    }

    if (chain != NULL) {
        memcpy(buffer, chain->buffer + chain->misalign, size);
        chain->misalign += size;
        chain->off -= size;
    } else {
        p->last = tmp;
    }

    tmp->next = chain;

    if (removed_last_with_data) {
        p->last_with_datap = &p->first;
    } else if (removed_last_with_datap) {
        if (p->first->next && p->first->next->off)
            p->last_with_datap = &p->first->next;
        else
            p->last_with_datap = &p->first;
    }
    return tmp->buffer + tmp->misalign;
}
