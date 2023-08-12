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
	...
}
*/

#include <fcom.h>
#include <util/fntree.h>
#include <util/ltconf.h>
#include <FFOS/path.h>
#include <FFOS/ffos-extern.h>
#include <ffbase/map.h>

static const fcom_core *core;

struct entdata {
	uint64 size;
	uint unixattr, winattr;
	uint uid, gid;
	fftime mtime; // UTC
	uint crc32;
};

struct ent {
	char type; // 'f', 'd'
	ffstr name;
	struct entdata d;
};

enum DIFF_FLAGS {
	DIFF_LEFT = 1,
	DIFF_RIGHT = 2,
	DIFF_MOD = 4,
	DIFF_MOVE = 8,
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
	ffstr root_dir;
	fntree_block *root, *parent_blk;
	fntree_cursor cur;
	ffvec name;
	uint64 total;
};

struct sync {
	uint st;
	fcom_cominfo *cmd;
	struct srcdst src, dst;
	fcom_file_obj *in;
	ffstr name, dir;
	struct ent ent;
	uint stop;
	uint hdr :1;
	uint ent_ready :1;

	struct {
		uint state;
		ffvec ibuf;
		ffstr data;
		struct ltconf conf;
		struct ent ent;
		fntree_block *curblock;
		fntree_entry *cur_ent;
		struct srcdst *sd;
		fntree_cursor cur;
		const char *fn;
	} sr;

	struct {
		fcom_file_obj *snap;
		ffvec buf;
		uint bhdr :1;
		uint bftr :1;
	} sw;

	struct {
		fntree_cmp fcmp;
		ffvec ents; // fntree_cmp_ent[]
		ffmap moved; // hash(name, struct entdata) -> fntree_cmp_ent*
		ffvec lname, rname;
		uint cmp_idx;
		struct {
			uint eq, left, right, neq, moved;
		} stats;
	} cmp;

	struct {
		uint cmp_idx;
		ffvec lname, rname;
		struct {
			uint add, del, overwritten, moved;
		} stats;
	} sc;

	char *diff_flags_str;
	byte diff_flags; // enum DIFF_FLAGS
	byte sync_add, sync_del, sync_update;
	byte diff_only;
	byte plain_list;
	byte write_snapshot;
	byte left_snapshot, right_snapshot;
	byte left_path_strip, right_path_strip;
	byte diff_no_attr, diff_no_time;
	byte diff_full_name;
};

static void sync_close(fcom_op *op);
#include <ops/sync-cmp.h>
#include <ops/sync-fsync.h>
#include <ops/sync-rsnap.h>
#include <ops/sync-wsnap.h>

static int diff_flags_parse(const char *s);

static const char* sync_help()
{
	return "\
Compare/synchronize directories or create a file tree snapshot.\n\
Implies '--recursive', ignores '--include'/'--exclude'.\n\
Usage:\n\
  fcom sync [INPUT_DIR -o OUT_DIR]|[SNAPSHOT --source-snap -o OUT_DIR]|[INPUT_DIR -s -o SNAPSHOT] [OPTIONS]\n\
    OPTIONS:\n\
    -s, --snapshot      Create an INPUT_DIR tree snapshot\n\
        --source-snap   Use snapshot file for input file tree\n\
        --target-snap   Use snapshot file for output file tree\n\
        --source-path-strip1\n\
                        Strip top-level path-component from source tree\n\
        --target-path-strip1\n\
                        Strip top-level path-component from target tree\n\
\n\
    -d, --diff=STR      Just show difference table between source and target\n\
                        STR: empty (\"\") or a set of flags [ADUM]\n\
        --diff-no-attr  diff: Don't check file attributes\n\
        --diff-no-time  diff: Don't check file time\n\
        --diff-fullname diff: Don't cut file names\n\
    -p, --plain         Plain list of file names\n\
\n\
        --add           Copy new files\n\
        --delete        Delete old files\n\
        --update        Overwrite modified files\n\
\n\
Examples:\n\
\n\
Compare 2 directories:\n\
  fcom sync /home --diff=\"\" -o /home2\n\
\n\
Create file tree snapshot of '/home' directory:\n\
  fcom sync /home --snapshot -o home.snap\n\
\n\
Use snapshot file and compare it with '/home2' directory:\n\
  fcom sync home.snap --source-snap --diff=\"\" -o /home2\n\
";
}

#define O(member)  FF_OFF(struct sync, member)

