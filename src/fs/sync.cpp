/** fcom: synchronize directories
2022, Simon Zolin */

static const char* sync_help()
{
	return "\
Compare/synchronize directories or create a file tree snapshot.\n\
Implies '--recursive', ignores '--include'/'--exclude'.\n\
Usage:\n\
  `fcom sync` [INPUT_DIR -o OUT_DIR]|[SNAPSHOT --source-snap -o OUT_DIR]|[INPUT_DIR -s -o SNAPSHOT] [OPTIONS]\n\
\n\
OPTIONS:\n\
    `-s`, `--snapshot`      Create an INPUT_DIR tree snapshot\n\
        `--source-snap`   Use snapshot file for input file tree\n\
        `--target-snap`   Use snapshot file for output file tree\n\
\n\
    `-d`, `--diff` STR      Just show difference table between source and target\n\
                        STR: empty (\"\") or a set of flags [ADUM]\n\
        `--diff-no-dir`   diff: Don't compare directory entries\n\
        `--diff-no-attr`  diff: Don't check file attributes\n\
        `--diff-no-time`  diff: Don't check file time\n\
        `--diff-time-sec` diff: File time granularity is 2 seconds\n\
        `--diff-fullname` diff: Don't cut file names\n\
    `-p`, `--plain`         Plain list of file names\n\
\n\
        `--add`           Copy new files\n\
        `--delete`        Delete old files\n\
        `--update`        Overwrite modified files\n\
        `--move`          Move files\n\
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
#include <util/util.hpp>
#include <ffsys/path.h>
#include <ffsys/globals.h>
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
	xxvec name;
	uint64 total;

	~srcdst() {
		fntree_free_all(root);
	}
};

struct sync {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	struct srcdst	src, dst;
	fcom_filexx		input;
	ffstr			name, dir;
	struct ent		ent;
	uint			stop;
	uint			hdr :1;
	uint			ent_ready :1;

	struct {
		uint state;
		xxvec		ibuf;
		ffstr data;
		struct ent ent;
		fntree_block *curblock;
		fntree_entry *cur_ent;
		struct srcdst *sd;
		fntree_cursor cur;
		const char *fn;
	} sr;

	struct sw_s {
		fcom_filexx	snap;
		xxvec		buf;
		uint bhdr :1;
		uint bftr :1;
		sw_s() : snap(core) {}
	} sw;

	struct {
		fntree_cmp fcmp;
		xxvec		ents; // fntree_cmp_ent[]
		ffmap moved; // hash(name, struct entdata) -> fntree_cmp_ent*
		xxvec		lname, rname;
		uint cmp_idx;
		struct {
			uint eq, left, right, neq, moved;
		} stats;
	} cmp;

	struct {
		uint cmp_idx;
		xxvec		lname, rname;
		struct {
			uint add, del, overwritten, moved;
		} stats;
	} sc;

	char *diff_flags_str;
	u_char diff_flags; // enum DIFF_FLAGS
	u_char sync_add, sync_del, sync_update, sync_move;
	u_char diff_only;
	u_char plain_list;
	u_char write_snapshot;
	u_char left_snapshot, right_snapshot;
	u_char left_path_strip, right_path_strip;
	u_char diff_no_dir, diff_no_attr, diff_no_time, diff_time_2sec;
	u_char diff_full_name;

	sync() : input(core) {}

	int left_tree_init()
	{
		ffstr rtpath = {};
		this->src.root = fntree_create(rtpath);
		ffstr *it;
		FFSLICE_WALK(&this->cmd->input, it) {
			if (NULL == fntree_add(&this->src.root, *it, sizeof(struct entdata)))
				return -1;
		}
		if (this->cmd->input.len == 1)
			this->src.root_dir = *ffslice_itemT(&this->cmd->input, 0, ffstr);

		struct fcom_file_conf fc = {};
		fc.buffer_size = this->cmd->buffer_size;
		this->input.create(&fc);
		fcom_infolog("Scanning source...");
		return 0;
	}

