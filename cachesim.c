/*
 * CACHESIM - The Cache Simulator
 *
 * for ECE 550
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "list.h"

//#define DEBUG

static void debug(const char *fmt, ...)
{
#ifdef DEBUG
	va_list argp;

	va_start(argp, fmt);
	vprintf(fmt, argp);
	va_end(argp);
#endif
}

struct cache_line {
	unsigned tag;
	unsigned dirty:1, valid:1;
	struct list_node list;
};

struct cache_set {
	struct list_node list;
	struct cache_line *firstline;
	struct cache_line **hashtab;
};

#define WRITE_ALLOC 001
#define REPLACE_MASK 070
#define LRU 000
#define RND 010
#define FIFO 020

struct cache_info {
	unsigned assoc, blksz, cap;
	unsigned hit_time;
	unsigned flags;
};

struct cache {
	struct cache_info info;
	unsigned idx_bits:5, off_bits:5, hash_bits:6;
	unsigned nlines, nsets;
	struct cache_set *sets;
	struct cache_line *lines;
	struct cache_line **hashtab;
};

void
debug_cache(struct cache *c)
{
	unsigned i;

	debug("cache %p\n", c);
	debug("A = %u, B = %u, C = %u\n",
		c->info.assoc, c->info.blksz, c->info.cap);
	debug("idx_bits = %u, off_bits = %u\n",
		c->idx_bits, c->off_bits);
	debug("nlines = %u, nsets = %u\n",
		c->nlines, c->nsets);
	for (i = 0; i < c->nsets; i++) {
		struct cache_set *set;
		struct cache_line *linep;
		struct list_node *listp;

		set = &c->sets[i];
		debug("set %u first line %u\n",
			i, (unsigned) (set->firstline - c->lines));
		list_foreach(listp, &set->list) {
			linep = container_of(listp, struct cache_line, list);
			debug("set %u line %u: ",
				i, (unsigned) (linep - c->lines));
			debug("tag = %x, dirty = %u, valid = %u\n",
				linep->tag, linep->dirty, linep->valid);
		}
	}
}

struct cache_pair {
	struct cache *i, *d; /* I$, D$ */
	int n; /* level */
	unsigned fetchcount[3];
	unsigned misscount[3];
};

static void
failure(const char *msg, ...)
{
	va_list argp;

	va_start(argp, msg);
	vfprintf(stderr, msg, argp);
	va_end(argp);
	exit(EXIT_FAILURE);
}

unsigned
logbase2(unsigned n)
{
	unsigned l;

	if (n == 0 || (n & (n-1)) != 0)
		failure("%u is not a power of two", n);
	for (l = 0; (n >>= 1); l++)
		;
	return l;
}

void
cache_init_sets(struct cache *c)
{
	unsigned i;
	struct cache_line *linep, *end;
	struct cache_line **hashtab;

	linep = c->lines;
	hashtab = c->hashtab;
	for (i = 0; i < c->nsets; i++) {
		struct cache_set *set;

		set = &c->sets[i];
		set->firstline = linep;
		set->hashtab = hashtab;
		list_init(&set->list);
		end = linep + c->info.assoc;
		hashtab += 1 << c->hash_bits;
		for (; linep < end; linep++) {
			list_add_tail(&set->list, &linep->list);
		}
	}
}

void
cache_init(struct cache *c, struct cache_info *info)
{
	unsigned logA, logB, logC;

	c->info = *info;
	c->off_bits = logB = logbase2(info->blksz);
	logA = logbase2(info->assoc);
	logC = logbase2(info->cap);
	if (logC < logA + logB) {
		failure("capactiy cannot be smaller than set size\n");
	}
	c->idx_bits = logC - logA - logB;
	c->nlines = 1U << (logC - logB);
	c->nsets = 1U << c->idx_bits;
	c->lines = calloc(c->nlines, sizeof *c->lines);
	c->sets = calloc(c->nsets, sizeof *c->sets);
	c->hash_bits = logA + 1; /* per set */
	c->hashtab = calloc(1U << (c->idx_bits + c->hash_bits),
			sizeof *c->hashtab);
	cache_init_sets(c);
}

