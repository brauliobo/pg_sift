/*
 * pgarena.h - the chunk allocator behind the palloc stubs of the standalone harnesses (tools/analyze, tools/query).
 *
 * One malloc per chunk, so that AddressSanitizer sees every overflow of a palloc'd object; all chunks are linked, so that
 * arena_reset frees whatever the code under test left allocated (a backend frees it with its memory context) and arena_atexit
 * does it before LeakSanitizer runs. What LeakSanitizer then reports is real: malloc, strdup or fopen in sift code that
 * was never released.
 */
#ifndef PGARENA_H
#define PGARENA_H

#include <stdlib.h>
#include <string.h>

typedef struct ArenaChunk
{
	struct ArenaChunk *prev,
			   *next;
	size_t		size;
	size_t		pad;			/* keeps the payload 16-byte aligned */
} ArenaChunk;

static ArenaChunk arena = {&arena, &arena, 0, 0};

static inline void *
arena_alloc(size_t size)
{
	ArenaChunk *c = malloc(sizeof(ArenaChunk) + size);

	c->size = size;
	c->next = arena.next;
	c->prev = &arena;
	arena.next->prev = c;
	arena.next = c;
	return c + 1;
}

static inline void
arena_free(void *p)
{
	ArenaChunk *c = (ArenaChunk *) p - 1;

	c->prev->next = c->next;
	c->next->prev = c->prev;
	free(c);
}

static inline void *
arena_realloc(void *p, size_t size)
{
	void	   *n = arena_alloc(size);
	size_t		old = ((ArenaChunk *) p - 1)->size;

	memcpy(n, p, old < size ? old : size);
	arena_free(p);
	return n;
}

static inline void
arena_reset(void)
{
	while (arena.next != &arena)
		arena_free(arena.next + 1);
}

/* Frees the arena at exit, ahead of LeakSanitizer (its handler was registered first, so it runs last) */
static inline void
arena_atexit(void)
{
	static int	registered;

	if (!registered++)
		atexit(arena_reset);
}

#endif							/* PGARENA_H */
