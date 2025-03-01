/** fcom: sync: compare directory trees
2022, Simon Zolin */

#include <ffsys/std.h>
#include <ffbase/murmurhash3.h>

enum FILE_CMP_F {
	FILE_CMP_FAST = 1, // Compare just 3 chunks of data (beginning, middle, end)
};

/** Compare files content.
window: buffer size
flags: enum FILE_CMP_F'
Return 0 if equal;
 1 if not equal;
 <0 on error */
static inline int file_cmp(const char *fn1, const char *fn2, ffsize window, ffuint flags)
{
	int rc = -1;
	char *buf = NULL;
	fffd f1 = FFFILE_NULL, f2 = FFFILE_NULL;
	ffssize r1, r2;
	ffuint64 sz, off = 0, offsets[3];
	ffuint i = 0;

	if (FFFILE_NULL == (f1 = fffile_open(fn1, FFFILE_READONLY))
		|| FFFILE_NULL == (f2 = fffile_open(fn2, FFFILE_READONLY)))
		goto end;

	sz = fffile_size(f1);
	if (sz != (ffuint64)fffile_size(f2)) {
		rc = 1;
		goto end;
	}
	offsets[0] = 0;
	offsets[1] = (sz > window) ? sz / 2 : ~0ULL;
	offsets[2] = sz - window;

	if (!(buf = (char*)ffmem_align(2 * window, 4096)))
		goto end;

	for (;;) {
		if (flags & FILE_CMP_FAST) {
			if (i == 3)
				break;
			off = offsets[i++];
			if ((ffint64)off < 0)
				break;
		}

		r1 = fffile_readat(f1, buf, window, off);
		r2 = fffile_readat(f2, buf + window, window, off);
		off += window;
		if (r1 < 0 || r2 < 0)
			goto end;
		if (r1 != r2 || ffmem_cmp(buf, buf + window, r1)) {
			rc = 1;
			goto end;
		}
		if ((ffsize)r1 < window)
			break;
	}

	rc = 0;

end:
	fffile_close(f1);
	fffile_close(f2);
	ffmem_free(buf);
	return rc;
}

struct fntree_cmp_ent {
	uint status; // enum FNTREE_CMP | enum FCOM_SYNC
	const fntree_entry *l, *r;
	const fntree_block *lb, *rb;
};

#define WIDTH_NAME  40L
#define WIDTH_SIZE  "10"

struct diff {
	fntree_cmp	fcmp;
	xxvec		ents; // fntree_cmp_ent[]
	xxvec		filter; // fntree_cmp_ent*[]
	ffmap		moved; // hash(name, struct fcom_sync_entry) -> fntree_cmp_ent*
	fcom_sync_snapshot *left, *right;
	xxvec		lname, rname;
	uint		options;
	uint		sort_flags;
	struct fcom_sync_props props;
	fcom_sync_diff_entry dif_ent;
	struct fcom_sync_diff_stats stats;

	~diff() {
		ffmap_free(&this->moved);
	}

	/** Compare 2 files */
	static int data_cmp(void *opaque, const fntree_entry *l, const fntree_entry *r)
	{
		ffstr lname = fntree_name(l), rname = fntree_name(r);
		int i = ffstr_icmp2(&lname, &rname);
		if (i < 0)
			return FNTREE_CMP_LEFT;
		else if (i > 0)
			return FNTREE_CMP_RIGHT;

		struct diff *sd = (struct diff*)opaque;
		const struct fcom_sync_entry *ld = (struct fcom_sync_entry*)fntree_data(l)
			, *rd = (struct fcom_sync_entry*)fntree_data(r);

		uint k = FNTREE_CMP_EQ;

		if (ld->size != rd->size) {
			uint f = (ld->size < rd->size) ? FCOM_SYNC_SMALLER : FCOM_SYNC_LARGER;
			k |= FNTREE_CMP_NEQ | f;
		}

		if (!(sd->options & FCOM_SYNC_DIFF_NO_ATTR)
			&& (ld->unix_attr != rd->unix_attr
				|| ld->win_attr != rd->win_attr))
			k |= FNTREE_CMP_NEQ | FCOM_SYNC_ATTR;

		if (!(sd->options & FCOM_SYNC_DIFF_NO_TIME)) {
			int i;
			if ((sd->options & FCOM_SYNC_DIFF_TIME_2SEC)
				&& ld->mtime.sec/2 == rd->mtime.sec/2)
			{}
			else if ((i = fftime_cmp(&ld->mtime, &rd->mtime))) {
				uint f = (i < 0) ? FCOM_SYNC_OLDER : FCOM_SYNC_NEWER;
				k |= FNTREE_CMP_NEQ | f;
			}
		}

		return k;
	}

