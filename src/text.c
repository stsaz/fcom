/** Text operations.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/data/utf8.h>
#include <FF/number.h>
#include <FF/path.h>

#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define verblog(fmt, ...)  fcom_verblog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;
extern const fcom_command *com;

// TEXTCOUNT
static void* txcnt_open(fcom_cmd *cmd);
static void txcnt_close(void *p, fcom_cmd *cmd);
static int txcnt_process(void *p, fcom_cmd *cmd);
const fcom_filter txcnt_filt = {
	&txcnt_open, &txcnt_close, &txcnt_process,
};

struct tcount;
struct txcnt_stat;
static void txcnt_analyze(struct txcnt_stat *f, ffstr *ss);
static void txcnt_print(struct txcnt_stat *f, fcom_cmd *cmd);
static void txcnt_add(struct tcount *c, const struct txcnt_stat *f);

// UTF8
static void* utf8_open(fcom_cmd *cmd);
static void utf8_close(void *p, fcom_cmd *cmd);
static int utf8_process(void *p, fcom_cmd *cmd);
const fcom_filter utf8_filt = {
	&utf8_open, &utf8_close, &utf8_process,
};

static void path_split3(const char *fullname, size_t len, ffstr *path, ffstr *name, ffstr *ext);


#define FILT_NAME  "f-textcount"

struct txcnt_stat {
	uint64 sz;

	uint64 ln;
	uint64 ln_empty;

	uint64 b_ln;
	uint64 b_ln_min;
	uint64 b_ln_max;
};

struct tcount {
	uint state;

	uint64 f;
	uint64 sz_f_min;
	uint64 sz_f_max;
	uint64 ln_f_min;
	uint64 ln_f_max;
	struct txcnt_stat all;
	struct txcnt_stat cur;
};

static void* txcnt_open(fcom_cmd *cmd)
{
	struct tcount *c;
	if (NULL == (c = ffmem_new(struct tcount)))
		return NULL;
	c->ln_f_min = (uint64)-1;
	c->sz_f_min = (uint64)-1;
	return c;
}

static void txcnt_close(void *p, fcom_cmd *cmd)
{
	struct tcount *c = p;
	struct txcnt_stat *a = &c->all;
	uint nempty = a->ln - a->ln_empty;
	uint nempty_perc = FFINT_DIVSAFE(nempty * 100, a->ln);
	fcom_infolog(FILT_NAME, "Files: %U, %U bytes (%D..%U), size/file:%U\n"
		"Lines: %U (%D..%U), non-empty:%U(%u%%), lines/file:%U"
		, c->f, a->sz, c->sz_f_min, c->sz_f_max, FFINT_DIVSAFE(a->sz, c->f)
		, a->ln, c->ln_f_min, c->ln_f_max, nempty, nempty_perc, FFINT_DIVSAFE(a->ln, c->f));

	ffmem_free(c);
}

/** Count stats for each input file. */
static int txcnt_process(void *p, fcom_cmd *cmd)
{
	enum { I_NEXTFILE, I_DATA, };
	struct tcount *c = p;

	switch (c->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		c->state = I_DATA;

		ffmem_tzero(&c->cur);
		c->cur.b_ln_min = (uint64)-1;

		return FCOM_MORE;

	case I_DATA:
		break;
	}

	struct txcnt_stat *f = &c->cur;
	txcnt_analyze(f, &cmd->in);

	if (cmd->in_last) {
		if (f->b_ln != 0)
			f->ln++;

		txcnt_print(f, cmd);
		txcnt_add(c, f);
		c->state = I_NEXTFILE;
	}

	return FCOM_MORE;
}

/** Count lines in buffer, bytes in line. */
static void txcnt_analyze(struct txcnt_stat *f, ffstr *ss)
{
	const char *s = ss->ptr, *end = ffarr_end(ss), *lf;
	f->sz += ss->len;
	for (;;) {
		lf = ffs_find(s, end - s, '\n');
		f->b_ln += lf - s;
		if (lf == end)
			break;
		s = lf + 1;
		ffint_setmin(f->b_ln_min, f->b_ln);
		ffint_setmax(f->b_ln_max, f->b_ln);
		f->ln++;
		if (f->b_ln == 0)
			f->ln_empty++;
		f->b_ln = 0;
	}
}