void
cache_cleanup(struct cache *c)
{
	free(c->sets);
	free(c->lines);
	free(c->hashtab);
}


struct cache_line *
cache_find_victim(struct cache *c, struct cache_set *set)
{
	struct cache_line *linep;
	linep = container_of(set->list.prev, struct cache_line, list);

	switch (c->info.flags & REPLACE_MASK) {
	case LRU:
	case FIFO:
		break;
	case RND:
		if (linep->valid) {
			linep = set->firstline + rand() % c->info.assoc;
		}
		break;
	default:
		failure("unknown replacement policy\n");
	}
	return linep;
}

struct cache_line **
hash_find(struct cache *c, struct cache_set *set, unsigned tag)
{
	struct cache_line *linep;
	struct cache_line **slot, **end;
	unsigned hash;

	end = set->hashtab + (1U << c->hash_bits);
	hash = tag & ((1U << c->hash_bits) - 1);
	slot = set->hashtab + hash;
	while ((linep = *slot) != NULL) {
		assert(linep->valid);
		if (linep->tag == tag) {
			break;
		}
		slot++;
		if (slot == end) {
			slot = set->hashtab;
		}
	}
	return slot;
}

void
hash_add(struct cache *c, struct cache_set *set, struct cache_line *linep)
{
	struct cache_line **slot;

	slot = hash_find(c, set, linep->tag);
	assert(*slot == NULL);
	*slot = linep;
}

void
hash_del(struct cache *c, struct cache_set *set, struct cache_line *linep)
{
	size_t i, j, k;
	size_t hashsize;
	struct cache_line **hashtab, **slot;

	hashtab = set->hashtab;
	hashsize = 1U << c->hash_bits;
	slot = hash_find(c, set, linep->tag);
	assert(*slot == linep);
	*slot = NULL;
	i = j = slot - set->hashtab;
	for (;;) { /* fill up holes due to deletion */
		j++;
		if (j == hashsize) {
			j = 0;
		}
		if (hashtab[j] == NULL) {
			break;
		}
		k = hashtab[j]->tag & (hashsize - 1);
		if ((i<j) + (j<k) + (k<=i) == 2) {
			hashtab[i] = hashtab[j];
			hashtab[j] = NULL;
			i = j;
		}
	}
}

struct cache_line *
cache_find_tag(struct cache *c, struct cache_set *set, unsigned tag)
{
	struct cache_line *linep;
#if 0 /* linear search */
	struct list_node *listp;

	list_foreach(listp, &set->list) {
		linep = container_of(listp, struct cache_line, list);
		if (linep->valid && linep->tag == tag)
			return linep;
	}
#else /* hash find */
	linep = *hash_find(c, set, tag);
	if (linep != NULL && linep->valid && linep->tag == tag) {
		return linep;
	}
#endif
	return NULL;
}

