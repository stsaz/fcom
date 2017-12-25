/** File operations.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/number.h>
#include <FF/crc.h>
#include <FFOS/file.h>


extern const fcom_core *core;
static const fcom_command *com;

// MODULE
static int f_sig(uint signo);
static const void* f_iface(const char *name);
static int f_conf(const char *name, ffpars_ctx *ctx);
const fcom_mod f_mod = {
	.sig = &f_sig, .iface = &f_iface, .conf = &f_conf,
};

static const char* next_file(fcom_cmd *cmd);

// TOUCH
static void* f_touch_open(fcom_cmd *cmd);
static void f_touch_close(void *p, fcom_cmd *cmd);
static int f_touch_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_touch_filt = {
	&f_touch_open, &f_touch_close, &f_touch_process,
};

// TEXTCOUNT
static void* f_tcnt_open(fcom_cmd *cmd);
static void f_tcnt_close(void *p, fcom_cmd *cmd);
static int f_tcnt_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_tcnt_filt = {
	&f_tcnt_open, &f_tcnt_close, &f_tcnt_process,
};

struct tcount;
struct tcnt_stat;
static void f_tcnt_analyze(struct tcnt_stat *f, ffstr *ss);
static void f_tcnt_print(struct tcnt_stat *f, fcom_cmd *cmd);
static void f_tcnt_add(struct tcount *c, const struct tcnt_stat *f);

// CRC
static void* f_crc_open(fcom_cmd *cmd);
static void f_crc_close(void *p, fcom_cmd *cmd);
static int f_crc_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_crc_filt = { &f_crc_open, &f_crc_close, &f_crc_process };


struct oper {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};

const fcom_filter wregfind_filt;

static const struct oper cmds[] = {
	{ "touch", "core.f-touch", &f_touch_filt },
	{ "textcount", "core.f-textcount", &f_tcnt_filt },
	{ "crc", "core.f-crc", &f_crc_filt },
	{ "wregfind", "core.wregfind", &wregfind_filt },
};

static const void* f_iface(const char *name)
{
	const struct oper *op;
	FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
		if (ffsz_eq(name, op->mod + FFSLEN("core.")))
			return op->iface;
	}
	return NULL;
}

static int f_conf(const char *name, ffpars_ctx *ctx)
{
	return 1;
}

static int f_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		com = core->iface("core.com");

		const struct oper *op;
		FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
			if (0 != com->reg(op->name, op->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGSTART:
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}


/** Get next input file, excluding directories.
Return NULL if no more files. */
static const char* next_file(fcom_cmd *cmd)
{
	const char *fn;
	fffileinfo fi;
	for (;;) {

		if (NULL == (fn = com->arg_next(cmd, 0)))
			return NULL;

		if (0 == fffile_infofn(fn, &fi) && fffile_isdir(fffile_infoattr(&fi)))
			continue;

		return fn;
	}
}


static void* f_touch_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void f_touch_close(void *p, fcom_cmd *cmd)
{
}

static int f_touch_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->output.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	if (0 != com->fcom_cmd_filtadd(cmd, FCOM_CMD_FILT_OUT(cmd)))
		return FCOM_ERR;

	if (fftime_sec(&cmd->mtime) != 0 && cmd->date_as_fn != NULL)
		return FCOM_ERR;

	if (cmd->date_as_fn != NULL) {
		fffileinfo fi;
		if (0 != fffile_infofn(cmd->date_as_fn, &fi))
			return FCOM_SYSERR;
		cmd->output.mtime = fffile_infomtime(&fi);

	} else if (fftime_sec(&cmd->mtime) != 0)
		cmd->output.mtime = cmd->mtime;
	else
		fftime_now(&cmd->output.mtime);

	// open or create, but don't modify file data
	cmd->out_overwrite = 1;
	cmd->out_notrunc = 1;
	cmd->output.size = 0;
	return FCOM_NEXTDONE;
}


#define FILT_NAME  "f-textcount"

