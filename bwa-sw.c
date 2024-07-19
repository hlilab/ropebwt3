#include <string.h>
#include <stdio.h>
#include "rb3priv.h"
#include "libsais16.h"
#include "io.h" // for rb3_nt6_table[]
#include "fm-index.h"
#include "align.h"
#include "kalloc.h"

/*******************
 * Lightweight BWT *
 *******************/

static uint32_t bwtl_cnt_table[256];

typedef struct {
	void *km;
	int32_t seq_len, bwt_size, n_occ;
	int32_t primary;
	int32_t *occ, *sa, L2[5];
	uint32_t *bwt;
} bwtl_t;

#define bwtl_B0(b, k) ((b)->bwt[(k)>>4]>>((~(k)&0xf)<<1)&3)

void bwtl_init_cnt_table(void)
{
	int i, c;
	for (i = 0; i != 256; ++i) {
		uint32_t x = 0;
		for (c = 0; c != 4; ++c)
			x |= (((i&3) == c) + ((i>>2&3) == c) + ((i>>4&3) == c) + (i>>6 == c)) << (c<<3);
		bwtl_cnt_table[i] = x;
	}
}

bwtl_t *bwtl_gen(void *km, int len, const uint8_t *seq)
{
	bwtl_t *b;
	int32_t i;
	b = Kcalloc(km, bwtl_t, 1); // allocate long-term memory first to reduce memory fragmentation
	b->km = km;
	b->seq_len = len;
	b->bwt_size = (len + 15) / 16;
	b->bwt = Kcalloc(km, uint32_t, b->bwt_size);
	b->n_occ = (len + 16) / 16 * 4;
	b->occ = Kcalloc(km, int32_t, b->n_occ);

	{ // calculate b->bwt
		uint8_t *s;
		uint16_t *s16;
		s16 = Kcalloc(km, uint16_t, len);
		for (i = 0; i < len; ++i) s16[i] = rb3_nt6_table[seq[i]];
		b->sa = Kcalloc(km, int32_t, len + 1);
		libsais16(s16, &b->sa[1], len, 0, 0);
		b->sa[0] = len;
		s = Kcalloc(km, uint8_t, len + 1);
		for (i = 0; i <= len; ++i) {
			if (b->sa[i] == 0) b->primary = i;
			else s[i] = s16[b->sa[i] - 1] - 1;
		}
		kfree(km, s16);
		for (i = b->primary; i < len; ++i) s[i] = s[i + 1];
		for (i = 0; i < len; ++i)
			b->bwt[i>>4] |= s[i] << ((15 - (i&15)) << 1);
		kfree(km, s);
	}
	{ // calculate b->occ
		int32_t c[4];
		memset(c, 0, 16);
		for (i = 0; i < len; ++i) {
			if (i % 16 == 0)
				memcpy(b->occ + (i/16) * 4, c, 16);
			++c[bwtl_B0(b, i)];
		}
		if (i % 16 == 0)
			memcpy(b->occ + (i/16) * 4, c, 16);
		memcpy(b->L2+1, c, 16);
		b->L2[0] = 1;
		for (i = 1; i < 5; ++i) b->L2[i] += b->L2[i-1];
	}
	return b;
}

void bwtl_rank1a(const bwtl_t *bwt, int32_t k, int32_t cnt[4])
{
	uint32_t x, b;
	if (k > bwt->primary) --k; // because $ is not in bwt
	memcpy(cnt, bwt->occ + (k>>4<<2), 16);
	if (k % 16 == 0) return;
	--k;
	b = bwt->bwt[k>>4] & ~((1U<<((~k&15)<<1)) - 1);
	x = bwtl_cnt_table[b&0xff] + bwtl_cnt_table[b>>8&0xff] + bwtl_cnt_table[b>>16&0xff] + bwtl_cnt_table[b>>24];
	x -= 15 - (k&15);
	cnt[0] += x&0xff; cnt[1] += x>>8&0xff; cnt[2] += x>>16&0xff; cnt[3] += x>>24;
}

void bwtl_rank2a(const bwtl_t *bwt, int32_t k, int32_t l, int32_t cntk[4], int32_t cntl[4])
{
	bwtl_rank1a(bwt, k, cntk);
	bwtl_rank1a(bwt, l, cntl);
}

