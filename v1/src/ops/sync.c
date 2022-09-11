/** fcom: synchronize directories
2022, Simon Zolin */

/* Snapshot ver.1 format:
# fcom file tree snapshot

b "/dir" {
	v 1
	f "file" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	d "dir1" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
}

b "/dir/dir1" {
	v 1
	f "file" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
}
*/

#include <fcom.h>
#include <FFOS/path.h>
#include <util/fntree.h>
#include <ffbase/map.h>

static const fcom_core *core;

struct data {
	uint64 size;
	uint unixattr, winattr;
	uint uid, gid;
	fftime mtime; // UTC
	uint crc32;
};

struct ent {
	char type; // 'f', 'd'
	ffstr name;
	struct data d;
};

enum FNTREE_CMPEX {
	FNTREE_CMP_NEWER = 0x10,
	FNTREE_CMP_OLDER = 0x20,
	FNTREE_CMP_LARGER = 0x40,
	FNTREE_CMP_SMALLER = 0x80,
	FNTREE_CMP_ATTR = 0x0100,
	FNTREE_CMP_MOVED = 0x0200,
	FNTREE_CMP_SKIP = 0x0400, // moved-double or size-changed-directory
};

typedef struct fntree_cmp_ent {
	uint status; // enum FNTREE_CMP | enum FNTREE_CMPEX
	const fntree_entry *l, *r;
	const fntree_block *lb, *rb;
} fntree_cmp_ent;

struct srcdst {
	fntree_block *root, *parent_blk;
	fntree_cursor cur;
	ffvec name;
	uint64 total;
};

struct sync {
	uint st;
	fcom_cominfo *cmd;
	struct srcdst src, dst;
	fcom_file_obj *in, *snap;
	ffstr name, dir;
	ffvec buf;
	struct ent ent;
	uint stop;
	byte sync_add;
	byte diff;
	byte snapshot;
	uint hdr :1;
	uint bhdr :1;
	uint bftr :1;
	uint ent_ready :1;

	fntree_cmp cmp;
	ffvec cmp_ents; // fntree_cmp_ent[]
	ffmap moved; // hash(name, struct data) -> fntree_cmp_ent*
	ffvec lname, rname;
	uint cmp_idx;
	struct {
		uint eq, left, right, neq, moved;
	} cmpstats;
	struct {
		uint add, del, owr, moved;
	} syncstats;
};

#define WIDTH_NAME  40L
#define WIDTH_SIZE  "10"

static const char* sync_help()
{
	return "\
Synchronize directories or create a file tree snapshot.\n\
Implies '--recursive', ignores '--include'/'--exclude'.\n\
Usage:\n\
  fcom sync INPUT_DIR [-o OUT_DIR]|[-s -o FILE] [OPTIONS]\n\
    OPTIONS:\n\
        --add           Copy new files\n\
    -d, --diff          Show difference between source and target\n\
    -s, --snapshot      Create an INPUT_DIR tree snapshot\n\
";
}

static int args_parse(struct sync *s, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 0,	"add",	FFCMDARG_TSWITCH, FF_OFF(struct sync, sync_add) },
		{ 'd',	"diff",	FFCMDARG_TSWITCH, FF_OFF(struct sync, diff) },
		{ 's',	"snapshot",	FFCMDARG_TSWITCH, FF_OFF(struct sync, snapshot) },
		{}
	};
	return core->com->args_parse(cmd, args, s);
}

static void sync_close(fcom_op *op);

static fcom_op* sync_create(fcom_cominfo *cmd)
{
	struct sync *s = ffmem_new(struct sync);
	s->cmd = cmd;

	if (0 != args_parse(s, cmd))
		goto end;
	cmd->recursive = 1;

	if (!s->diff && !s->snapshot) {
		fcom_fatlog("Please use '--diff' or '--snapshot'");
		goto end;
	}
	if (cmd->output.len == 0 && !cmd->stdout) {
		fcom_fatlog("Please use '--output'");
		goto end;
	}

	ffstr rtpath = {};
	s->src.root = fntree_create(rtpath);
	ffstr *path = cmd->input.ptr;
	if (NULL == fntree_add(&s->src.root, path[0], sizeof(struct data)))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	s->in = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&s->buf, cap, 1);
	return s;

