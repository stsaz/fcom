/** fcom: sync: compare directory trees
2022, Simon Zolin */

#include <ffsys/std.h>

#define WIDTH_NAME  40L
#define WIDTH_SIZE  "10"

static void cmp_destroy(struct sync *s)
{
	ffmap_free(&s->cmp.moved);
}

/** Compare 2 files */
static int data_cmp(void *opaque, const fntree_entry *l, const fntree_entry *r)
{
	struct sync *s = (struct sync*)opaque;
	const struct entdata *ld = (entdata*)fntree_data(l), *rd = (entdata*)fntree_data(r);

	uint k = FNTREE_CMP_EQ;

	if (ld->size != rd->size) {
		uint f = (ld->size < rd->size) ? FNTREE_CMP_SMALLER : FNTREE_CMP_LARGER;
		k |= FNTREE_CMP_NEQ | f;
	}

	if (!s->diff_no_attr
		&& (ld->unixattr != rd->unixattr
			|| ld->winattr != rd->winattr))
		k |= FNTREE_CMP_NEQ | FNTREE_CMP_ATTR;

	int i;
	if (!s->diff_no_time
		&& 0 != (i = fftime_cmp(&ld->mtime, &rd->mtime))) {
		uint f = (i < 0) ? FNTREE_CMP_OLDER : FNTREE_CMP_NEWER;
		k |= FNTREE_CMP_NEQ | f;
	}

	return k;
}

/** Prepare full file name */
static void full_name(ffvec *buf, const fntree_entry *e, const fntree_block *b)
{
	buf->len = 0;
	if (e == NULL)
		return;
	ffstr name = fntree_name(e);
	ffstr path = fntree_path(b);
	if (path.len != 0)
		ffvec_addfmt(buf, "%S%c", &path, FFPATH_SLASH);
	ffvec_addfmt(buf, "%S%Z", &name);
	buf->len--;
}

/** Return 1 if two files match by name and meta */
static int moved_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	struct sync *s = (struct sync*)opaque;
	const fntree_cmp_ent *ce = (fntree_cmp_ent*)val;
	const fntree_entry *l = ce->l;
	if (l == NULL)
		l = ce->r;
	const struct entdata *ld = (entdata*)fntree_data(l);

	const fntree_entry *r = (fntree_entry*)key;
	const struct entdata *rd = (entdata*)fntree_data(r);

	return l->name_len == r->name_len
		&& !ffmem_cmp(l->name, r->name, l->name_len)
		&& ld->size == rd->size
		&& (s->diff_no_attr || (ld->unixattr & FFFILE_UNIX_DIR) == (rd->unixattr & FFFILE_UNIX_DIR))
		&& (s->diff_no_time || fftime_to_msec(&ld->mtime) == fftime_to_msec(&rd->mtime));
}

static uint ent_moved_hash(struct sync *s, fntree_entry *e)
{
	const struct entdata *d = (entdata*)fntree_data(e);
	struct {
		char type;
		char mtime_msec[8];
		char size[8];
		char name[1024];
	} key;
	if (!s->diff_no_attr)
		key.type = !!(d->unixattr & FFFILE_UNIX_DIR);
	if (!s->diff_no_time)
		*(uint64*)key.mtime_msec = fftime_to_msec(&d->mtime);
	*(uint64*)key.size = d->size;
	ffuint n = 1+8+8 + ffmem_ncopy(key.name, sizeof(key.name), e->name, e->name_len);
	return murmurhash3(&key, n, 0x789abcde);
}

/** Find (and add if not found) unique file to a Moved table */
static fntree_cmp_ent* moved_find_add(struct sync *s, fntree_entry *e, fntree_cmp_ent *ce)
{
	uint hash = ent_moved_hash(s, e);
	fntree_cmp_ent *it = (fntree_cmp_ent*)ffmap_find_hash(&s->cmp.moved, hash, e, sizeof(void*), s);
	if (it == NULL) {
		// store this file in Moved table
		// fcom_dbglog("moved: ffmap_add_hash %p", ce);
		ffmap_add_hash(&s->cmp.moved, hash, ce);
	}
	return it;
}

