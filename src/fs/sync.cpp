/** fcom: synchronize directories
2022, Simon Zolin */

static const char* sync_help()
{
	return "\
Compare/synchronize directories or create a file tree snapshot.\n\
Implies '--recursive'.\n\
Usage:\n\
  `fcom sync` [INPUT_DIR -o OUT_DIR]|[SNAPSHOT --source-snap -o OUT_DIR]|[INPUT_DIR -s -o SNAPSHOT] [OPTIONS]\n\
\n\
OPTIONS:\n\
    `-s`, `--snapshot`      Create an INPUT_DIR tree snapshot\n\
        `--zip-expand`    Treat .zip files as directories\n\
        `--source-snap`   Use snapshot file for input file tree\n\
        `--target-snap`   Use snapshot file for output file tree\n\
\n\
    `-d`, `--diff` STR      Just show difference table between source and target\n\
                        STR: empty (\"\") or a set of flags [ADUM]\n\
        `--diff-no-attr`  diff: Don't check file attributes\n\
        `--diff-no-time`  diff: Don't check file time\n\
        `--diff-time-sec` diff: File time granularity is 2 seconds\n\
        `--diff-no-dir`   diff: Don't show directory entries\n\
        `--diff-fullname` diff: Don't cut file names\n\
        `--recent` DAYS   Only show files less than DAYS days old\n\
    `-p`, `--plain`         Plain list of file names\n\
\n\
        `--add`           Copy new files\n\
        `--delete`        Delete old files\n\
        `--update`        Overwrite modified files\n\
        `--move`          Move files\n\
        `--replace-date`  Just copy file date (don't overwrite content).  Use with `--update`.\n\
        `--write-into`    Pass option to `copy`\n\
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
	f "file" size unix_attr/win_attr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	d "dir1" size unix_attr/win_attr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
}

b "/dir/dir1" {
	...
}
*/

#include <fcom.h>
#include <ffbase/fntree.h>
#include <util/util.hpp>
#include <ffsys/path.h>
#include <ffsys/globals.h>
#include <ffbase/map.h>

static const fcom_core *core;

#include <fs/sync-scan.h>
#include <fs/sync-rsnap.h>
#include <fs/sync-cmp.h>

struct wsnap {
	fcom_filexx	snap;
	xxvec		buf;
	uint		bhdr :1;
	uint		bftr :1;

	wsnap() : snap(core) {}
	void init(struct sync *s);
	int process(struct sync *s, const struct ent& e);
};

struct sync {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	const fcom_sync_if *sync_if;
	snapshot		*src, *dst;
	fcom_filexx		input;
	ffstr			dir;
	struct ent		ent;
	uint			cmp_idx;
	uint			stop;
	uint			hdr :1;
	uint			ent_ready :1;

	struct rsnap sr;
	struct diff cmp;
	struct wsnap sw;

	struct {
		uint cmp_idx;
		xxvec		lname, rname;
		struct {
			uint add, del, overwritten, moved;
		} stats;
	} sc;

	char *diff_flags_str;
	uint	diff_flags; // enum FCOM_SYNC
	u_char	sync_add, sync_del, sync_update, sync_move;
	u_char	diff_only;
	u_char	plain_list;
	u_char	write_snapshot;
	u_char	left_snapshot, right_snapshot;
	u_char	left_path_strip, right_path_strip;
	u_char	diff_no_dir, diff_no_attr, diff_no_time, diff_time_2sec;
	u_char	diff_full_name;
	u_char	replace_date;
	u_char	zip_expand;
	u_char	write_into;
	uint	recent_days;
	fftime	since_time;

	sync() : input(core) {}
	~sync() {
		if (src) src->~snapshot();
		ffmem_free(src);
		if (dst) dst->~snapshot();
		ffmem_free(dst);
	}