void bwtl_destroy(bwtl_t *bwt)
{
	kfree(bwt->km, bwt->occ);
	kfree(bwt->km, bwt->bwt);
	kfree(bwt->km, bwt->sa);
	kfree(bwt->km, bwt);
}

/*********************
 * Constructing DAWG *
 *********************/

#define kh_packed
#include "khashl-km.h"

typedef struct {
	int32_t total, visit, id;
} deg_cell_t;

KHASHL_MAP_INIT(KH_LOCAL, sw_deg_t, sw_deg, uint64_t, deg_cell_t, kh_hash_uint64, kh_eq_generic)

static sw_deg_t *sw_cal_deg(void *km, const bwtl_t *bwt) // calculate the in-degree of each node in DAWG
{
	sw_deg_t *h;
	int32_t n = 0, m = 16, c, absent;
	uint64_t *a;
	khint_t itr;

	h = sw_deg_init2(km);
	sw_deg_resize(h, bwt->seq_len + 1); // preallocate for efficiency
	itr = sw_deg_put(h, bwt->seq_len + 1, &absent); // put the root
	kh_val(h, itr).total = 0;

	a = Kmalloc(km, uint64_t, m);
	a[n++] = bwt->seq_len + 1; // the root interval is [0,bwt->seq_len + 1)
	while (n > 0) { // 1st pass: count the in-degree of each node in DAWG
		uint64_t x = a[--n]; // pop
		int32_t rlo[4], rhi[4];
		bwtl_rank2a(bwt, x>>32, (int32_t)x, rlo, rhi);
		for (c = 3; c >= 0; --c) { // traverse children
			uint64_t key;
			int32_t lo = bwt->L2[c] + rlo[c];
			int32_t hi = bwt->L2[c] + rhi[c];
			if (lo == hi) continue;
			key = (uint64_t)lo << 32 | hi;
			itr = sw_deg_put(h, key, &absent);
			if (absent) {
				kh_val(h, itr).total = 0;
				Kgrow(km, uint64_t, a, n, m);
				a[n++] = key;
			}
			kh_val(h, itr).total++;
		}
	}
	kfree(km, a);
	return h;
}

typedef struct {
	int32_t n_pre, c;
	int32_t lo, hi;
	int32_t *pre;
} sw_node_t;

typedef struct {
	int32_t n_node, n_pre;
	sw_node_t *node;
	int32_t *pre;
	const bwtl_t *bwt;
} sw_dawg_t;

static sw_dawg_t *sw_dawg_gen(void *km, const bwtl_t *q)
{
	khint_t itr;
	sw_deg_t *h;
	sw_dawg_t *g;
	int32_t i, off_pre = 0, n_a = 0, id = 0;
	uint64_t *a;
	sw_node_t *p;

	h = sw_cal_deg(km, q);
	g = Kcalloc(km, sw_dawg_t, 1);
	g->bwt = q;
	g->n_node = kh_size(h);
	g->node = Kcalloc(km, sw_node_t, g->n_node);
	kh_foreach(h, itr) {
		g->n_pre += kh_val(h, itr).total;
		kh_val(h, itr).visit = kh_val(h, itr).id = 0;
	}
	g->pre = Kcalloc(km, int32_t, g->n_pre);

	a = Kmalloc(km, uint64_t, g->n_node);
	p = &g->node[id++];
	p->lo = 0, p->hi = q->seq_len + 1, p->n_pre = 0, p->pre = g->pre;
	a[n_a++] = q->seq_len + 1; // the root interval
	while (n_a > 0) { // 2nd pass: topological sorting; this is different from the first pass
		uint64_t x = a[--n_a]; // pop
		int32_t rlo[4], rhi[4], c;
		bwtl_rank2a(q, x>>32, (int32_t)x, rlo, rhi);
		for (c = 3; c >= 0; --c) { // traverse children
			uint64_t key;
			int32_t lo = q->L2[c] + rlo[c];
			int32_t hi = q->L2[c] + rhi[c];
			if (lo == hi) continue;
			key = (uint64_t)lo << 32 | hi;
			itr = sw_deg_get(h, key);
			assert(itr != kh_end(h));
			kh_val(h, itr).visit++;
			if (kh_val(h, itr).visit == kh_val(h, itr).total) { // the last predecessors being visited
				kh_val(h, itr).id = id;
				p = &g->node[id++];
				p->lo = lo, p->hi = hi, p->c = c + 1, p->n_pre = 0, p->pre = &g->pre[off_pre]; // c+1 for the nt6 encoding
				off_pre += kh_val(h, itr).total;
				a[n_a++] = key;
			}
		}
	}
	assert(id == g->n_node && off_pre == g->n_pre);
	kfree(km, a);

	for (i = 0; i < g->n_node; ++i) { // populate predecessors
		int32_t rlo[4], rhi[4], c;
		bwtl_rank2a(q, g->node[i].lo, g->node[i].hi, rlo, rhi);
		for (c = 0; c < 4; ++c) { // traverse i's children
			int32_t lo = q->L2[c] + rlo[c];
			int32_t hi = q->L2[c] + rhi[c];
			if (lo == hi) continue;
			itr = sw_deg_get(h, (uint64_t)lo << 32 | hi);
			p = &g->node[kh_val(h, itr).id];
			p->pre[p->n_pre++] = i;
		}
	}
	sw_deg_destroy(h);

	if (rb3_dbg_flag & RB3_DBG_DAWG) {
		for (i = 0; i < g->n_node; ++i) {
			sw_node_t *p = &g->node[i];
			int j;
			fprintf(stderr, "DG\t%d\t[%d,%d)\t", i, p->lo, p->hi);
			for (j = 0; j < p->n_pre; ++j) {
				if (j) fputc(',', stderr);
				fprintf(stderr, "%d", p->pre[j]);
			}
			fputc('\n', stderr);
		}
	}
	return g;
}