static int args_parse(struct sync *s, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 's',	"snapshot",	FFCMDARG_TSWITCH, O(write_snapshot) },
		{ 0,	"source-snap",	FFCMDARG_TSWITCH, O(left_snapshot) },
		{ 0,	"target-snap",	FFCMDARG_TSWITCH, O(right_snapshot) },
		{ 0,	"source-path-strip1",	FFCMDARG_TSWITCH, O(left_path_strip) },
		{ 0,	"target-path-strip1",	FFCMDARG_TSWITCH, O(right_path_strip) },

		{ 'd',	"diff",	FFCMDARG_TSTRZ, O(diff_flags_str) },
		{ 0,	"diff-no-attr",	FFCMDARG_TSWITCH, O(diff_no_attr) },
		{ 0,	"diff-no-time",	FFCMDARG_TSWITCH, O(diff_no_time) },
		{ 0,	"diff-fullname",	FFCMDARG_TSWITCH, O(diff_full_name) },
		{ 'p',	"plain",	FFCMDARG_TSWITCH, O(plain_list) },

		{ 0,	"add",	FFCMDARG_TSWITCH, O(sync_add) },
		{ 0,	"delete",	FFCMDARG_TSWITCH, O(sync_del) },
		{ 0,	"update",	FFCMDARG_TSWITCH, O(sync_update) },
		{}
	};
	if (0 != core->com->args_parse(cmd, args, s))
		return -1;

	cmd->recursive = 1;

	if (s->left_snapshot && s->write_snapshot) {
		fcom_fatlog("'--snapshot' and '--source-snap' can't be used together");
		return -1;
	}

	if (s->diff_flags_str != NULL) {
		s->diff_only = 1;
		int r = diff_flags_parse(s->diff_flags_str);
		if (r < 0) {
			fcom_fatlog("--diff: incorrect flags");
			return -1;
		}
		s->diff_flags = r;
	}

	if (s->plain_list && !s->diff_only) {
		fcom_fatlog("'--plain' requires '--diff'");
		return -1;
	}
	if (s->plain_list && !(s->sync_add || s->sync_del || s->sync_update) && !s->sync_del) {
		fcom_fatlog("'--plain' requires '--add', '--delete' or '--update'");
		return -1;
	}

	if (cmd->output.len == 0 && !cmd->stdout) {
		fcom_fatlog("Please use '--output'");
		return -1;
	}

	return 0;
}

#undef O

static int diff_flags_parse(const char *s)
{
	if (s[0] == '\0')
		return 0xff;

	int r = 0;
	for (uint i = 0;  s[i] != '\0';  i++) {
		switch (s[i]) {
		case 'A': r |= DIFF_LEFT; break;
		case 'D': r |= DIFF_RIGHT; break;
		case 'U': r |= DIFF_MOD; break;
		case 'M': r |= DIFF_MOVE; break;
		default:
			return -1;
		}
	}
	return r;
}

static fcom_op* sync_create(fcom_cominfo *cmd)
{
	struct sync *s = ffmem_new(struct sync);
	s->cmd = cmd;

	if (0 != args_parse(s, cmd))
		goto end;

	return s;

end:
	sync_close(s);
	return NULL;
}

static void sync_close(fcom_op *op)
{
	struct sync *s = op;
	core->file->destroy(s->in);

	ffvec_free(&s->src.name);
	fntree_free_all(s->src.root);

	ffvec_free(&s->dst.name);
	fntree_free_all(s->dst.root);

	cmp_destroy(s);
	fsync_destroy(s);
	rsnap_destroy(s);
	wsnap_destroy(s);
	ffmem_free(s);
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
	fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct entdata));
	sd->total += b->entries;
	fntree_attach((fntree_entry*)sd->cur.cur, b);
	fcom_dbglog("added branch '%S' [%u]", &path, b->entries);
	ffdirscan_close(&ds);
	return 0;
}

/** Get file attributes
fd: [output] directory descriptor */
static int f_info(struct sync *s, ffstr name, struct ent *e, struct entdata *d, fffd *fd)
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
	d->mtime = fffileinfo_mtime1(&fi);
	d->mtime.nsec = (d->mtime.nsec / 1000000) * 1000000;

#ifdef FF_UNIX
	d->unixattr = fffileinfo_attr(&fi);
	d->uid = fi.st_uid;
	d->gid = fi.st_gid;
	d->winattr = 0;
	if (e->type == 'd')
		d->winattr |= FFFILE_WIN_DIR;
#else
	d->winattr = fffileinfo_attr(&fi);
	d->unixattr = 0;
	if (e->type == 'd')
		d->unixattr |= FFFILE_UNIX_DIR;
#endif

	// d->crc32 = ;

	return 0;
}

