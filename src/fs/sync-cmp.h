/** fcom: sync: compare directory trees
2022, Simon Zolin */

#include <ffsys/std.h>
#include <ffbase/murmurhash3.h>

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