static void sw_dawg_destroy(void *km, sw_dawg_t *g)
{
	kfree(km, g->pre); kfree(km, g->node); kfree(km, g);
}

/**********
 * BWA-SW *
 **********/

#include "ksort.h"

#define heap_lt(a, b) ((a) > (b))
KSORT_INIT(rb3_64, uint64_t, heap_lt)

void rb3_swopt_init(rb3_swopt_t *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->n_best = 25;
	opt->min_sc = 30;
	opt->match = 1, opt->mis = 3;
	opt->gap_open = 3, opt->gap_ext = 1;
	opt->r2cache_size = 0x10000;
}

// NB: don't change these values!
#define SW_FROM_H    0
#define SW_FROM_E    1
#define SW_FROM_F    2
#define SW_FROM_OPEN 0
#define SW_FROM_EXT  1

#define SW_F_UNSET (0xfffffff) // 28 bits

typedef struct {
	int32_t H, E, F;
	uint32_t H_from:2, E_from:1, F_from:1, F_from_off:28;
	uint32_t H_from_pos, E_from_pos;
	int64_t lo, hi;
} sw_cell_t;

typedef struct {
	int32_t n;
	sw_cell_t *a;
} sw_row_t;

#define sw_cell_hash(x) (kh_hash_uint64((x).lo) + kh_hash_uint64((x).hi))
#define sw_cell_eq(x, y) ((x).lo == (y).lo && (x).hi == (y).hi)
KHASHL_SET_INIT(KH_LOCAL, sw_candset_t, sw_candset, sw_cell_t, sw_cell_hash, sw_cell_eq)

static void sw_push_state(int32_t last_op, int32_t op, int c, rb3_swrst_t *rst, int32_t len_only)
{
	if (!len_only) {
		rst->rseq[rst->rlen] = c;
		if (last_op == op)
			rst->cigar[rst->n_cigar - 1] += 1U<<4;
		else
			rst->cigar[rst->n_cigar++] = 1U<<4 | op;
	} else {
		rst->n_cigar += last_op == op? 0 : 1;
	}
	if (op == 7 || op == 8) rst->qlen++, rst->rlen++;
	else if (op == 1) rst->qlen++;
	else if (op == 2) rst->rlen++;
}