end:
	sync_close(s);
	return NULL;
}

static void sync_close(fcom_op *op)
{
	struct sync *s = op;
	core->file->destroy(s->in);
	if (s->snap != NULL)
		core->file->close(s->snap);
	core->file->destroy(s->snap);
	ffvec_free(&s->buf);
	ffvec_free(&s->cmp_ents);
	ffvec_free(&s->lname);
	ffvec_free(&s->rname);
	ffmap_free(&s->moved);

	ffvec_free(&s->src.name);
	fntree_free_all(s->src.root);

	ffvec_free(&s->dst.name);
	fntree_free_all(s->dst.root);

	ffmem_free(s);
}

/** File header */
static ffstr snap_hdr()
{
	ffstr s = FFSTR_INITZ("# fcom file tree snapshot\r\n\r\n");
	return s;
}

/** Branch header */
static void snap_bhdr(ffvec *buf, ffstr dirname)
{
	ffvec_addfmt(buf, "b \"%S\" {\r\n\tv 1\r\n", &dirname);
}

/** File entry */
static void snap_ent_serialize(ffvec *buf, const struct ent *e)
{
	const struct data *d = &e->d;
	ffdatetime dt;
	fftime_split1(&dt, &d->mtime);

	char date_s[16];
	int n = fftime_tostr1(&dt, date_s, sizeof(date_s), FFTIME_DATE_YMD);
	ffstr date = FFSTR_INITN(date_s, n);

	char time_s[16];
	n = fftime_tostr1(&dt, time_s, sizeof(time_s), FFTIME_HMS_MSEC);
	ffstr time = FFSTR_INITN(time_s, n);

	// TODO escape
	// f|d "file" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	ffvec_addfmt(buf, "\t%c \"%S\" %U %xu/%xu %u:%u %S+%S %u\r\n"
		, e->type, &e->name
		, d->size, d->unixattr, d->winattr, d->uid, d->gid, &date, &time, d->crc32);
}

/** Branch footer */
static ffstr snap_bftr()
{
	ffstr s = FFSTR_INITZ("}\r\n\r\n");
	return s;
}

#define RINPUT_NEW_DIR  0x10

/** Get next file from tree */
static int srcdst_next(struct sync *s, struct srcdst *sd, ffstr *name, fntree_entry **e)
{
	fntree_block *b = sd->root;
	fntree_entry *it = fntree_cur_next_r_ctx(&sd->cur, &b);
	if (it == NULL) {
		fcom_dbglog("no more input files");
		return FCOM_COM_RINPUT_NOMORE;
	}

	ffstr nm = fntree_name(it);
	ffstr path = fntree_path(b);
	sd->name.len = 0;
	if (path.len != 0)
		ffvec_addfmt(&sd->name, "%S%c", &path, FFPATH_SLASH);
	ffvec_addfmt(&sd->name, "%S%Z", &nm);

	ffstr_set(name, sd->name.ptr, sd->name.len - 1);
	fcom_dbglog("file: '%S'", name);
	*e = it;

	if (sd->parent_blk != b) {
		sd->parent_blk = b;
		return RINPUT_NEW_DIR;
	}
	return FCOM_COM_RINPUT_OK;
}

/** Add tree branch */
static int srcdst_add_dir(struct srcdst *sd, fffd fd)
{
	ffdirscan ds = {};
	uint flags = 0;

#ifdef FF_LINUX
	ds.fd = fd;
	flags = FFDIRSCAN_USEFD;
#endif

	if (0 != ffdirscan_open(&ds, sd->name.ptr, flags)) {
		fcom_syserrlog("ffdirscan_open");
		return -1;
	}
	ffstr path = FFSTR_INITZ(sd->name.ptr);
	fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct data));
	sd->total += b->entries;
	fntree_attach((fntree_entry*)sd->cur.cur, b);
	ffdirscan_close(&ds);
	return 0;
}