	/** Return 1 if two files match by name and meta */
	static int moved_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
	{
		struct diff *sd = (struct diff*)opaque;
		const fntree_cmp_ent *ce = (fntree_cmp_ent*)val;
		const fntree_entry *l = ce->l;
		if (l == NULL)
			l = ce->r;
		const struct fcom_sync_entry *ld = (struct fcom_sync_entry*)fntree_data(l);

		const fntree_entry *r = (fntree_entry*)key;
		const struct fcom_sync_entry *rd = (struct fcom_sync_entry*)fntree_data(r);

		return ((sd->options & FCOM_SYNC_DIFF_MOVE_NO_NAME)
				|| (l->name_len == r->name_len
					&& !ffmem_cmp(l->name, r->name, l->name_len)))
			&& ld->size == rd->size
			&& ((sd->options & FCOM_SYNC_DIFF_NO_ATTR)
				|| (ld->unix_attr & FFFILE_UNIX_DIR) == (rd->unix_attr & FFFILE_UNIX_DIR))
			&& ((sd->options & FCOM_SYNC_DIFF_NO_TIME)
				|| fftime_to_msec(&ld->mtime) == fftime_to_msec(&rd->mtime));
	}

	uint ent_moved_hash(fntree_entry *e)
	{
		const struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(e);
		struct {
			char type;
			char mtime_msec[8];
			char size[8];
			char name[1024];
		} key;

		key.type = 0;
		if (!(this->options & FCOM_SYNC_DIFF_NO_ATTR))
			key.type = !!(d->unix_attr & FFFILE_UNIX_DIR);

		*(uint64*)key.mtime_msec = 0;
		if (!(this->options & FCOM_SYNC_DIFF_NO_TIME))
			*(uint64*)key.mtime_msec = fftime_to_msec(&d->mtime);

		*(uint64*)key.size = d->size;

		uint n = 1+8+8;
		if (!(this->options & FCOM_SYNC_DIFF_MOVE_NO_NAME))
			n += ffmem_ncopy(key.name, sizeof(key.name), e->name, e->name_len);

		return murmurhash3(&key, n, 0x789abcde);
	}

	/** Find (and add if not found) unique file to a Moved table */
	fntree_cmp_ent* moved_find_add(fntree_entry *e, fntree_cmp_ent *ce)
	{
		uint hash = this->ent_moved_hash(e);
		ffstr name = fntree_name(e);
		fntree_cmp_ent *it = (fntree_cmp_ent*)ffmap_find_hash(&this->moved, hash, e, sizeof(void*), this);
		fcom_dbglog("move: found:%u  hash:%xu  '%S'"
			, (!!it), hash, &name);
		if (it == NULL) {
			// store this file in Moved table
			ffmap_add_hash(&this->moved, hash, ce);
		}
		return it;
	}

	/** Prepare to analyze the difference */
	void init(fcom_sync_snapshot *left, fcom_sync_snapshot *right, uint flags)
	{
		this->options = flags;
		this->left = left;
		this->right = right;
		ffmap_init(&this->moved, moved_keyeq);

		const fntree_block *l = left->root, *r = right->root;
		if (flags & FCOM_SYNC_DIFF_LEFT_PATH_STRIP)
			l = _fntr_ent_first(left->root)->children;
		if (flags & FCOM_SYNC_DIFF_RIGHT_PATH_STRIP)
			r = _fntr_ent_first(right->root)->children;

		fntree_cmp_init(&this->fcmp, l, r, data_cmp, this);
		ffvec_allocT(&this->ents, left->total + right->total + 2, fntree_cmp_ent);
		this->stats.ltotal = left->total;
		this->stats.rtotal = right->total;
	}