static void sw_backtrack_core(const rb3_swopt_t *opt, const rb3_fmi_t *f, const sw_dawg_t *g, const sw_row_t *row, uint32_t pos, rb3_swrst_t *rst, int32_t len_only)
{
	int32_t n_col = opt->n_best, last = 0, last_op = -1;
	rst->n_cigar = rst->rlen = rst->qlen = 0;
	while (pos > 0) {
		int32_t r = pos / n_col, c;
		const sw_cell_t *p = &row[r].a[pos%n_col];
		int32_t x = (int32_t)p->H_from | (int32_t)p->E_from<<2 | (int32_t)p->F_from<<3;
		int32_t state = last == 0? x&0x3 : last;
		int32_t ext = state == 1 || state == 2? x>>(state+1)&1 : 0;
		int32_t op = state; // cigar operator
		for (c = 1; c < 7; ++c)
			if (f->acc[c] > p->lo) break;
		--c; // this is the reference base
		//fprintf(stderr, "(%d,%d): sai=[%lld,%lld),c=%c,H=%d,H_from=%d,state=%d,%d\n", r, pos%n_col, p->lo, p->hi, "$ACGTN"[c], p->H, p->H_from, last, state);
		if (state == SW_FROM_H) {
			op = c == g->node[r].c? 7 : 8;
			pos = p->H_from_pos;
		} else if (state == SW_FROM_E) {
			assert(p->E > 0 && p->E_from_pos != UINT32_MAX);
			pos = p->E_from_pos;
		} else if (state == SW_FROM_F) {
			assert(p->F > 0 && p->F_from_off != SW_F_UNSET);
			pos = r * n_col + p->F_from_off;
		}
		sw_push_state(last_op, op, c, rst, len_only);
		last_op = op;
		last = (state == 1 || state == 2) && ext? state : 0;
	}
}

static void sw_backtrack(const rb3_swopt_t *opt, const rb3_fmi_t *f, const sw_dawg_t *g, const sw_row_t *row, uint32_t pos, rb3_swrst_t *rst)
{
	int32_t k;
	const sw_node_t *p;
	const sw_cell_t *q;

	// get CIGAR
	if (rst->score == 0) return;
	sw_backtrack_core(opt, f, g, row, pos, rst, 1);
	rst->rseq = RB3_CALLOC(uint8_t, rst->rlen);
	rst->cigar = RB3_CALLOC(uint32_t, rst->n_cigar);
	sw_backtrack_core(opt, f, g, row, pos, rst, 0);

	// calculate block length and matching length
	rst->mlen = rst->blen = 0;
	for (k = 0; k < rst->n_cigar; ++k) {
		int32_t op = rst->cigar[k]&0xf, len = rst->cigar[k]>>4;
		rst->blen += len;
		if (op == 7) rst->mlen += len;
	}

	// get positions
	p = &g->node[pos / opt->n_best];
	q = &row[pos / opt->n_best].a[pos % opt->n_best];
	rst->lo = q->lo, rst->hi = q->hi;
	rst->n_qoff = p->hi - p->lo;
	rst->qoff = RB3_CALLOC(int32_t, p->hi - p->lo);
	for (k = p->lo; k < p->hi; ++k)
		rst->qoff[k - p->lo] = g->bwt->sa[k];
}

static sw_cell_t *sw_update_candset(sw_candset_t *h, const sw_cell_t *p)
{
	khint_t k;
	int absent;
	k = sw_candset_put(h, *p, &absent);
	if (!absent) {
		sw_cell_t *q = &kh_key(h, k);
		if (q->E < p->E) q->E = p->E, q->E_from = p->E_from, q->E_from_pos = p->E_from_pos;
		if (q->F < p->F) q->F = p->F, q->F_from = p->F_from; // NB: F_from_off is populated differently
		if (q->H < p->H) {
			q->H = p->H, q->H_from = p->H_from;
			if (p->H_from == SW_FROM_H)
				q->H_from_pos = p->H_from_pos; // TODO: is this correct
		}
	}
	return &kh_key(h, k);
}

static inline int32_t sw_heap_insert1(uint64_t *heap, int32_t max, int32_t *sz, uint32_t score, uint32_t id)
{
	uint64_t x = (uint64_t)score<<32 | id;
	if (*sz < max) {
		heap[(*sz)++] = x;
		ks_heapup_rb3_64(*sz, heap);
		return 1;
	} else if (x > heap[0]) {
		heap[0] = x;
		ks_heapdown_rb3_64(0, *sz, heap);
		return 1;
	}
	return 0;
}