	int right_tree_init()
	{
		ffstr rtpath = {};
		this->dst.root = fntree_create(rtpath);
		if (NULL == fntree_add(&this->dst.root, this->cmd->output, sizeof(struct entdata)))
			return -1;

		this->dst.root_dir = this->cmd->output;

		if (!this->input.f) {
			struct fcom_file_conf fc = {};
			fc.buffer_size = this->cmd->buffer_size;
			this->input.create(&fc);
		}

		fcom_infolog("Scanning target...");
		return 0;
	}

	static const int RINPUT_NEW_DIR = 0x10;

	/** Get next file from tree */
	static int srcdst_next(struct srcdst *sd, ffstr *name, fntree_entry **e)
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

		if (0 != ffdirscan_open(&ds, (char*)sd->name.ptr, flags)) {
			fcom_syserrlog("ffdirscan_open");
			return -1;
		}
		ffstr path = FFSTR_INITZ((char*)sd->name.ptr);
		fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct entdata));
		sd->total += b->entries;
		fntree_attach((fntree_entry*)sd->cur.cur, b);
		fcom_dbglog("added branch '%S' [%u]", &path, b->entries);
		ffdirscan_close(&ds);
		return 0;
	}

	/** Get file attributes
	fd: [output] directory descriptor */
	int f_info(ffstr name, struct ent *e, struct entdata *d, fffd *fd)
	{
		uint flags = fcom_file_cominfo_flags_i(this->cmd);
		flags |= FCOM_FILE_READ;
		int r = this->input.open(name.ptr, flags);
		if (r == FCOM_FILE_ERR) return -1;

		xxfileinfo fi;
		r = this->input.info(&fi);
		if (r == FCOM_FILE_ERR) return -1;

		ffmem_zero_obj(e);
		e->type = 'f';

		if (fi.dir()) {
			e->type = 'd';
			*fd = this->input.acquire_fd();
		}

		this->input.close();

		d->size = fi.size();
		d->mtime = fi.mtime1();
		d->mtime.nsec = (d->mtime.nsec / 1000000) * 1000000;

	#ifdef FF_UNIX
		d->unixattr = fi.attr();
		d->uid = fi.info.st_uid;
		d->gid = fi.info.st_gid;
		d->winattr = 0;
		if (e->type == 'd')
			d->winattr |= FFFILE_WIN_DIR;
	#else
		d->winattr = fi.attr();
		d->unixattr = 0;
		if (e->type == 'd')
			d->unixattr |= FFFILE_UNIX_DIR;
	#endif

		// d->crc32 = ;

		return 0;
	}

	int left_tree_scan_next()
	{
		int r;
		fntree_entry *e;
		if (0 > (r = srcdst_next(&this->src, &this->name, &e))) {
			if (r == FCOM_COM_RINPUT_NOMORE) {
				return 'done';
			}
			return 'erro';
		}

		struct entdata *d = (entdata*)fntree_data(e);

		if (r == RINPUT_NEW_DIR) {
			if (this->hdr)
				this->sw.bftr = 1;
			this->sw.bhdr = 1;
		}

		fffd fd = FFFILE_NULL;
		if (0 != this->f_info(this->name, &this->ent, &this->ent.d, &fd))
			return 'erro';
		*d = this->ent.d;

		this->ent.name = this->name;
		if (this->hdr)
			ffpath_splitpath(this->name.ptr, this->name.len, &this->dir, &this->ent.name);
		this->ent_ready = 1;

		if (fd != FFFILE_NULL)
			if (0 != srcdst_add_dir(&this->src, fd))
				return 'erro';
		return 0;
	}

	int right_tree_scan_next()
	{
		int r;
		ffstr name;
		fntree_entry *e;
		if (0 > (r = srcdst_next(&this->dst, &name, &e))) {
			if (r == FCOM_COM_RINPUT_NOMORE) {
				return 'done';
			}
			return 'erro';
		}

		struct entdata *d = (entdata*)fntree_data(e);
		struct ent ent;
		fffd fd = FFFILE_NULL;
		if (0 != this->f_info(name, &ent, d, &fd))
			return 'erro';

		if (fd != FFFILE_NULL)
			if (0 != srcdst_add_dir(&this->dst, fd))
				return 'erro';
		return 0;
	}
};

