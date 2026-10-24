/*
 * The 68000's 16 MB as 256-byte pages, for a machine with far less RAM:
 *
 * - a page is read where it is: in the pack (in flash on the Game & Watch),
 *   in RAM, or a shared page of zeros when the game has never written it;
 * - the first write to a page that is not in RAM copies it into a page
 *   from the RAM pool (copy on write);
 * - a sink range (GVRAM when the pack holds the backgrounds) reads as
 *   zero and drops writes;
 * - released pages (the text planes when the game clears them) go back to
 *   the pool.
 *
 * Two levels: 256 segments of 64 KB, each a table of 256 page entries made
 * on its first write (1 KB, from the top of the RAM). An entry is 0 for
 * zeros, else (index + 1) << 1 | in_ram: the page's index in the RAM pages
 * or in the mapped (pack) pages. Entries are 32 bits on every machine, so
 * the PC needs the same RAM as the Game & Watch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crs.h"
#include "crs_pack.h"

#define PAGE CRS_PACK_PAGE
#define SINK ((uint32_t *)&sink_marker)
#define RAM 1u

static uint32_t *segments[256];
static const uint32_t sink_marker = 0;
const uint8_t crs_zero_page[PAGE];

static uint8_t *pool;			/* RAM pages from the bottom, tables from the top */
static uint32_t next_page, page_limit;	/* page indices */
static uint8_t *tables_end;
static uint32_t free_pages;		/* released pages: index + 1, linked through their first 4 bytes */
static const uint8_t *mapped;		/* the pack's pages */
uint32_t crs_ram_pages, crs_ram_pages_peak, crs_ram_tables;

/* What crs_memory_reset sets up again (save states). */
static uint8_t *init_memory;
static uint32_t init_size;
static const uint8_t *map_numbers;
static uint32_t map_count;
static uint32_t sinks[4][2];
static int sink_count;

void (*crs_fatal)(const char *message);

static void fatal(const char *message)
{
	if (crs_fatal) crs_fatal(message);
	CRS_LOG("%s\n", message);
	abort();
}

void crs_memory_init(uint8_t *memory, uint32_t size)
{
	init_memory = memory;
	init_size = size;
	map_numbers = NULL;
	map_count = 0;
	sink_count = 0;
	pool = (uint8_t *)(((uintptr_t)memory + 3) & ~(uintptr_t)3);
	tables_end = (uint8_t *)(((uintptr_t)memory + size) & ~(uintptr_t)3);
	next_page = 0;
	page_limit = (tables_end - pool) / PAGE;
	free_pages = 0;
	mapped = NULL;
	memset(segments, 0, sizeof segments);
	crs_ram_pages = crs_ram_pages_peak = crs_ram_tables = 0;
}

static inline const uint8_t *entry_page(uint32_t e)
{
	uint32_t index = (e >> 1) - 1;
	return e & RAM ? pool + index * PAGE : mapped + index * PAGE;
}

static uint32_t allocate_page(void)
{
	uint32_t index;
	if (free_pages) {
		index = free_pages - 1;
		memcpy(&free_pages, pool + index * PAGE, 4);
	} else {
		if (next_page >= page_limit) fatal("crs: out of RAM pages");
		index = next_page++;
	}
	if (++crs_ram_pages > crs_ram_pages_peak) crs_ram_pages_peak = crs_ram_pages;
	return index;
}

static uint32_t *segment_for_write(uint32_t a)
{
	uint32_t **s = &segments[(a >> 16) & 255];
	if (*s == SINK) return NULL;
	if (!*s) {
		uint32_t size = 256 * sizeof(uint32_t);
		if (tables_end - size < pool + next_page * PAGE) fatal("crs: out of RAM for page tables");
		tables_end -= size;
		page_limit = (tables_end - pool) / PAGE;
		*s = (uint32_t *)tables_end;
		crs_ram_tables++;
		memset(*s, 0, size);
	}
	return *s;
}

const uint8_t *crs_page_read(uint32_t a)
{
	uint32_t *s = segments[(a >> 16) & 255];
	if (!s || s == SINK) return crs_zero_page;
	uint32_t e = s[(a >> 8) & 255];
	return e ? entry_page(e) : crs_zero_page;
}

uint8_t *crs_page_write(uint32_t a)
{
	uint32_t *s = segment_for_write(a);
	if (!s) return NULL;
	uint32_t *e = &s[(a >> 8) & 255];
	if (!(*e & RAM)) {
		uint32_t index = allocate_page();
		memcpy(pool + index * PAGE, *e ? entry_page(*e) : crs_zero_page, PAGE);
		*e = (index + 1) << 1 | RAM;
	}
	return pool + ((*e >> 1) - 1) * PAGE;
}

/* Pages read where they are (a pack): count pages, page numbers in
   little-endian u32, data PAGE bytes each. */
void crs_memory_map(const uint8_t *numbers, const uint8_t *data, uint32_t count)
{
	map_numbers = numbers;
	map_count = count;
	mapped = data;
	for (uint32_t i = 0; i < count; i++, numbers += 4) {
		uint32_t page = numbers[0] | numbers[1] << 8 | numbers[2] << 16 | (uint32_t)numbers[3] << 24;
		uint32_t *s = segment_for_write(page * PAGE);
		if (s) s[page & 255] = (i + 1) << 1;
	}
}

void crs_memory_sink(uint32_t start, uint32_t end)
{
	if (sink_count < 4) {
		sinks[sink_count][0] = start;
		sinks[sink_count][1] = end;
		sink_count++;
	}
	for (uint32_t a = start; a < end; a += 0x10000) segments[(a >> 16) & 255] = SINK;
}