	int left_tree_init()
	{
		this->src = ffmem_new(struct snapshot);
		this->src->zip_expand = this->zip_expand;
		ffstr rtpath = {};
		this->src->root = fntree_create(rtpath);
		ffstr *it;
		FFSLICE_WALK(&this->cmd->input, it) {
			if (NULL == fntree_add(&this->src->root, *it, sizeof(struct fcom_sync_entry)))
				return -1;
		}
		if (this->cmd->input.len == 1) {
			ffstr *s = ffslice_itemT(&this->cmd->input, 0, ffstr);
			ffstr_dupstr(&this->src->root_dir, s);
		}

		struct fcom_file_conf fc = {};
		fc.buffer_size = this->cmd->buffer_size;
		this->input.create(&fc);
		return 0;
	}

	int right_tree_init()
	{
		this->dst = ffmem_new(struct snapshot);
		this->dst->zip_expand = this->zip_expand;
		ffstr rtpath = {};
		this->dst->root = fntree_create(rtpath);
		if (NULL == fntree_add(&this->dst->root, this->cmd->output, sizeof(struct fcom_sync_entry)))
			return -1;

		ffstr_dupstr(&this->dst->root_dir, &this->cmd->output);

		if (!this->input.f) {
			struct fcom_file_conf fc = {};
			fc.buffer_size = this->cmd->buffer_size;
			this->input.create(&fc);
		}

		return 0;
	}

	void diff_begin()
	{
		fcom_infolog("Comparing source & target...");
		uint flags = 0;
		if (this->left_path_strip)
			flags |= FCOM_SYNC_DIFF_LEFT_PATH_STRIP;
		if (this->right_path_strip)
			flags |= FCOM_SYNC_DIFF_RIGHT_PATH_STRIP;
		if (this->diff_no_attr)
			flags |= FCOM_SYNC_DIFF_NO_ATTR;
		if (this->diff_no_time)
			flags |= FCOM_SYNC_DIFF_NO_TIME;
		if (this->diff_time_2sec)
			flags |= FCOM_SYNC_DIFF_TIME_2SEC;
		this->cmp.init(this->src, this->dst, flags);
	}

	void diff_fin()
	{
		fcom_infolog("diff status: moved:%u  add:%u  del:%u  upd:%u  eq:%u  total:%U/%U"
			, this->cmp.stats.moved, this->cmp.stats.left, this->cmp.stats.right, this->cmp.stats.neq, this->cmp.stats.eq
			, this->cmp.stats.ltotal, this->cmp.stats.rtotal);

		struct fcom_sync_props props = {};
		props.include = *(ffslice*)&this->cmd->include;
		props.exclude = *(ffslice*)&this->cmd->exclude;
		props.since_time = this->since_time;

		uint show_flags = this->diff_flags;
		this->sync_if->view(&this->cmp, &props, show_flags);
	}