static void sw_track_F(void *km, const rb3_fmi_t *f, void *rc, sw_candset_t *h, sw_row_t *row)
{
	int32_t j, n_F = 0;
	for (j = 0; j < row->n; ++j)
		if (row->a[j].F > 0)
			++n_F;
	if (n_F == 0) return; // no F is calculated
	sw_candset_clear(h);
	for (j = 0; j < row->n; ++j) {
		int absent;
		if (row->a[j].F == 0) continue;
		sw_cell_t r = row->a[j];
		r.H = j; // reuse "H" for index
		sw_candset_put(h, r, &absent);
	}
	for (j = 0; j < row->n - 1; ++j) {
		sw_cell_t r = row->a[j];
		int64_t clo[6], chi[6];
		int c;
		khint_t k;
		rb3_fmi_rank2a_cached(f, rc, r.lo, r.hi, clo, chi);
		for (c = 1; c < 6; ++c) {
			r.lo = f->acc[c] + clo[c];
			r.hi = f->acc[c] + chi[c];
			if (r.lo == r.hi) continue;
			k = sw_candset_get(h, r);
			if (k != kh_end(h))
				row->a[kh_key(h, k).H].F_from_off = j;
		}
		if (row->a[j].F_from_off == SW_F_UNSET) {
			assert(row->a[j].H_from != SW_FROM_F);
			row->a[j].F = 0;
		}
	}
}