/** Get file attributes
fd: [output] directory descriptor */
static int f_info(struct sync *s, ffstr name, struct ent *e, struct data *d, fffd *fd)
{
	uint flags = fcom_file_cominfo_flags_i(s->cmd);
	flags |= FCOM_FILE_READ;
	int r = core->file->open(s->in, name.ptr, flags);
	if (r == FCOM_FILE_ERR) return -1;

	fffileinfo fi;
	r = core->file->info(s->in, &fi);
	if (r == FCOM_FILE_ERR) return -1;

	ffmem_zero_obj(e);
	e->type = 'f';

	if (fffile_isdir(fffileinfo_attr(&fi))) {
		e->type = 'd';
		*fd = core->file->fd(s->in, FCOM_FILE_ACQUIRE);
	}

	core->file->close(s->in);

	d->size = fffileinfo_size(&fi);
	d->mtime = fffileinfo_mtime(&fi);
	d->mtime.sec += FFTIME_1970_SECONDS;

#ifdef FF_UNIX
	d->unixattr = fffileinfo_attr(&fi);
	d->uid = fi.st_uid;
	d->gid = fi.st_gid;
	if (e->type == 'd')
		d->winattr |= FFFILE_WIN_DIR;
#else
	d->winattr = fffileinfo_attr(&fi);
	if (e->type == 'd')
		d->unixattr |= FFFILE_UNIX_DIR;
#endif

	// d->crc32 = ;

	return 0;
}