#define HIT 0
#define MISS_NOKICK 1
#define MISS_KICK 2
int
cache_access(struct cache *c, unsigned addr, int writing, unsigned *kickout)
{
	unsigned tag, index;
	struct cache_set *set;
	struct cache_line *linep;
	int allocate;
	int rc;

	index = (addr >> c->off_bits) & ((1U << c->idx_bits) - 1);
	tag = addr >> (c->off_bits + c->idx_bits);
	//debug("tag = %x, index = %x\n", tag, index);

	set = &c->sets[index];
	linep = cache_find_tag(c, set, tag);

	allocate = (!writing || c->info.flags & WRITE_ALLOC);

	if (linep == NULL) {
		rc = MISS_NOKICK;
		debug("tag %x not found in set %u\n", tag, index);
		if (allocate) {
			linep = cache_find_victim(c, set);
			debug("victim is line %u, old tag = %x",
				(unsigned) (linep - c->lines), linep->tag);
			if (linep->dirty) {
				debug(" (dirty, to be flushed)");
				*kickout = ((linep->tag << c->idx_bits)
						| index) << c->off_bits;
				rc = MISS_KICK;
			}
			if (linep->valid) {
				hash_del(c, set, linep);
			}
			linep->tag = tag;
			linep->valid = 1;
			debug(", read %x from next level, new tag = %x\n",
				addr, tag);
			linep->dirty = 0;
			hash_add(c, set, linep);
		} else {
			debug("write %x in next level\n", addr);
		}
	} else {
		rc = HIT;
		debug("tag %x found in set %u, line %u\n",
			tag, index, (unsigned) (linep - c->lines));
	}
	if (linep != NULL) {
		if (writing) {
			linep->dirty = 1;
		}
		switch (c->info.flags & REPLACE_MASK) {
		case RND:
		case FIFO:
			if (rc == HIT) {
				break;
			}
			/* FALLTHROUGH */
		case LRU:
			list_del(&linep->list);
			list_add(&set->list, &linep->list);
			break;
		default:
			failure("unknown replacement policy\n");
		}
	}
	return rc;
}


#define DATA_R 0
#define DATA_W 1
#define INST_F 2

void cache_pair_access(struct cache_pair *, unsigned, int);

void
cache_pair_access_block(struct cache_pair *cp, unsigned block_start,
		unsigned block_end, int mode)
{
	unsigned addr, nextblksz;

	if (cp->i == NULL) { /* DRAM? */
		nextblksz = block_end - block_start;
	} else {
		nextblksz = ((mode == INST_F) ? cp->i : cp->d)->info.blksz;
	}
	for (addr = block_start; addr < block_end; addr += nextblksz) {
		cache_pair_access(cp, addr, mode);
	}
}

void
cache_pair_access(struct cache_pair *cp, unsigned addr, int mode)
{
	static const char *modestr[] = {
		"reading data", "writing data", "fetching instruction"
	};
	unsigned kickout;
	unsigned block_start, block_end;
	struct cache *c;
	int rc;

	cp->fetchcount[mode]++;
	if (cp->i == NULL) {
		debug("DRAM: %s at %x\n", modestr[mode], addr);
		return;
	}

	debug("L%d: %s at %x\n", cp->n, modestr[mode], addr);
	c = (mode == INST_F) ? cp->i : cp->d;
	rc = cache_access(c, addr, (mode == DATA_W), &kickout);
	if (rc == HIT) {
		debug("we got a hit, all done!\n");
	} else {
		debug("we got a miss!\n");
		cp->misscount[mode]++;
		block_start = addr >> c->off_bits << c->off_bits;
		block_end = block_start + c->info.blksz;
		debug("so we need to read %x (size %u) from next level\n",
			block_start, c->info.blksz);
		cache_pair_access_block(cp+1, block_start, block_end,
				(mode == INST_F) ? INST_F : DATA_R);
		if (rc == MISS_KICK) {
			debug("kicking out %x (size %u)\n",
				kickout, c->info.blksz);
			cache_pair_access_block(cp+1, kickout,
				kickout + c->info.blksz, DATA_W);
		}
	}
	debug("L%d: finish %s\n", cp->n, modestr[mode]);
}

void
cache_flush(struct cache *c, struct cache_pair *nextlevel)
{
	struct cache_line *linep, *end;
	unsigned index;
	unsigned block_start;

	linep = c->lines;
	for (index = 0; index < c->nsets; index++) {
		end = linep + c->info.assoc;
		for (; linep < end; linep++) {
			if (!linep->dirty) {
				continue;
			}
			block_start = ((linep->tag << c->idx_bits)
					| index) << c->off_bits;
			cache_pair_access_block(nextlevel,
					block_start,
					block_start + c->info.blksz,
					DATA_W);
			linep->dirty = 0;
		}
	}
}