static void sync_run(fcom_op *op)
{
	struct sync *s = op;
	int r, rc = 1;
	enum {
		I_INIT,
		I_IN_PREP, I_IN,
		I_LSNAP,
		I_OUT_INIT, I_OUT,
		I_DIFF_BEGIN, I_DIFF, I_DIFF_SHOW,
		I_SYNC,
		I_SNAP_WR,
	};

	while (!FFINT_READONCE(s->stop)) {
		switch (s->st) {

		case I_INIT:
			s->st = I_IN_PREP;
			if (s->left_snapshot)
				s->st = I_LSNAP;
			continue;

		case I_IN_PREP: {
			ffstr rtpath = {};
			s->src.root = fntree_create(rtpath);
			ffstr *it;
			FFSLICE_WALK(&s->cmd->input, it) {
				if (NULL == fntree_add(&s->src.root, *it, sizeof(struct entdata)))
					goto end;
			}
			if (s->cmd->input.len == 1)
				s->src.root_dir = *ffslice_itemT(&s->cmd->input, 0, ffstr);

			struct fcom_file_conf fc = {};
			fc.buffer_size = s->cmd->buffer_size;
			s->in = core->file->create(&fc);
			fcom_infolog("Scanning source...");
			s->st = I_IN;
		}
			// fallthrough

		case I_IN: {
			fntree_entry *e;
			if (0 > (r = srcdst_next(s, &s->src, &s->name, &e))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					s->st = I_OUT_INIT;
					if (s->write_snapshot) {
						rc = 0;
						s->sw.bftr = 1;
						s->st = I_SNAP_WR;
					}
					continue;
				}
				goto end;
			}

			struct entdata *d = fntree_data(e);

			if (r == RINPUT_NEW_DIR) {
				if (s->hdr)
					s->sw.bftr = 1;
				s->sw.bhdr = 1;
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

			if (s->write_snapshot)
				s->st = I_SNAP_WR;
			continue;
		}

		case I_LSNAP: {
			if (s->cmd->input.len == 0) {
				fcom_fatlog("Please specify input snapshot file");
				goto end;
			}

			ffstr *in = s->cmd->input.ptr;
			const char *fn = in[0].ptr;

			r = rsnap_read(s, fn, &s->sr.data);
			if (r == FCOM_FILE_ERR) goto end;
			if (0 != (r = rsnap_parse(s, &s->src, s->sr.data))) {
				if (r == 0xbad) goto end;
				s->st = I_OUT_INIT;
			}
			continue;
		}

		case I_OUT_INIT: {
			if (s->right_snapshot) {
				r = rsnap_read(s, s->cmd->output.ptr, &s->sr.data);
				if (r == FCOM_FILE_ERR) goto end;
				r = rsnap_parse(s, &s->dst, s->sr.data);
				if (r == 0xbad) goto end;
				s->st = I_DIFF_BEGIN;
				continue;
			}

			ffstr rtpath = {};
			s->dst.root = fntree_create(rtpath);
			if (NULL == fntree_add(&s->dst.root, s->cmd->output, sizeof(struct entdata)))
				goto end;

			if (s->in == NULL) {
				struct fcom_file_conf fc = {};
				fc.buffer_size = s->cmd->buffer_size;
				s->in = core->file->create(&fc);
			}

			fcom_infolog("Scanning target...");
			s->st = I_OUT;
		}
			// fallthrough

		case I_OUT: {
			ffstr name;
			fntree_entry *e;
			if (0 > (r = srcdst_next(s, &s->dst, &name, &e))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					s->st = I_DIFF_BEGIN;
					continue;
				}
				goto end;
			}

			struct entdata *d = fntree_data(e);
			struct ent ent;
			fffd fd = FFFILE_NULL;
			if (0 != f_info(s, name, &ent, d, &fd))
				goto end;

			if (fd != FFFILE_NULL)
				if (0 != srcdst_add_dir(&s->dst, fd))
					goto end;
			continue;
		}

		case I_DIFF_BEGIN:
			fcom_infolog("Comparing source & target...");
			diff_init(s);
			s->st = I_DIFF;
			// fallthrough

		case I_DIFF:
			if (0 != diff_next(s)) {
				s->st = I_DIFF_SHOW;
			}
			continue;

		case I_DIFF_SHOW:
			if (0 != diff_show_next(s)) {
				if (s->diff_only) {
					rc = 0;
					goto end;
				}
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

		case I_SNAP_WR:
			r = wsnap_process(s);
			if (r == 0xbad) goto end;
			if (rc == 0) goto end;
			s->st = I_IN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = s->cmd;
	sync_close(s);
	core->com->complete(cmd, rc);
	}
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
	FCOM_VER, FCOM_CORE_VER,
	sync_init, sync_destroy, sync_provide_op,
};