static void sync_close(fcom_op *op);
#include <fs/sync-cmp.h>
#include <fs/sync-fsync.h>
#include <fs/sync-rsnap.h>
#include <fs/sync-wsnap.h>

static int diff_flags_parse(const char *s);

#define O(member)  (void*)FF_OFF(struct sync, member)

static int args_parse(struct sync *s, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--add",				'1',	O(sync_add) },
		{ "--delete",			'1',	O(sync_del) },
		{ "--diff",				's',	O(diff_flags_str) },
		{ "--diff-fullname",	'1',	O(diff_full_name) },
		{ "--diff-no-attr",		'1',	O(diff_no_attr) },
		{ "--diff-no-dir",		'1',	O(diff_no_dir) },
		{ "--diff-no-time",		'1',	O(diff_no_time) },
		{ "--diff-time-sec",	'1',	O(diff_time_2sec) },
		{ "--move",				'1',	O(sync_move) },
		{ "--plain",			'1',	O(plain_list) },
		{ "--snapshot",			'1',	O(write_snapshot) },
		{ "--source-snap",		'1',	O(left_snapshot) },
		{ "--target-snap",		'1',	O(right_snapshot) },
		{ "--update",			'1',	O(sync_update) },
		{ "-d",					's',	O(diff_flags_str) },
		{ "-p",					'1',	O(plain_list) },
		{ "-s",					'1',	O(write_snapshot) },
		{}
	};
	if (0 != core->com->args_parse(cmd, args, s, FCOM_COM_AP_INOUT))
		return -1;

	cmd->recursive = 1;

	if (s->left_snapshot && s->write_snapshot) {
		fcom_fatlog("'--snapshot' and '--source-snap' can't be used together");
		return -1;
	}

	s->left_path_strip = s->right_path_strip = (!s->write_snapshot);

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
	struct sync *s = new(ffmem_new(struct sync)) struct sync;
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
	struct sync *s = (struct sync*)op;
	cmp_destroy(s);
	rsnap_destroy(s);
	s->~sync();
	ffmem_free(s);
}

static void sync_run(fcom_op *op)
{
	struct sync *s = (struct sync*)op;
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

		case I_IN_PREP:
			if (s->left_tree_init()) goto end;
			s->st = I_IN;
			// fallthrough

		case I_IN:
			switch (s->left_tree_scan_next()) {
			case 'done':
				s->st = I_OUT_INIT;
				if (s->write_snapshot) {
					rc = 0;
					s->sw.bftr = 1;
					s->st = I_SNAP_WR;
				}
				continue;

			case 'erro':
				goto end;
			}

			if (s->write_snapshot)
				s->st = I_SNAP_WR;
			continue;

		case I_LSNAP: {
			if (s->cmd->input.len == 0) {
				fcom_fatlog("Please specify input snapshot file");
				goto end;
			}

			ffstr *in = (ffstr*)s->cmd->input.ptr;
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

			if (s->right_tree_init()) goto end;
			s->st = I_OUT;
		}
			// fallthrough

		case I_OUT:
			switch (s->right_tree_scan_next()) {
			case 'done':
				s->st = I_DIFF_BEGIN;
				continue;

			case 'erro':
				goto end;
			}

			continue;

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
	struct sync *s = (struct sync*)op;
	FFINT_WRITEONCE(s->stop, 1);
}

static const fcom_operation fcom_op_sync = {
	sync_create, sync_close,
	sync_run, sync_signal,
	sync_help,
};

FCOM_MOD_DEFINE(sync, fcom_op_sync, core)