void
cache_pair_flush(struct cache_pair *cp)
{
	for (; cp->i != NULL; cp++) {
		debug("Flushing all dirty blocks in L%d %s cache\n",
			cp->n, (cp->i == cp->d) ? "unified" : "instruction");
		cache_flush(cp->i, cp+1);
		if (cp->i != cp->d) {
			debug("Flushing all dirty blocks in L%d data cache\n",
				cp->n);
			cache_flush(cp->d, cp+1);
		}
	}
}

void
cache_pair_cleanup(struct cache_pair *cp)
{
	for (; cp->i != NULL; cp++) {
		cache_cleanup(cp->i);
		free(cp->i);
		if (cp->i != cp->d) {
			cache_cleanup(cp->d);
			free(cp->d);
		}
	}
}

void
print_count(const char *title, unsigned c[5])
{
	printf("%-11s %11u %11u %11u %11u %11u\n",
		title, c[0], c[1], c[2], c[3], c[4]);
}

void
print_fraction(const char *title, unsigned n[5], unsigned d[5])
{
	double f[5];
	int i;

	for (i = 0; i < 5; i++) {
		f[i] = (double) n[i] / ((n == d) ? d[0] : d[i]);
	}
	printf("%-11s %11f %11f %11f %11f %11f\n",
		title, f[0], f[1], f[2], f[3], f[4]);
}

static void
fullcount(unsigned full[5], unsigned count[3])
{
	full[1] = count[INST_F];
	full[2] = count[DATA_R] + count[DATA_W];
	full[3] = count[DATA_R];
	full[4] = count[DATA_W];
	full[0] = full[2] + count[INST_F];
}

#define STAT_HEADER \
"Metrics     Total       Instruction Data        Read        Write\n" \
"----------- ----------- ----------- ----------- ----------- -----------\n"

void
print_stats(struct cache_pair *cp,
		unsigned dram_access_time, unsigned totalinst)
{
	unsigned fetchcount[5], misscount[5];
	double totaltime = 0.0, leveltime;

	for (; cp->i != NULL; cp++) {
		fullcount(fetchcount, cp->fetchcount);
		fullcount(misscount, cp->misscount);
		leveltime = (double) fetchcount[1] * cp->i->info.hit_time +
			fetchcount[2] * cp->d->info.hit_time;
		totaltime += leveltime;
		printf("L%d cache\n%s", cp->n, STAT_HEADER);
		print_count("fetches", fetchcount);
		print_fraction(" fraction", fetchcount, fetchcount);
		print_count("misses", misscount);
		print_fraction(" miss rate", misscount, fetchcount);
		printf("Total time spent on L%d: %.0f\n\n", cp->n, leveltime);
	}
	printf("DRAM\n%s", STAT_HEADER);
	fullcount(fetchcount, cp->fetchcount);
	leveltime = (double) fetchcount[0] * dram_access_time;
	totaltime += leveltime;
	print_count("fetches", fetchcount);
	print_fraction(" fraction", fetchcount, fetchcount);
	printf("Total time spent on DRAM: %.0f\n\n", leveltime);
	printf("Total time: %.0f, average time per instruction: %g\n",
		totaltime, totaltime / totalinst);
}

void
showhelp()
{
	printf("CACHESIM v0.2\n"
		"usage: cachesim [options] input_file\n\n"
		"OPTIONS\n"
		"-L<n>,<cachespec>     specify unified L<n> cache\n"
		"-I<n>,<cachespec>     specify split L<n> instruction cache\n"
		"-D<n>,<cachespec>     specify split L<n> data cache\n"
		"-T,<T>                specify DRAM access time = <T>\n"
		"\n<cachespec>: <A>,<B>,<C>,<T>,<flags>\n"
		"\tA: associativity\n"
		"\tB: block size\n"
		"\tC: capacity\n"
		"\tT: hit time\n"
		"\tflags: sum of\n"
		"\t\t00\twrite-allocate OFF\n"
		"\t\t01\twrite-allocate ON\n"
		"\t\t00\treplacement LRU\n"
		"\t\t10\treplacement RND\n"
		"\t\t20\treplacement FIFO\n");
}