	void status_print(const fcom_sync_diff_entry *de, ffstr lname, ffstr rname)
	{
		const char *clr_add = "", *clr_del = "", *clr_move = "", *clr_reset = "";
		if (core->stdout_color) {
			clr_move = FFSTD_CLR_I(FFSTD_BLUE);
			clr_add = FFSTD_CLR_I(FFSTD_GREEN);
			clr_del = FFSTD_CLR_I(FFSTD_RED);
			clr_reset = FFSTD_CLR_RESET;
		}

		if (!this->diff_full_name && this->src->root_dir.len) {
			if (lname.len)
				ffstr_shift((ffstr*)&lname, this->src->root_dir.len);
			if (rname.len)
				ffstr_shift((ffstr*)&rname, this->cmd->output.len);
		}

		switch (de->status & FCOM_SYNC_MASK) {
		case FCOM_SYNC_LEFT:
			fcom_infolog("%sADD       %*S  >>%s"
				, clr_add, WIDTH_NAME, &lname, clr_reset);
			break;

		case FCOM_SYNC_RIGHT:
			fcom_infolog("%sDEL       %*c  <<  %S%s"
				, clr_del, WIDTH_NAME, ' ', &rname, clr_reset);
			break;

		case FCOM_SYNC_MOVE:
			fcom_infolog("%sMOV       %*S  ->  %S%s"
				, clr_move, WIDTH_NAME, &lname, &rname, clr_reset);
			break;

		case FCOM_SYNC_NEQ: {
			int a1 = '.', a2 = '.', a3 = '.';

			uint st = de->status;
			if (st & FCOM_SYNC_NEWER)
				a1 = 'N';
			else if (st & FCOM_SYNC_OLDER)
				a1 = 'O';

			if (st & FCOM_SYNC_LARGER)
				a2 = '>';
			else if (st & FCOM_SYNC_SMALLER)
				a2 = '<';

			if (st & FCOM_SYNC_ATTR)
				a3 = 'A';

			const char *cmp = "!=";
			if (st & FCOM_SYNC_LARGER)
				cmp = "> ";
			else if (st & FCOM_SYNC_SMALLER)
				cmp = "< ";
			else if (st & FCOM_SYNC_NEWER)
				cmp = ">=";
			else if (st & FCOM_SYNC_OLDER)
				cmp = "<=";

			xxvec v;
			v.add_f("UPD[%c%c%c]  %*S  %s  %*S"
				, a1, a2, a3
				, WIDTH_NAME, &lname
				, cmp
				, WIDTH_NAME, &rname);

			if (st & (FCOM_SYNC_LARGER | FCOM_SYNC_SMALLER)) {
				v.add_f(" %" WIDTH_SIZE "U %" WIDTH_SIZE "U"
					, de->left->size, de->right->size);
			}

			if (st & FCOM_SYNC_ATTR) {
				uint la = de->left->unix_attr, ra = de->right->unix_attr;
#ifdef FF_WIN
				la = de->left->win_attr, ra = de->right->win_attr;
#endif
				v.add_f(" %xu %xu"
					, la, ra);
			}

			fcom_infolog("%S", &v);
			break;
		}
		}
	}

	/** Show next difference from 'cmp.ents' */
	int diff_show_next()
	{
		int rc = 1;
		fcom_sync_diff_entry de_s, *de = &de_s;
		if (this->sync_if->info(&this->cmp, this->cmp_idx++, 0, de))
			return 1;

		if (this->plain_list) {
			switch (de->status & FCOM_SYNC_MASK) {
			case FCOM_SYNC_LEFT:
				ffstdout_fmt("%S\n", &de->lname);  break;

			case FCOM_SYNC_RIGHT:
				ffstdout_fmt("%S\n", &de->rname);  break;

			case FCOM_SYNC_NEQ:
				ffstdout_fmt("%S\n", &de->lname);  break;

			case FCOM_SYNC_MOVE:
				ffstdout_fmt("%S\n", &de->lname);  break;
			}
			rc = 0;
			goto end;
		}

		switch (de->status & FCOM_SYNC_MASK) {
		case FCOM_SYNC_LEFT:
		case FCOM_SYNC_RIGHT:
		case FCOM_SYNC_NEQ:
		case FCOM_SYNC_MOVE:
			this->status_print(de, de->lname, de->rname);
			break;

		case FCOM_SYNC_EQ:
			break;

		default:
			FCOM_ASSERT(0);
			goto end;
		}

		rc = 0;

	end:
		fcom_sync_diff_entry_destroy(de);
		return rc;
	}
};

static void sync_close(fcom_op *op);
#include <fs/sync-fsync.h>
#include <fs/sync-wsnap.h>