struct tcnt_stat {
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
	struct tcnt_stat all;
	struct tcnt_stat cur;
};

static void* f_tcnt_open(fcom_cmd *cmd)
{
	struct tcount *c;
	if (NULL == (c = ffmem_new(struct tcount)))
		return NULL;
	c->ln_f_min = (uint64)-1;
	c->sz_f_min = (uint64)-1;
	return c;
}

static void f_tcnt_close(void *p, fcom_cmd *cmd)
{
	struct tcount *c = p;
	struct tcnt_stat *a = &c->all;
	uint nempty = a->ln - a->ln_empty;
	uint nempty_perc = FFINT_DIVSAFE(nempty * 100, a->ln);
	fcom_infolog(FILT_NAME, "Files: %U, %U bytes (%D..%U), size/file:%U\n"
		"Lines: %U (%D..%U), non-empty:%U(%u%%), lines/file:%U"
		, c->f, a->sz, c->sz_f_min, c->sz_f_max, FFINT_DIVSAFE(a->sz, c->f)
		, a->ln, c->ln_f_min, c->ln_f_max, nempty, nempty_perc, FFINT_DIVSAFE(a->ln, c->f));

	ffmem_free(c);
}

/** Count stats for each input file. */
static int f_tcnt_process(void *p, fcom_cmd *cmd)
{
	enum { I_NEXTFILE, I_DATA, };
	struct tcount *c = p;

	switch (c->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = next_file(cmd)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		c->state = I_DATA;

		ffmem_tzero(&c->cur);
		c->cur.b_ln_min = (uint64)-1;

		return FCOM_MORE;

	case I_DATA:
		break;
	}

	struct tcnt_stat *f = &c->cur;
	f_tcnt_analyze(f, &cmd->in);

	if (cmd->in_last) {
		if (f->b_ln != 0)
			f->ln++;

		f_tcnt_print(f, cmd);
		f_tcnt_add(c, f);
		c->state = I_NEXTFILE;
	}

	return FCOM_MORE;
}

/** Count lines in buffer, bytes in line. */
static void f_tcnt_analyze(struct tcnt_stat *f, ffstr *ss)
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
static void f_tcnt_print(struct tcnt_stat *f, fcom_cmd *cmd)
{
	uint nempty = f->ln - f->ln_empty;
	uint nempty_perc = FFINT_DIVSAFE(nempty * 100, f->ln);
	fcom_verblog(FILT_NAME, "%s: size:%U  lines:%U  non-empty:%U(%u%%)  line-width:(%D..%U)"
		, cmd->input.fn, f->sz, f->ln, nempty, nempty_perc
		, f->b_ln_min, f->b_ln_max);
}

/** Aggregate file stats into overall stats. */
static void f_tcnt_add(struct tcount *c, const struct tcnt_stat *f)
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


#define FILT_NAME  "f-crc"

struct f_crc {
	uint state;
	uint cur;
};

static void* f_crc_open(fcom_cmd *cmd)
{
	struct f_crc *c;
	if (NULL == (c = ffmem_new(struct f_crc)))
		return NULL;
	return c;
}

static void f_crc_close(void *p, fcom_cmd *cmd)
{
	struct f_crc *c = p;
	ffmem_free(c);
}

static int f_crc_process(void *p, fcom_cmd *cmd)
{
	enum { I_NEXTFILE, I_DATA, };
	struct f_crc *c = p;

	switch (c->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = next_file(cmd)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		c->state = I_DATA;
		c->cur = 0;
		return FCOM_MORE;

	case I_DATA:
		break;
	}

	c->cur = crc32((void*)cmd->in.ptr, cmd->in.len, c->cur);

	if (cmd->in_last) {
		fcom_infolog(FILT_NAME, "%s: CRC32:%xu"
			, cmd->input.fn, c->cur);
		c->state = I_NEXTFILE;
	}

	return FCOM_MORE;
}

#undef FILT_NAME