/** Compare 2 files */
static int data_cmp(void *opaque, const fntree_entry *l, const fntree_entry *r)
{
	const struct data *ld = fntree_data(l), *rd = fntree_data(r);

	uint k = FNTREE_CMP_EQ;

	if (ld->size != rd->size) {
		uint f = (ld->size < rd->size) ? FNTREE_CMP_SMALLER : FNTREE_CMP_LARGER;
		k |= FNTREE_CMP_NEQ | f;
	}

	if (ld->unixattr != rd->unixattr
		|| ld->winattr != rd->winattr)
		k |= FNTREE_CMP_NEQ | FNTREE_CMP_ATTR;

	int i;
	if (0 != (i = fftime_cmp(&ld->mtime, &rd->mtime))) {
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
	const fntree_cmp_ent *ce = val;
	const fntree_entry *l = ce->l;
	if (l == NULL)
		l = ce->r;
	const struct data *ld = fntree_data(l);

	const fntree_entry *r = key;
	const struct data *rd = fntree_data(r);

	return l->name_len == r->name_len
		&& !ffmem_cmp(l->name, r->name, l->name_len)
		&& (ld->unixattr & FFFILE_UNIX_DIR) == (rd->unixattr & FFFILE_UNIX_DIR)
		&& ld->size == rd->size
		&& fftime_to_msec(&ld->mtime) == fftime_to_msec(&rd->mtime);
}

/** Find (and add if not found) unique file to a Moved table */
static fntree_cmp_ent* moved_find_add(struct sync *s, fntree_entry *e, fntree_cmp_ent *ce)
{
	const struct data *d = fntree_data(e);

	struct {
		char type;
		char size[8];
		char mtime_msec[8];
		char name[255];
	} key;
	key.type = !!(d->unixattr & FFFILE_UNIX_DIR);
	*(uint64*)key.size = d->size;
	*(uint64*)key.mtime_msec = fftime_to_msec(&d->mtime);
	ffuint n = 1+8+8 + ffmem_ncopy(key.name, sizeof(key.name), e->name, e->name_len);
	ffuint hash = murmurhash3(&key, n, 0x789abcde);

	fntree_cmp_ent *it = ffmap_find_hash(&s->moved, hash, e, sizeof(void*), NULL);
	if (it == NULL) {
		// store this file in Moved table
		fcom_dbglog("ffmap_add_hash %p", ce);
		ffmap_add_hash(&s->moved, hash, ce);
	}
	return it;
}

/** Prepare to analyze the difference */
static void diff_init(struct sync *s)
{
	ffmap_init(&s->moved, moved_keyeq);
	const fntree_block *l = _fntr_ent_first(s->src.root)->children;
	const fntree_block *r = _fntr_ent_first(s->dst.root)->children;
	fntree_cmp_init(&s->cmp, l, r, data_cmp, NULL);
	ffvec_allocT(&s->cmp_ents, s->src.total + s->dst.total, fntree_cmp_ent);
}

/** Compare next file pair */
static int diff_next(struct sync *s)
{
	fntree_entry *le, *re;
	fntree_block *lb, *rb;
	int r = fntree_cmp_next(&s->cmp, &le, &re, &lb, &rb);
	if (r < 0) {
		fcom_infolog("Diff status: moved:%u  add:%u  del:%u  mod:%u  eq:%u  total:%U/%U"
			, s->cmpstats.moved, s->cmpstats.left, s->cmpstats.right, s->cmpstats.neq, s->cmpstats.eq
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

	fntree_cmp_ent *ce = ffvec_pushT(&s->cmp_ents, fntree_cmp_ent);
	ce->status = r;
	ce->l = le;
	ce->r = re;
	ce->lb = lb;
	ce->rb = rb;

	full_name(&s->lname, le, lb);
	full_name(&s->rname, re, rb);

	fcom_dbglog("%S <-> %S: %d", &s->lname, &s->rname, r);

	switch (r & 0x0f) {
	case FNTREE_CMP_LEFT: {
		s->cmpstats.left++;

		fntree_cmp_ent *it = moved_find_add(s, le, ce);
		if (it != NULL && it->l == NULL && it->r != NULL) {
			// the same file was seen before within Right list
			fcom_dbglog("moved R: %S", &s->lname);
			ce->status |= FNTREE_CMP_MOVED;
			it->status |= FNTREE_CMP_MOVED | FNTREE_CMP_SKIP;
			ce->l = le;
			ce->lb = lb;
			s->cmpstats.moved++;
			s->cmpstats.left--;
			s->cmpstats.right--;
		}
		break;
	}

	case FNTREE_CMP_RIGHT: {
		s->cmpstats.right++;

		fntree_cmp_ent *it = moved_find_add(s, re, ce);
		if (it != NULL && it->l != NULL && it->r == NULL) {
			// the same file was seen before within Left list
			fcom_dbglog("moved L: %S", &s->rname);
			it->status |= FNTREE_CMP_MOVED;
			ce->status |= FNTREE_CMP_MOVED | FNTREE_CMP_SKIP;
			it->r = re;
			it->rb = rb;
			s->cmpstats.moved++;
			s->cmpstats.left--;
			s->cmpstats.right--;
		}
		break;
	}

	case FNTREE_CMP_EQ:
		s->cmpstats.eq++;
		break;

	case FNTREE_CMP_NEQ: {
		const struct data *ld = fntree_data(ce->l);
		if ((ld->unixattr & FFFILE_UNIX_DIR)
			&& (ce->status & (FNTREE_CMP_LARGER | FNTREE_CMP_SMALLER))
			&& !(ce->status & (FNTREE_CMP_NEWER | FNTREE_CMP_OLDER | FNTREE_CMP_ATTR))) {
			// skip directories that differ only by size - we already know their files were added/deleted
			ce->status |= FNTREE_CMP_SKIP;
			return 0;
		}

		s->cmpstats.neq++;
		break;
	}

	default:
		FCOM_ASSERT("0");
		return 1;
	}
	return 0;
}

/** Show next difference from 'cmp_ents' */
static int diff_show_next(struct sync *s)
{
	if (s->cmp_idx == s->cmp_ents.len) {
		return 1;
	}

	fntree_cmp_ent *ce = ffslice_itemT(&s->cmp_ents, s->cmp_idx, fntree_cmp_ent);
	s->cmp_idx++;

	full_name(&s->lname, ce->l, ce->lb);
	full_name(&s->rname, ce->r, ce->rb);

	if (ce->status & FNTREE_CMP_SKIP)
		return 0;

	if (ce->status & FNTREE_CMP_MOVED) {
		fcom_infolog("MOVE      %*S  ->  %S", WIDTH_NAME, &s->lname, &s->rname);
		return 0;
	}

	uint st = ce->status;

	switch (st & 0x0f) {
	case FNTREE_CMP_LEFT:
		fcom_infolog("ADD       %*S  >>", WIDTH_NAME, &s->lname);
		break;

	case FNTREE_CMP_RIGHT:
		fcom_infolog("DEL       %*c  <<  %S", WIDTH_NAME, ' ', &s->rname);
		break;

	case FNTREE_CMP_EQ:
		break;

	case FNTREE_CMP_NEQ: {
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

		const struct data *ld = fntree_data(ce->l), *rd = fntree_data(ce->r);
		ffvec v = {};

		ffvec_addfmt(&v, "MOD[%c%c%c]  %*S  !=  %*S"
			, a1, a2, a3
			, WIDTH_NAME, &s->lname, WIDTH_NAME, &s->rname);

		if (st & (FNTREE_CMP_LARGER | FNTREE_CMP_SMALLER)) {
			ffvec_addfmt(&v, " %" WIDTH_SIZE "U %" WIDTH_SIZE "U"
				, ld->size, rd->size);
		}

		fcom_infolog("%S", &v);
		ffvec_free(&v);
		break;
	}

	default:
		FCOM_ASSERT("0");
		return 1;
	}

	return 0;
}

/** Perform a single sync operation */
static int sync1(struct sync *s)
{
	if (s->cmp_idx == s->cmp_ents.len) {
		fcom_infolog("Result: added:%u"
			, s->syncstats.add);
		return 1;
	}

	fntree_cmp_ent *ce = ffslice_itemT(&s->cmp_ents, s->cmp_idx, fntree_cmp_ent);
	s->cmp_idx++;

	full_name(&s->lname, ce->l, ce->lb);
	full_name(&s->rname, ce->r, ce->rb);

	switch (ce->status & 0x0f) {
	case FNTREE_CMP_EQ:
		break;

	case FNTREE_CMP_LEFT:
		if (s->sync_add) {
		}
		break;

	case FNTREE_CMP_RIGHT:
		break;

	case FNTREE_CMP_NEQ:
		break;

	default:
		FCOM_ASSERT("0");
		return 1;
	}
	return 0;
}

static void sync_run(fcom_op *op)
{
	struct sync *s = op;
	int r, rc = 1;
	enum {
		I_IN, I_OUT_INIT, I_OUT, I_DIFF, I_DIFF_SHOW, I_SYNC, I_SNAP_WR,
	};

	while (!FFINT_READONCE(s->stop)) {
		switch (s->st) {

		case I_IN: {
			fntree_entry *e;
			if (0 > (r = srcdst_next(s, &s->src, &s->name, &e))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					s->st = I_OUT_INIT;
					if (s->snapshot) {
						rc = 0;
						s->bftr = 1;
						s->st = I_SNAP_WR;
					}
					continue;
				}
				goto end;
			}

			struct data *d = fntree_data(e);

			if (r == RINPUT_NEW_DIR) {
				if (s->hdr)
					s->bftr = 1;
				s->bhdr = 1;
			}

			fffd fd = FFFILE_NULL;
			if (0 != f_info(s, s->name, &s->ent, &s->ent.d, &fd))
				goto end;
			*d = s->ent.d;

			s->ent.name = s->name;
			if (s->hdr)
				ffpath_splitpath(s->name.ptr, s->name.len, &s->dir, &s->ent.name);
			s->ent_ready = 1;

			if (fd != FFFILE_NULL)
				if (0 != srcdst_add_dir(&s->src, fd))
					goto end;

			if (s->snapshot)
				s->st = I_SNAP_WR;
			continue;
		}

		case I_OUT_INIT: {
			ffstr rtpath = {};
			s->dst.root = fntree_create(rtpath);
			if (NULL == fntree_add(&s->dst.root, s->cmd->output, sizeof(struct data)))
				goto end;
			s->st = I_OUT;
		}
			// fallthrough

		case I_OUT: {
			ffstr name;
			fntree_entry *e;
			if (0 > (r = srcdst_next(s, &s->dst, &name, &e))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					diff_init(s);
					s->st = I_DIFF;
					continue;
				}
				goto end;
			}

			struct data *d = fntree_data(e);
			struct ent ent;
			fffd fd = FFFILE_NULL;
			if (0 != f_info(s, name, &ent, d, &fd))
				goto end;

			if (fd != FFFILE_NULL)
				if (0 != srcdst_add_dir(&s->dst, fd))
					goto end;
			continue;
		}

		case I_DIFF:
			if (0 != diff_next(s)) {
				s->st = I_DIFF_SHOW;
			}
			continue;

		case I_DIFF_SHOW:
			if (0 != diff_show_next(s)) {
				if (s->diff) {
					rc = 0;
					goto end;
				}
				s->cmp_idx = 0;
				s->st = I_SYNC;
			}
			continue;

		case I_SYNC:
			if (0 != (r = sync1(s))) {
				if (r == 0x123)
					return;
				rc = 0;
				goto end;
			}
			continue;

		case I_SNAP_WR: {
			ffstr data;
			if (!s->hdr) {
				s->hdr = 1;
				data = snap_hdr();

				if (s->snap == NULL) {
					struct fcom_file_conf fc = {};
					fc.buffer_size = s->cmd->buffer_size;
					s->snap = core->file->create(&fc);
					uint flags = fcom_file_cominfo_flags_o(s->cmd);
					flags |= FCOM_FILE_WRITE;
					r = core->file->open(s->snap, s->cmd->output.ptr, flags);
					if (r == FCOM_FILE_ERR) goto end;
				}

			} else if (s->bftr) {
				s->bftr = 0;
				data = snap_bftr();

			} else if (s->bhdr) {
				s->bhdr = 0;
				snap_bhdr(&s->buf, s->dir);
				ffstr_setstr(&data, &s->buf);
				s->buf.len = 0;

			} else if (s->ent_ready) {
				s->ent_ready = 0;
				snap_ent_serialize(&s->buf, &s->ent);
				ffstr_setstr(&data, &s->buf);
				s->buf.len = 0;

			} else {
				s->st = I_IN;
				continue;
			}

			r = core->file->write(s->snap, data, -1);
			if (r == FCOM_FILE_ERR) goto end;

			if (rc == 0)
				goto end;

			continue;
		}
		}
	}

end:
	fcom_cominfo *cmd = s->cmd;
	sync_close(s);
	core->com->destroy(cmd);
	core->exit(rc);
}

static void sync_signal(fcom_op *op, uint signal)
{
	struct sync *s = op;
	FFINT_WRITEONCE(s->stop, 1);
}

static const fcom_operation fcom_op_sync = {
	sync_create, sync_close,
	sync_run, sync_signal,
	sync_help,
};


static void sync_init(const fcom_core *_core) { core = _core; }
static void sync_destroy() {}
static const fcom_operation* sync_provide_op(const char *name)
{
	if (ffsz_eq(name, "sync"))
		return &fcom_op_sync;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER,
	sync_init, sync_destroy, sync_provide_op,
};