static void sw_core(void *km, const rb3_swopt_t *opt, const rb3_fmi_t *f, const sw_dawg_t *g, const bwtl_t *bwt, rb3_swrst_t *rst)
{
	uint32_t best_pos = 0;
	int32_t i, c, n_col = opt->n_best, m_fstack = opt->n_best * 2;
	sw_cell_t *cell, *fstack, *p;
	sw_row_t *row;
	int64_t clo[6], chi[6];
	uint64_t *heap;
	sw_candset_t *h;
	void *rc = 0;

	rc = rb3_r2cache_init(km, opt->r2cache_size);
	cell = Kcalloc(km, sw_cell_t, opt->n_best * g->n_node);
	row = Kcalloc(km, sw_row_t, g->n_node);
	for (i = 0; i < g->n_node; ++i)
		row[i].a = &cell[i * n_col];
	p = &row[0].a[row[0].n++];
	p->lo = 0, p->hi = f->acc[6];
	p->H_from = SW_FROM_H;
	rst->score = 0;

	fstack = Kcalloc(km, sw_cell_t, m_fstack);
	heap = Kcalloc(km, uint64_t, opt->n_best);
	h = sw_candset_init2(km);
	sw_candset_resize(h, opt->n_best * 4);
	for (i = 1; i < g->n_node; ++i) {
		const sw_node_t *t = &g->node[i];
		sw_row_t *ri = &row[i];
		int32_t j, k, heap_sz;
		khint_t itr;
		sw_candset_clear(h);
		for (j = 0; j < t->n_pre; ++j) {
			int32_t pid = t->pre[j]; // parent/pre ID
			for (k = 0; k < row[pid].n; ++k) {
				sw_cell_t r;
				p = &row[pid].a[k];
				memset(&r, 0, sizeof(sw_cell_t));
				r.F_from_off = SW_F_UNSET;
				// calculate E
				if (p->H - opt->gap_open > p->E)
					r.E_from = SW_FROM_OPEN, r.E = p->H - opt->gap_open;
				else
					r.E_from = SW_FROM_EXT,  r.E = p->E;
				r.E -= opt->gap_ext;
				if (r.E > 0) { // add to rowaux
					r.lo = p->lo, r.hi = p->hi;
					r.H = r.E;
					r.H_from = SW_FROM_E;
					r.E_from_pos = pid * n_col + k, r.H_from_pos = UINT32_MAX;
					sw_update_candset(h, &r);
				}
				// calculate H
				rb3_fmi_rank2a_cached(f, rc, p->lo, p->hi, clo, chi);
				r.H_from = SW_FROM_H;
				r.E = 0, r.E_from_pos = UINT32_MAX, r.H_from_pos = pid * n_col + k;
				for (c = 1; c < 6; ++c) {
					int32_t sc = c == t->c? opt->match : -opt->mis;
					if (p->H + sc <= 0) continue;
					r.lo = f->acc[c] + clo[c];
					r.hi = f->acc[c] + chi[c];
					if (r.lo == r.hi) continue;
					r.H = p->H + sc;
					sw_update_candset(h, &r);
				}
			}
		}
		if (kh_size(h) == 0) break;
		// find top-n hits
		heap_sz = 0;
		kh_foreach(h, itr)
			sw_heap_insert1(heap, opt->n_best, &heap_sz, kh_key(h, itr).H, itr);
		ks_heapsort_rb3_64(heap_sz, heap);
		ri->n = heap_sz;
		for (j = 0; j < ri->n; ++j)
			ri->a[j] = kh_key(h, (uint32_t)heap[j]);
		for (j = 0; j < heap_sz>>1; ++j) { // reverse heap[] such that it remains a heap
			uint64_t tmp = heap[j];
			heap[j] = heap[heap_sz - j - 1];
			heap[heap_sz - j - 1] = tmp;
		}
		{ // update F
			int32_t n_fstack = 0;
			for (j = ri->n - 1; j >= 0; --j)
				if (ri->a[j].H > opt->gap_open + opt->gap_ext)
					fstack[n_fstack++] = ri->a[j];
			while (n_fstack > 0) {
				sw_cell_t r, z = fstack[--n_fstack];
				int32_t min = heap_sz < opt->n_best? 0 : heap[0]>>32;
				memset(&r, 0, sizeof(sw_cell_t));
				r.H_from_pos = r.E_from_pos = UINT32_MAX, r.F_from_off = SW_F_UNSET;
				if (z.H - opt->gap_open > z.F)
					r.F_from = SW_FROM_OPEN, r.F = z.H - opt->gap_open;
				else
					r.F_from = SW_FROM_EXT,  r.F = z.F;
				r.F -= opt->gap_ext;
				r.H = r.F, r.H_from = SW_FROM_F;
				if (r.H <= min) continue;
				rb3_fmi_rank2a_cached(f, rc, z.lo, z.hi, clo, chi);
				for (c = 1; c < 6; ++c) {
					sw_cell_t *q;
					r.lo = f->acc[c] + clo[c];
					r.hi = f->acc[c] + chi[c];
					if (r.lo == r.hi) continue;
					q = sw_update_candset(h, &r);
					sw_heap_insert1(heap, opt->n_best, &heap_sz, r.H, UINT32_MAX);
					if (r.H - opt->gap_ext > min) {
						Kgrow(km, sw_cell_t, fstack, n_fstack, m_fstack);
						fstack[n_fstack++] = *q;
					}
				}
			}
		}
		heap_sz = 0;
		kh_foreach(h, itr)
		sw_heap_insert1(heap, opt->n_best, &heap_sz, kh_key(h, itr).H, itr);
		ks_heapsort_rb3_64(heap_sz, heap);
		assert(heap_sz > 0);
		ri->n = heap_sz;
		for (j = 0; j < ri->n; ++j)
			ri->a[j] = kh_key(h, (uint32_t)heap[j]);
		sw_track_F(km, f, rc, h, &row[i]);
		if (rb3_dbg_flag & RB3_DBG_SW) { // NB: single-threaded only
			fprintf(stderr, "SW\t%d\t[%d,%d)\t%d:%.3f\t", i, t->lo, t->hi, ri->n, rb3_cputime());
			for (j = 0; j < t->n_pre; ++j) {
				if (j) fputc(',', stderr);
				fprintf(stderr, "%d", t->pre[j]);
			}
			fputc('\t', stderr);
			for (j = 0; j < ri->n; ++j) {
				if (j) fputc(',', stderr);
				fprintf(stderr, "%d", ri->a[j].H);
			}
			fputc('\n', stderr);
		}
		if (ri->a[0].H > rst->score)
			rst->score = ri->a->H, best_pos = i * n_col;
	}
	sw_backtrack(opt, f, g, row, best_pos, rst);

	kfree(km, fstack);
	sw_candset_destroy(h);
	kfree(km, heap);
	kfree(km, row);
	kfree(km, cell);
	rb3_r2cache_destroy(rc);
}

void rb3_sw(void *km, const rb3_swopt_t *opt, const rb3_fmi_t *f, int len, const uint8_t *seq, rb3_swrst_t *rst)
{
	bwtl_t *q;
	sw_dawg_t *g;
	q = bwtl_gen(km, len, seq);
	g = sw_dawg_gen(km, q);
	sw_core(km, opt, f, g, q, rst);
	sw_dawg_destroy(km, g); // this doesn't deallocate q
	bwtl_destroy(q);
}