/** Prepare to analyze the difference */
static void diff_init(struct sync *s)
{
	ffmap_init(&s->cmp.moved, moved_keyeq);

	const fntree_block *l = s->src.root, *r = s->dst.root;
	if (s->left_path_strip)
		l = _fntr_ent_first(s->src.root)->children;
	if (s->right_path_strip)
		r = _fntr_ent_first(s->dst.root)->children;

	fntree_cmp_init(&s->cmp.fcmp, l, r, data_cmp, s);
	ffvec_allocT(&s->cmp.ents, s->src.total + s->dst.total + 2, fntree_cmp_ent);
}

/** Compare next file pair */
static int diff_next(struct sync *s)
{
	fntree_entry *le, *re;
	fntree_block *lb, *rb;
	int r = fntree_cmp_next(&s->cmp.fcmp, &le, &re, &lb, &rb);
	if (r < 0) {
		fcom_infolog("Diff status: moved:%u  add:%u  del:%u  upd:%u  eq:%u  total:%U/%U"
			, s->cmp.stats.moved, s->cmp.stats.left, s->cmp.stats.right, s->cmp.stats.neq, s->cmp.stats.eq
			, s->src.total, s->dst.total);
		return 1;
	}

	// unset the unused elements
	switch (r & 0x0f) {
	case FNTREE_CMP_LEFT:
		re = NULL, rb = NULL; break;
	case FNTREE_CMP_RIGHT:
		le = NULL, lb = NULL; break;
	}

	fntree_cmp_ent *ce = ffvec_pushT(&s->cmp.ents, fntree_cmp_ent);
	ce->status = r;
	ce->l = le;
	ce->r = re;
	ce->lb = lb;
	ce->rb = rb;

	full_name(&s->cmp.lname, le, lb);
	full_name(&s->cmp.rname, re, rb);

	fcom_dbglog("%S <-> %S: %d", &s->cmp.lname, &s->cmp.rname, r);

	switch (r & 0x0f) {
	case FNTREE_CMP_LEFT: {
		s->cmp.stats.left++;

		fntree_cmp_ent *it = moved_find_add(s, le, ce);
		if (it != NULL && it->l == NULL && it->r != NULL) {
			// the same file was seen before within Right list
			fcom_dbglog("moved R: %S", &s->cmp.lname);
			it->status |= FNTREE_CMP_MOVED;
			ce->status |= FNTREE_CMP_MOVED | FNTREE_CMP_SKIP;
			it->l = le;
			it->lb = lb;
			s->cmp.stats.moved++;
			s->cmp.stats.left--;
			s->cmp.stats.right--;
		}
		break;
	}

	case FNTREE_CMP_RIGHT: {
		s->cmp.stats.right++;

		fntree_cmp_ent *it = moved_find_add(s, re, ce);
		if (it != NULL && it->l != NULL && it->r == NULL) {
			// the same file was seen before within Left list
			fcom_dbglog("moved L: %S", &s->cmp.rname);
			it->status |= FNTREE_CMP_MOVED;
			ce->status |= FNTREE_CMP_MOVED | FNTREE_CMP_SKIP;
			it->r = re;
			it->rb = rb;
			s->cmp.stats.moved++;
			s->cmp.stats.left--;
			s->cmp.stats.right--;
		}
		break;
	}

	case FNTREE_CMP_EQ:
		s->cmp.stats.eq++;
		break;

	case FNTREE_CMP_NEQ: {
		const struct entdata *ld = (entdata*)fntree_data(ce->l);
		if ((ld->unixattr & FFFILE_UNIX_DIR)
			&& (ce->status & (FNTREE_CMP_LARGER | FNTREE_CMP_SMALLER))
			&& !(ce->status & (FNTREE_CMP_NEWER | FNTREE_CMP_OLDER | FNTREE_CMP_ATTR))) {
			// skip directories that differ only by size - we already know their files were added/deleted
			ce->status |= FNTREE_CMP_SKIP;
			return 0;
		}

		s->cmp.stats.neq++;
		break;
	}

	default:
		FCOM_ASSERT("0");
		return 1;
	}
	return 0;
}