void crs_memory_release(uint32_t start, uint32_t end)
{
	for (uint32_t a = start; a < end; a += PAGE) {
		uint32_t *s = segments[(a >> 16) & 255];
		if (!s || s == SINK) continue;
		uint32_t *e = &s[(a >> 8) & 255];
		if (*e & RAM) {
			uint32_t index = (*e >> 1) - 1;
			memcpy(pool + index * PAGE, &free_pages, 4);
			free_pages = index + 1;
			crs_ram_pages--;
		}
		*e = 0;
	}
}

/* --- save states --- */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Whether the pack has this page (its numbers are in ascending order). */
static int pack_has(uint32_t page)
{
	uint32_t low = 0, high = map_count;
	while (low < high) {
		uint32_t middle = (low + high) / 2, n = le32(map_numbers + middle * 4);
		if (n == page) return 1;
		if (n < page) low = middle + 1;
		else high = middle;
	}
	return 0;
}

/* Back to the pack's pages alone (the RAM pool emptied), as after crs_init. */
static void memory_reset(void)
{
	uint32_t peak = crs_ram_pages_peak;
	const uint8_t *numbers = map_numbers, *data = mapped;
	uint32_t count = map_count, saved_sinks[4][2];
	int saved_sink_count = sink_count;
	memcpy(saved_sinks, sinks, sizeof sinks);

	crs_memory_init(init_memory, init_size);
	if (numbers) crs_memory_map(numbers, data, count);
	for (int i = 0; i < saved_sink_count; i++) crs_memory_sink(saved_sinks[i][0], saved_sinks[i][1]);
	crs_ram_pages_peak = peak;
}

/* Every page that differs from the pack: the pages in RAM (number, 1, 256
   bytes) and the pack's pages released to zero (number, 0). Little-endian:
   count, then the records. */
static int page_differs(uint32_t page, uint32_t *e)
{
	uint32_t *s = segments[page >> 8];
	if (!s || s == SINK) return 0;
	*e = s[page & 255];
	return (*e & RAM) || (!*e && pack_has(page));
}

int crs_memory_save(FILE *f)
{
	uint32_t count = 0, e;
	for (uint32_t page = 0; page < 0x10000; page++) count += page_differs(page, &e);
	uint8_t word[4] = { count, count >> 8, count >> 16, count >> 24 };
	if (fwrite(word, 1, 4, f) != 4) return -1;

	for (uint32_t page = 0; page < 0x10000; page++) {
		if (!page_differs(page, &e)) continue;
		uint8_t record[5] = { page, page >> 8, page >> 16, page >> 24, e & RAM };
		if (fwrite(record, 1, 5, f) != 5) return -1;
		if ((e & RAM) && fwrite(entry_page(e), 1, PAGE, f) != PAGE) return -1;
	}
	return 0;
}

int crs_memory_load(FILE *f)
{
	uint8_t word[5];
	if (fread(word, 1, 4, f) != 4) return -1;
	uint32_t count = le32(word);

	memory_reset();
	for (uint32_t i = 0; i < count; i++) {
		if (fread(word, 1, 5, f) != 5) return -1;
		uint32_t a = le32(word) * PAGE;
		if (word[4]) {
			uint8_t *p = crs_page_write(a);
			if (!p || fread(p, 1, PAGE, f) != PAGE) return -1;
		} else {
			crs_memory_release(a, a + PAGE);
		}
	}
	return 0;
}

/* Big-endian access, across pages when it has to. */

uint32_t crs_rd8(uint32_t a)
{
	a &= 0xFFFFFF;
	return crs_page_read(a)[a & (PAGE - 1)];
}

uint32_t crs_rd16(uint32_t a)
{
	a &= 0xFFFFFF;
	if ((a & (PAGE - 1)) <= PAGE - 2) {
		const uint8_t *p = crs_page_read(a) + (a & (PAGE - 1));
		return p[0] << 8 | p[1];
	}
	return crs_rd8(a) << 8 | crs_rd8(a + 1);
}

uint32_t crs_rd32(uint32_t a)
{
	a &= 0xFFFFFF;
	if ((a & (PAGE - 1)) <= PAGE - 4) {
		const uint8_t *p = crs_page_read(a) + (a & (PAGE - 1));
		return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
	}
	return crs_rd16(a) << 16 | crs_rd16(a + 2);
}

void crs_wr8(uint32_t a, uint32_t v)
{
	a &= 0xFFFFFF;
	uint8_t *p = crs_page_write(a);
	if (p) p[a & (PAGE - 1)] = v;
}

void crs_wr16(uint32_t a, uint32_t v)
{
	crs_wr8(a, v >> 8);
	crs_wr8(a + 1, v);
}

void crs_wr32(uint32_t a, uint32_t v)
{
	crs_wr16(a, v >> 16);
	crs_wr16(a + 2, v);
}

void crs_read_bytes(uint32_t a, void *out, uint32_t n)
{
	uint8_t *o = out;
	while (n) {
		a &= 0xFFFFFF;
		uint32_t offset = a & (PAGE - 1), chunk = PAGE - offset;
		if (chunk > n) chunk = n;
		memcpy(o, crs_page_read(a) + offset, chunk);
		a += chunk, o += chunk, n -= chunk;
	}
}

void crs_write_bytes(uint32_t a, const void *in, uint32_t n)
{
	const uint8_t *i = in;
	while (n) {
		a &= 0xFFFFFF;
		uint32_t offset = a & (PAGE - 1), chunk = PAGE - offset;
		if (chunk > n) chunk = n;
		uint8_t *p = crs_page_write(a);
		if (p) memcpy(p + offset, i, chunk);
		a += chunk, i += chunk, n -= chunk;
	}
}