/** Print file stats. */
static void txcnt_print(struct txcnt_stat *f, fcom_cmd *cmd)
{
	uint nempty = f->ln - f->ln_empty;
	uint nempty_perc = FFINT_DIVSAFE(nempty * 100, f->ln);
	fcom_verblog(FILT_NAME, "%s: size:%U  lines:%U  non-empty:%U(%u%%)  line-width:(%D..%U)"
		, cmd->input.fn, f->sz, f->ln, nempty, nempty_perc
		, f->b_ln_min, f->b_ln_max);
}

/** Aggregate file stats into overall stats. */
static void txcnt_add(struct tcount *c, const struct txcnt_stat *f)
{
	c->all.sz += f->sz;
	c->all.ln += f->ln;
	c->all.ln_empty += f->ln_empty;
	ffint_setmin(c->ln_f_min, f->ln);
	ffint_setmax(c->ln_f_max, f->ln);
	ffint_setmin(c->sz_f_min, f->sz);
	ffint_setmax(c->sz_f_max, f->sz);
	c->f++;
}

#undef FILT_NAME


#define FILT_NAME  "utf8"

struct utf8 {
	ffarr fdata;
	ffarr out;
	const char *ofn;
	ffarr fn;
};

static void* utf8_open(fcom_cmd *cmd)
{
	struct utf8 *u = ffmem_new(struct utf8);
	return u;
}

static void utf8_close(void *p, fcom_cmd *cmd)
{
	struct utf8 *u = p;
	ffarr_free(&u->fdata);
	ffarr_free(&u->out);
	ffarr_free(&u->fn);
	ffmem_free(u);
}

// Note: uses much memory (reads the whole file, writes the whole file)
static int utf8_process(void *p, fcom_cmd *cmd)
{
	struct utf8 *u = p;
	const char *fn;
	int coding;
	ffstr data;

	if (u->ofn == NULL) {
		if (cmd->output.fn == NULL) {
			errlog("output file isn't set", 0);
			return FCOM_ERR;
		}
		u->ofn = cmd->output.fn;
	}

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_DONE;
		if (0 != fffile_readall(&u->fdata, fn, (uint64)-1))
			return FCOM_SYSERR;
		ffstr_set2(&data, &u->fdata);

		size_t n = data.len;
		coding = ffutf_bom(data.ptr, &n);
		if (coding != FFU_UTF16LE && coding != FFU_UTF16BE) {
			fcom_infolog(FILT_NAME, "skipping file %s", fn);
			continue;
		}
		ffstr_shift(&data, n);

		if (0 == ffutf8_strencode(&u->out, data.ptr, data.len, coding)) {
			errlog("ffutf8_strencode", 0);
			return FCOM_ERR;
		}

		ffstr idir, iname, iext;
		ffstr odir, oname, oext;
		ffstr_setz(&iname, fn);
		ffpath_split3(iname.ptr, iname.len, &idir, &iname, &iext);
		ffstr_setz(&oname, u->ofn);
		path_split3(oname.ptr, oname.len, &odir, &oname, &oext);

		cmd->output.fn = u->ofn;
		if (oname.len == 0) {
			if (0 != ffpath_makefn_out(&u->fn, &idir, &iname, &odir, &oext))
				return FCOM_SYSERR;
			cmd->output.fn = u->fn.ptr;
		}

		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
		ffstr_set2(&cmd->out, &u->out);
		return FCOM_NEXTDONE;
	}
}

/** The same as ffpath_split3() except that for fullname="path/.ext" ".ext" is treated as extension. */
static void path_split3(const char *fullname, size_t len, ffstr *path, ffstr *name, ffstr *ext)
{
	ffpath_split2(fullname, len, path, name);
	char *dot = ffs_rfind(name->ptr, name->len, '.');
	ffs_split2(name->ptr, name->len, dot, name, ext);
}

#undef FILT_NAME