struct simulator_info {
	unsigned dram_access_time;
	const char *input_file;
};


#define MAXLEVEL 2

void
make_cache(struct cache_pair L[], int n, int type, struct cache_info *info)
{
	static const char *confstr[] = {
		" instruction", " data", ""
	};
	struct cache *c;
	unsigned assign, conflict;

	if (n <= 0 || n > MAXLEVEL) {
		failure("I cannot simulate L%d cache!\n", n);
	}
	L[n-1].n = n;
	c = malloc(sizeof *c);
	if (c == NULL) {
		failure("I cannot allocate memory for your cache!\n");
	}
	cache_init(c, info);
	assign = (type == 'I') ? 1 : (type == 'D') ? 2 : 3;
	conflict = assign & ((L[n-1].i != NULL) + 2 * (L[n-1].d != NULL));
	if (conflict) {
		failure("You cannot specify L%d%s cache twice!\n",
			n, confstr[conflict-1]);
	}
	if (assign & 1) {
		L[n-1].i = c;
	}
	if (assign & 2) {
		L[n-1].d = c;
	}
}

void
removegaps(struct cache_pair L[])
{
	int i, j;
	unsigned present;

	for (i = j = 0; j <= MAXLEVEL; j++) {
		present = (L[j].i != NULL) + 2 * (L[j].d != NULL);
		if (present == 3) {
			L[i] = L[j];
			i++;
		} else if (present != 0) {
			failure("You did not specify L%d %s cache!\n",
				L[j].n, (present == 1) ? "data"
					: "instruction");
		}
	}
	L[i].i = L[i].d = NULL;
}

void
parse_args(int argc, char *argv[],
		struct cache_pair L[], struct simulator_info *sinfo)
{
	struct cache_info info;
	const char *arg;
	char type;
	int n;

	if (argc < 2) {
		failure("I need at least one argument.  "
				"Try -help for help.\n");
	}
	for (argv++; (arg = *argv) != NULL; argv++) {
		if (strcmp(arg, "-help") == 0 || strcmp(arg, "--help") == 0) {
			showhelp();
			exit(EXIT_SUCCESS);
		}
		if (sscanf(arg, "-T,%u", &sinfo->dram_access_time) == 1) {
			debug("DRAM access time = %u\n",
					sinfo->dram_access_time);
		} else if (sscanf(arg, "-%c%d,%u,%u,%u,%u,%o", &type, &n,
				&info.assoc, &info.blksz, &info.cap,
				&info.hit_time, &info.flags) == 7 &&
				(type == 'L' || type == 'I' || type == 'D')) {
			make_cache(L, n, type, &info);
		} else if (arg[0] != '-') {
			sinfo->input_file = arg;
		} else {
			failure("unknown option %s\n", arg);
		}
	}
	removegaps(L);
}

int main(int argc, char *argv[])
{
	struct simulator_info sinfo = {0, 0};
	struct cache_pair L[MAXLEVEL+1] = { {NULL, NULL} };
	int mode;
	unsigned addr;
	unsigned totalinst = 0;
	FILE *f;

	parse_args(argc, argv, L, &sinfo);

	if (sinfo.input_file == NULL) {
		f = stdin;
	} else {
		f = fopen(sinfo.input_file, "r");
	}
	if (f == NULL) {
		failure("failed to open %s\n", sinfo.input_file);
	}

	while (fscanf(f, "%d%x", &mode, &addr) == 2) {
		totalinst++;
		cache_pair_access(L, addr, mode);
	}
	cache_pair_flush(L);

	print_stats(L, sinfo.dram_access_time, totalinst);

	cache_pair_cleanup(L);

	fclose(f);
	return 0;
}
