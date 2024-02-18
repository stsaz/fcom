/** fcom: analyze text files
2022, Simon Zolin */

static const char* txcnt_help()
{
	return "\
Analyze text files (e.g. print number of lines).\n\
Usage:\n\
  fcom textcount INPUT... [OPTIONS]\n\
";
}

#include <fcom.h>

static const fcom_core *core;

struct txcnt_stat {
	uint64 sz;
	uint64 ln, ln_empty;
	uint64 b_ln, b_ln_max;
};

struct txcnt {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *in;
	ffstr data;
	ffstr iname;
	uint no_hdr :1;

	uint64 f;
	uint64 sz_f_min, sz_f_max;
	uint64 ln_f_max;
	struct txcnt_stat all, cur;
};

static int args_parse(struct txcnt *c, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT))
		return -1;

	if (c->cmd->output.len == 0)
		c->cmd->stdout = 1;

	return 0;
}

static void txcnt_close(fcom_op *op)
{
	struct txcnt *c = op;
	core->file->destroy(c->in);
	ffmem_free(c);
}

static fcom_op* txcnt_create(fcom_cominfo *cmd)
{
	struct txcnt *c = ffmem_new(struct txcnt);
	c->cmd = cmd;

	if (0 != args_parse(c, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	c->in = core->file->create(&fc);

	c->sz_f_min = (uint64)-1;
	return c;

end:
	txcnt_close(c);
	return NULL;
}

/** Set the minimum value.
The same as: dst = min(dst, src) */
#define ffint_setmin(dst, src) \
do { \
	if ((dst) > (src)) \
		(dst) = (src); \
} while (0)

/** Set the maximum value.
The same as: dst = max(dst, src) */
#define ffint_setmax(dst, src) \
do { \
	if ((dst) < (src)) \
		(dst) = (src); \
} while (0)

/** Count lines in buffer, bytes in line. */
static void txcnt_analyze(struct txcnt_stat *f, ffstr ss)
{
	f->sz += ss.len;
	for (;;) {
		ffssize lf = ffstr_findchar(&ss, '\n');
		if (lf < 0) {
			f->b_ln += ss.len;
			break;
		}

		f->b_ln += lf;
		ffstr_shift(&ss, lf + 1);
		ffint_setmax(f->b_ln_max, f->b_ln);
		f->ln++;
		if (f->b_ln == 0)
			f->ln_empty++;
		f->b_ln = 0;
	}
}

/** Print file stats. */
static void txcnt_print(struct txcnt *c, struct txcnt_stat *f)
{
	if (!c->no_hdr) {
		c->no_hdr = 1;
		fcom_verblog("size       lines      non-empty      max-line-width");
	}

	uint empty = f->ln - f->ln_empty;
	uint empty_perc = FFINT_DIVSAFE(empty * 100, f->ln);
	fcom_verblog("%10U %10U %10U(%2u%%) %10U %s"
		, f->sz, f->ln
		, empty, empty_perc
		, f->b_ln_max
		, c->iname.ptr);
}

/** Aggregate file stats into overall stats. */
static void txcnt_add(struct txcnt *c, const struct txcnt_stat *f)
{
	c->all.sz += f->sz;
	c->all.ln += f->ln;
	c->all.ln_empty += f->ln_empty;
	ffint_setmax(c->ln_f_max, f->ln);
	ffint_setmin(c->sz_f_min, f->sz);
	ffint_setmax(c->sz_f_max, f->sz);
	c->f++;
}

static void txcnt_f_clear(struct txcnt_stat *f)
{
	ffmem_zero_obj(f);
}

static void txcnt_final_stats(struct txcnt *c)
{
	struct txcnt_stat *a = &c->all;
	uint empty = a->ln - a->ln_empty;
	uint empty_perc = FFINT_DIVSAFE(empty * 100, a->ln);
	fcom_infolog("Files: %U, %U bytes (%D..%U), size/file:%U\n"
		"Lines: %U (max:%U), non-empty:%U(%u%%), lines/file:%U"
		, c->f, a->sz, c->sz_f_min, c->sz_f_max, FFINT_DIVSAFE(a->sz, c->f)
		, a->ln, c->ln_f_max, empty, empty_perc, FFINT_DIVSAFE(a->ln, c->f));
}

static void txcnt_run(fcom_op *op)
{
	struct txcnt *c = op;
	int r, rc = 1;
	enum { I_NEXTFILE, I_READ, };

	while (!FFINT_READONCE(c->stop)) {
		switch (c->st) {

		case I_NEXTFILE: {
			if (0 > (r = core->com->input_next(c->cmd, &c->iname, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					txcnt_final_stats(c);
					rc = 0;
				}
				goto end;
			}

			uint iflags = fcom_file_cominfo_flags_i(c->cmd);
			r = core->file->open(c->in, c->iname.ptr, iflags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi;
			r = core->file->info(c->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(c->cmd, c->iname, fffile_isdir(fffileinfo_attr(&fi))))
				continue;

			if (fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(c->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(c->cmd, fd);
				continue;
			}

			txcnt_f_clear(&c->cur);
			c->st = I_READ;
			continue;
		}

		case I_READ:
			r = core->file->read(c->in, &c->data, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				struct txcnt_stat *f = &c->cur;
				if (f->b_ln != 0)
					f->ln++;

				txcnt_print(c, f);
				txcnt_add(c, f);

				c->st = I_NEXTFILE;
				continue;
			}

			txcnt_analyze(&c->cur, c->data);
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	txcnt_close(c);
	core->com->complete(cmd, rc);
	}
}

static void txcnt_signal(fcom_op *op, uint signal)
{
	struct txcnt *c = op;
	FFINT_WRITEONCE(c->stop, 1);
}

static const fcom_operation fcom_op_textcount = {
	txcnt_create, txcnt_close,
	txcnt_run, txcnt_signal,
	txcnt_help,
};

FCOM_MOD_DEFINE(textcount, fcom_op_textcount, core)