static const struct fcom_sync_if sync_if = {
	sync_snapshot_open,
	sync_scan,
	sync_scan_wc,
	sync_snapshot_free,
	sync_diff,
	sync_diff_free,
	sync_view,
	sync_info,
	sync_info_id,
	sync_status,
	sync_sync,
};

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
		{ "--recent",			'u',	O(recent_days) },
		{ "--replace-date",		'1',	O(replace_date) },
		{ "--snapshot",			'1',	O(write_snapshot) },
		{ "--source-snap",		'1',	O(left_snapshot) },
		{ "--target-snap",		'1',	O(right_snapshot) },
		{ "--update",			'1',	O(sync_update) },
		{ "--write-into",		'1',	O(write_into) },
		{ "--zip-expand",		'1',	O(zip_expand) },
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

	if (cmd->output.len == 0 && !cmd->stdout) {
		fcom_fatlog("Please use '--output'");
		return -1;
	}

	if (!s->diff_no_dir)
		s->diff_flags |= FCOM_SYNC_DIR;

	if (s->recent_days) {
		s->since_time = core->clock(NULL, FCOM_CORE_UTC);
		s->since_time.sec -= s->recent_days * 60*60*24;
	}

	return 0;
}

#undef O

static int diff_flags_parse(const char *s)
{
	if (s[0] == '\0')
		return FCOM_SYNC_LEFT
			| FCOM_SYNC_RIGHT
			| FCOM_SYNC_EQ
			| FCOM_SYNC_NEQ
			| FCOM_SYNC_MOVE;

	int r = 0;
	for (uint i = 0;  s[i] != '\0';  i++) {
		switch (s[i]) {
		case 'A': r |= FCOM_SYNC_LEFT; break;
		case 'D': r |= FCOM_SYNC_RIGHT; break;
		case 'U': r |= FCOM_SYNC_NEQ; break;
		case 'M': r |= FCOM_SYNC_MOVE; break;
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
	s->sync_if = &sync_if;

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
			fcom_infolog("Scanning source...");
			s->st = I_IN;
			// fallthrough

		case I_IN:
			switch (s->src->scan_next(&s->ent)) {
			case 'nblk':
				if (s->hdr)
					s->sw.bftr = 1;
				s->sw.bhdr = 1;
				break;

			case 'done':
				s->st = I_OUT_INIT;
				if (s->write_snapshot) {
					rc = 0;
					s->sw.bftr = 1;
					s->st = I_SNAP_WR;
				}
				continue;

			case 'erro':
				continue;
			}

			s->ent_ready = 1;
			if (s->hdr) {
				s->dir = s->src->path;
				s->ent.name = s->src->name_segment;
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
			s->src = s->sync_if->open(fn, 0);
			if (!s->src) goto end;
			s->st = I_OUT_INIT;
			continue;
		}

		case I_OUT_INIT:
			if (s->right_snapshot) {
				s->dst = s->sync_if->open(s->cmd->output.ptr, 0);
				if (!s->dst) goto end;
				s->st = I_DIFF_BEGIN;
				continue;
			}

			if (s->right_tree_init()) goto end;
			fcom_infolog("Scanning target...");
			s->st = I_OUT;
			// fallthrough

		case I_OUT:
			switch (s->dst->scan_next(NULL)) {
			case 'done':
				s->st = I_DIFF_BEGIN;
				continue;

			case 'erro':
				continue;
			}

			continue;

		case I_DIFF_BEGIN:
			s->diff_begin();
			s->st = I_DIFF;
			// fallthrough

		case I_DIFF:
			if (s->cmp.next()) {
				s->diff_fin();
				s->st = I_DIFF_SHOW;
			}
			continue;

		case I_DIFF_SHOW:
			if (0 != s->diff_show_next()) {
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
			r = s->sw.process(s, s->ent);
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

static const fcom_operation* sync_provide_op(const char *name)
{
	if (ffsz_eq(name, "sync"))
		return &fcom_op_sync;
	if (ffsz_eq(name, "if"))
		return (fcom_operation*)&sync_if;
	return NULL;
}

FCOM_MOD_DEFINE1(sync, sync_provide_op, core)