	/** Compare next file pair */
	int next()
	{
		fntree_entry *le, *re;
		fntree_block *lb, *rb;
		int r = fntree_cmp_next(&this->fcmp, &le, &re, &lb, &rb);
		if (r < 0) {
			return 1;
		}

		// unset the unused elements
		switch (r & 0x0f) {
		case FNTREE_CMP_LEFT:
			re = NULL, rb = NULL; break;
		case FNTREE_CMP_RIGHT:
			le = NULL, lb = NULL; break;
		}

		FF_ASSERT(ents.len < ents.cap);
		fntree_cmp_ent *ce = ffvec_pushT(&this->ents, fntree_cmp_ent);
		ce->status = 0;
		ce->l = le;
		ce->r = re;
		ce->lb = lb;
		ce->rb = rb;

		snapshot::full_name(&this->lname, le, lb);
		snapshot::full_name(&this->rname, re, rb);

		fcom_dbglog("%S <-> %S: %xu", &this->lname, &this->rname, r);

		switch (r & 0x0f) {
		case FNTREE_CMP_LEFT: {
			this->stats.left++;

			fntree_cmp_ent *ce2 = this->moved_find_add(le, ce);
			if (ce2 && (ce2->status & FCOM_SYNC_RIGHT)) {
				// the same file was seen before within Right list
				fcom_dbglog("moved R: %S", &this->lname);
				ce2->status &= ~FCOM_SYNC_RIGHT;
				ce2->status |= FCOM_SYNC_MOVE;
				ce->status = FCOM_SYNC_MOVE | _FCOM_SYNC_SKIP;
				ce2->l = le;
				ce2->lb = lb;
				this->stats.moved++;
				this->stats.left--;
				this->stats.right--;
				this->stats.entries--;
			} else {
				ce->status = FCOM_SYNC_LEFT;
			}
			break;
		}

		case FNTREE_CMP_RIGHT: {
			this->stats.right++;

			fntree_cmp_ent *ce2 = this->moved_find_add(re, ce);
			if (ce2 && (ce2->status & FCOM_SYNC_LEFT)) {
				// the same file was seen before within Left list
				fcom_dbglog("moved L: %S", &this->rname);
				ce2->status &= ~FCOM_SYNC_LEFT;
				ce2->status |= FCOM_SYNC_MOVE;
				ce->status = FCOM_SYNC_MOVE | _FCOM_SYNC_SKIP;
				ce2->r = re;
				ce2->rb = rb;
				this->stats.moved++;
				this->stats.left--;
				this->stats.right--;
				this->stats.entries--;
			} else {
				ce->status = FCOM_SYNC_RIGHT;
			}
			break;
		}

		case FNTREE_CMP_EQ:
			this->stats.eq++;
			ce->status = FCOM_SYNC_EQ;
			break;

		case FNTREE_CMP_NEQ:
			this->stats.neq++;
			ce->status = FCOM_SYNC_NEQ | (r & ~0x0f);
			break;

		default:
			FCOM_ASSERT(0);
			return 1;
		}

		this->stats.entries++;
		return 0;
	}

	static int sort_f(const void *_a, const void *_b, void *udata)
	{
		fcom_sync_diff *sd = (fcom_sync_diff*)udata;
		const fntree_cmp_ent *a = *(fntree_cmp_ent**)_a,  *b = *(fntree_cmp_ent**)_b;
		const struct fcom_sync_entry *d;
		uint64 as = 0, bs = 0;
		fftime at = {}, bt = {};

		if (a->l) {
			d = (struct fcom_sync_entry*)fntree_data(a->l);
			as = d->size;
			at = d->mtime;
		}
		if (a->r) {
			d = (struct fcom_sync_entry*)fntree_data(a->r);
			as = ffmax(as, d->size);
			if (fftime_cmp(&at, &d->mtime) < 0)
				at = d->mtime;
		}

		if (b->l) {
			d = (struct fcom_sync_entry*)fntree_data(b->l);
			bs = d->size;
			bt = d->mtime;
		}
		if (b->r) {
			d = (struct fcom_sync_entry*)fntree_data(b->r);
			bs = ffmax(bs, d->size);
			if (fftime_cmp(&bt, &d->mtime) < 0)
				bt = d->mtime;
		}

		int rs = (as > bs) ? -1
			: (as < bs) ? 1
			: 0;
		if (sd->sort_flags & FCOM_SYNC_SORT_FILESIZE)
			return rs;

		int rt = (fftime_cmp(&at, &bt) > 0) ? -1
			: (fftime_cmp(&at, &bt) < 0) ? 1
			: 0;
		if (sd->sort_flags & FCOM_SYNC_SORT_MTIME)
			return rt;

		return 0;
	}