static void status_print(struct sync *s, fntree_cmp_ent *ce, ffvec lname, ffvec rname)
{
	if (!s->diff_full_name && s->src.root_dir.len) {
		if (lname.len)
			ffstr_shift((ffstr*)&lname, s->src.root_dir.len);
		if (rname.len)
			ffstr_shift((ffstr*)&rname, s->cmd->output.len);
	}

	if (ce->status & FNTREE_CMP_MOVED) {
		if (s->diff_flags & DIFF_MOVE) {
			const char *clr = FFSTD_CLR_I(FFSTD_BLUE), *clr_reset = FFSTD_CLR_RESET;
			fcom_infolog("%sMOV       %*S  ->  %S%s"
				, clr, WIDTH_NAME, &lname, &rname, clr_reset);
		}
		return;
	}

	uint st = ce->status;

	switch (ce->status & 0x0f) {
	case FNTREE_CMP_LEFT:
		if (s->diff_flags & DIFF_LEFT) {
			const char *clr = FFSTD_CLR(FFSTD_GREEN), *clr_reset = FFSTD_CLR_RESET;
			fcom_infolog("%sADD       %*S  >>%s"
				, clr, WIDTH_NAME, &lname, clr_reset);
		}
		break;

	case FNTREE_CMP_RIGHT:
		if (s->diff_flags & DIFF_RIGHT) {
			const char *clr = FFSTD_CLR(FFSTD_RED), *clr_reset = FFSTD_CLR_RESET;
			fcom_infolog("%sDEL       %*c  <<  %S%s"
				, clr, WIDTH_NAME, ' ', &rname, clr_reset);
		}
		break;

	case FNTREE_CMP_NEQ: {
		if (!(s->diff_flags & DIFF_MOD))
			break;

		int a1 = '.', a2 = '.', a3 = '.';

		if (st & FNTREE_CMP_NEWER)
			a1 = 'N';
		else if (st & FNTREE_CMP_OLDER)
			a1 = 'O';

		if (st & FNTREE_CMP_LARGER)
			a2 = '>';
		else if (st & FNTREE_CMP_SMALLER)
			a2 = '<';

		if (st & FNTREE_CMP_ATTR)
			a3 = 'A';

		const char *cmp = "!=";
		if (st & FNTREE_CMP_LARGER)
			cmp = "> ";
		else if (st & FNTREE_CMP_SMALLER)
			cmp = "< ";
		else if (st & FNTREE_CMP_NEWER)
			cmp = ">=";
		else if (st & FNTREE_CMP_OLDER)
			cmp = "<=";

		const struct entdata *ld = (entdata*)fntree_data(ce->l), *rd = (entdata*)fntree_data(ce->r);
		ffvecxx v;
		v.addf("UPD[%c%c%c]  %*S  %s  %*S"
			, a1, a2, a3
			, WIDTH_NAME, &lname
			, cmp
			, WIDTH_NAME, &rname);

		if (st & (FNTREE_CMP_LARGER | FNTREE_CMP_SMALLER)) {
			v.addf(" %" WIDTH_SIZE "U %" WIDTH_SIZE "U"
				, ld->size, rd->size);
		}

		fcom_infolog("%S", &v);
		break;
	}
	}
}

/** Show next difference from 'cmp.ents' */
static int diff_show_next(struct sync *s)
{
	if (s->cmp.cmp_idx == s->cmp.ents.len) {
		return 1;
	}

	fntree_cmp_ent *ce = ffslice_itemT(&s->cmp.ents, s->cmp.cmp_idx, fntree_cmp_ent);
	s->cmp.cmp_idx++;

	full_name(&s->cmp.lname, ce->l, ce->lb);
	full_name(&s->cmp.rname, ce->r, ce->rb);

	if (ce->status & FNTREE_CMP_SKIP)
		return 0;

	if (s->plain_list) {
		if (ce->status & FNTREE_CMP_MOVED)
			return 0;
		switch (ce->status & 0x0f) {
		case FNTREE_CMP_LEFT:
			if (s->sync_add)
				ffstdout_fmt("%S\n", &s->cmp.lname);
			break;
		case FNTREE_CMP_RIGHT:
			if (s->sync_del)
				ffstdout_fmt("%S\n", &s->cmp.rname);
			break;
		case FNTREE_CMP_NEQ:
			if (s->sync_update)
				ffstdout_fmt("%S\n", &s->cmp.lname);
			break;
		}
		return 0;
	}

	if (ce->status & FNTREE_CMP_MOVED) {
		status_print(s, ce, s->cmp.lname, s->cmp.rname);
		return 0;
	}

	uint st = ce->status;

	switch (st & 0x0f) {
	case FNTREE_CMP_LEFT:
	case FNTREE_CMP_RIGHT:
	case FNTREE_CMP_NEQ:
		status_print(s, ce, s->cmp.lname, s->cmp.rname);
		break;

	case FNTREE_CMP_EQ:
		break;

	default:
		FCOM_ASSERT("0");
		return 1;
	}

	return 0;
}