	/** Prepare diff list; sort (filter) files by size. */
	void dups_init(fcom_sync_snapshot *left, uint flags)
	{
		this->options = flags;
		this->left = left;
		fntree_block *b = _fntr_ent_first(left->root)->children;
		if (!b)
			return;
		this->ents.alloc<fntree_cmp_ent>(left->total);
		this->filter.alloc<void*>(left->total);
		this->stats.ltotal = left->total;

		fntree_cursor cur = {};
		fntree_entry *e;
		while ((e = fntree_cur_next_r(&cur, &b))) {
			FF_ASSERT(this->ents.len < this->ents.cap);
			fntree_cmp_ent *ce = ffvec_zpushT(&this->ents, fntree_cmp_ent);
			ce->l = e;
			ce->lb = b;

			*this->filter.push<void*>() = ce;
		}

		this->sort_flags = FCOM_SYNC_SORT_FILESIZE;
		ffsort(this->filter.ptr, this->filter.len, sizeof(void*), sort_f, this);
	}

	/** Walk through the files and compare by content.
	 * Associate duplicate files with each other.
	Set 'EQ' status for the duplicate files; set 'NEQ' otherwise. */
	void dups_scan()
	{
		uint64 sz, sz_prev = ~0ULL;
		struct fntree_cmp_ent **it, *ce, *ce_prev = NULL;
		FFSLICE_WALK(&this->filter, it) {
			this->stats.entries++;
			ce = *it;
			struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(ce->l);
			sz = d->size;
			if (sz_prev != sz) {
				sz_prev = sz;
				if (ce_prev) {
					this->stats.neq++;
					ce_prev->status = FCOM_SYNC_NEQ;
				}
				ce_prev = ce;
				continue;
			}

			snapshot::full_name(&this->lname, ce_prev->l, ce_prev->lb);
			snapshot::full_name(&this->rname, ce->l, ce->lb);
			fcom_dbglog("comparing files \"%s\" and \"%s\"..."
				, this->lname.strz(), this->rname.strz());
			if (file_cmp(this->lname.strz(), this->rname.strz(), 4096, FILE_CMP_FAST)) {
				this->stats.neq++;
				ce_prev->status = FCOM_SYNC_NEQ;
				ce_prev = ce;
				continue; // Note: only 2 consecutive files with the same size are compared!
			}
			this->stats.eq++;
			ce_prev->status = FCOM_SYNC_EQ;
			ce_prev->r = ce->l;
			ce_prev->rb = ce->lb;
			ce->status = FCOM_SYNC_DONE;
			ce_prev = NULL;
			sz_prev = ~0ULL;
		}

		if (ce_prev) {
			this->stats.neq++;
			ce_prev->status = FCOM_SYNC_NEQ;
		}

		this->filter.len = 0;
	}
};

static fcom_sync_diff* sync_diff(fcom_sync_snapshot *left, fcom_sync_snapshot *right, struct fcom_sync_props *props, uint flags)
{
	struct diff *sd = ffmem_new(struct diff);
	sd->props = *props;
	sd->init(left, right, flags);
	while (!sd->next()) {
	}
	props->stats = sd->stats;
	return sd;
}

static fcom_sync_diff* sync_find_dups(fcom_sync_snapshot *left, struct fcom_sync_props *props, uint flags)
{
	struct diff *sd = ffmem_new(struct diff);
	sd->dups_init(left, flags);
	sd->dups_scan();
	props->stats = sd->stats;
	return sd;
}

static void sync_diff_free(fcom_sync_diff *sd)
{
	if (sd)
		sd->~diff();
	ffmem_free(sd);
}

static int str_match(const fntree_block *b, const fntree_entry *e, ffstr m)
{
	if (!e)
		return 0;
	ffstr path = fntree_path(b);
	ffstr name = fntree_name(e);
	return (ffstr_ifindstr(&name, &m) >= 0
		|| ffstr_ifindstr(&path, &m) >= 0);
}

static uint flags_swap(uint u, uint a, uint b)
{
	if (u & a)
		u = (u & ~a) | b;
	else if (u & b)
		u = (u & ~b) | a;
	return u;
}

static uint status_swap(uint st)
{
	st = flags_swap(st, FCOM_SYNC_LEFT, FCOM_SYNC_RIGHT);
	st = flags_swap(st, FCOM_SYNC_NEWER, FCOM_SYNC_OLDER);
	st = flags_swap(st, FCOM_SYNC_LARGER, FCOM_SYNC_SMALLER);
	return st;
}

static uint sync_view(fcom_sync_diff *sd, struct fcom_sync_props *props, uint flags)
{
	sd->filter.len = 0;
	fntree_cmp_ent *ce;
	FFSLICE_WALK(&sd->ents, ce) {

		if (ce->status & _FCOM_SYNC_SKIP)
			continue;

		if (!(flags & FCOM_SYNC_DONE) && (ce->status & FCOM_SYNC_DONE))
			continue;

		uint st = ce->status;
		if (flags & FCOM_SYNC_SWAP)
			st = status_swap(ce->status);

		switch (st & FCOM_SYNC_MASK) {
		case FCOM_SYNC_LEFT:
			if (!(flags & FCOM_SYNC_LEFT))
				continue;
			break;

		case FCOM_SYNC_RIGHT:
			if (!(flags & FCOM_SYNC_RIGHT))
				continue;
			break;

		case FCOM_SYNC_NEQ:
			if (!(flags & FCOM_SYNC_NEQ))
				continue;
			if (!(flags & (FCOM_SYNC_NEWER | FCOM_SYNC_OLDER))
				&& !(st & (FCOM_SYNC_LARGER | FCOM_SYNC_SMALLER | FCOM_SYNC_ATTR)))
				continue;
			break;

		case FCOM_SYNC_MOVE:
			if (!(flags & FCOM_SYNC_MOVE))
				continue;
			break;

		case FCOM_SYNC_EQ:
			if (!(flags & FCOM_SYNC_EQ))
				continue;
			break;
		}

		const struct fcom_sync_entry *ld = NULL, *rd = NULL;
		if (ce->l)
			ld = (struct fcom_sync_entry*)fntree_data(ce->l);
		if (ce->r)
			rd = (struct fcom_sync_entry*)fntree_data(ce->r);

		if (!(flags & FCOM_SYNC_DIR)
			&& ((ld && (ld->unix_attr & FFFILE_UNIX_DIR))
				|| (rd && (rd->unix_attr & FFFILE_UNIX_DIR))))
			continue;

		if (props->since_time.sec) {
			if ((!ld || fftime_cmp(&ld->mtime, &props->since_time) < 0)
				&& (!rd || fftime_cmp(&rd->mtime, &props->since_time) < 0))
				continue;
		}

		uint included = !props->include.len;

		ffstr *s;
		FFSLICE_WALK(&props->exclude, s) {
			if (str_match(ce->lb, ce->l, *s)
				|| str_match(ce->rb, ce->r, *s))
				goto next;
		}

		FFSLICE_WALK(&props->include, s) {
			if (str_match(ce->lb, ce->l, *s)
				|| str_match(ce->rb, ce->r, *s)) {
				included = 1;
				break;
			}
		}

		if (!included)
			continue;

		*sd->filter.push<void*>() = ce;

	next:
		;
	}

	return sd->filter.len;
}

static uint sync_sort(fcom_sync_diff *sd, uint flags)
{
	sd->sort_flags = flags;
	ffsort(sd->filter.ptr, sd->filter.len, sizeof(void*), fcom_sync_diff::sort_f, sd);
	return sd->filter.len;
}

#define FF_SWAP2(a, b) \
({ \
	__typeof__(a) __tmp = a; \
	a = b; \
	b = __tmp; \
})

static int sync_info_id(fcom_sync_diff *sd, void *id, uint flags, fcom_sync_diff_entry *dst)
{
	ffmem_zero_obj(dst);
	fntree_cmp_ent *ce = (fntree_cmp_ent*)id;
	dst->status = ce->status;
	ffvec lname = {}, rname = {};
	snapshot::full_name(&lname, ce->l, ce->lb);
	snapshot::full_name(&rname, ce->r, ce->rb);
	dst->lname = *(ffstr*)&lname;
	dst->rname = *(ffstr*)&rname;
	dst->left = (ce->l) ? (struct fcom_sync_entry*)fntree_data(ce->l) : NULL;
	dst->right = (ce->r) ? (struct fcom_sync_entry*)fntree_data(ce->r) : NULL;

	if (flags & FCOM_SYNC_SWAP) {
		dst->status = status_swap(ce->status);
		FF_SWAP2(dst->lname, dst->rname);
		FF_SWAP2(dst->left, dst->right);
	}

	dst->id = ce;
	return 0;
}

static int sync_info(fcom_sync_diff *sd, uint i, uint flags, fcom_sync_diff_entry *dst)
{
	if (i >= sd->filter.len)
		return -1;

	fntree_cmp_ent *ce = *ffslice_itemT(&sd->filter, i, fntree_cmp_ent*);
	return sync_info_id(sd, ce, flags, dst);
}

static uint sync_status(fcom_sync_diff *sd, void *id, uint mask, uint val)
{
	fntree_cmp_ent *ce = (fntree_cmp_ent*)id;
	ce->status = (ce->status & ~mask) | val;
	return ce->status;
}
