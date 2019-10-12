/** File operations.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/number.h>
#include <FF/crc.h>
#include <FF/data/pe.h>
#include <FF/data/pe-fmt.h>
#include <FFOS/dir.h>
#include <FFOS/file.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define verblog(fmt, ...)  fcom_verblog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

const fcom_core *core;
const fcom_command *com;

// MODULE
static int f_sig(uint signo);
static const void* f_iface(const char *name);
static int f_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod f_mod = {
	.sig = &f_sig, .iface = &f_iface, .conf = &f_conf,
};

// QUICK FILE OPS
extern int fffile_makepath(char *fn, size_t off);
static int fop_mkdir(const char *fn, uint flags);
static int fop_del(const char *fn, uint flags);
static int fop_move(const char *src, const char *dst, uint flags);
static int fop_time(const char *fn, const fftime *t, uint flags);
static const fcom_fops f_ops_iface = {
	&fop_mkdir, &fop_del, &fop_move, &fop_time,
};

// COPY
static void* f_copy_open(fcom_cmd *cmd);
static void f_copy_close(void *p, fcom_cmd *cmd);
static int f_copy_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_copy_filt = {
	&f_copy_open, &f_copy_close, &f_copy_process,
};

// TOUCH
static void* f_touch_open(fcom_cmd *cmd);
static void f_touch_close(void *p, fcom_cmd *cmd);
static int f_touch_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_touch_filt = {
	&f_touch_open, &f_touch_close, &f_touch_process,
};

// RENAME
static void* f_rename_open(fcom_cmd *cmd);
static void f_rename_close(void *p, fcom_cmd *cmd);
static int f_rename_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_rename_filt = {
	&f_rename_open, &f_rename_close, &f_rename_process,
};

// CRC
static void* f_crc_open(fcom_cmd *cmd);
static void f_crc_close(void *p, fcom_cmd *cmd);
static int f_crc_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_crc_filt = { &f_crc_open, &f_crc_close, &f_crc_process };

// PE INFO
static void* f_pe_open(fcom_cmd *cmd);
static void f_pe_close(void *p, fcom_cmd *cmd);
static int f_pe_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_peinfo = { &f_pe_open, &f_pe_close, &f_pe_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &f_mod;
}

struct oper {
	const char *name;
	const char *mod;
	const void *iface;
};

#ifdef FF_WIN
extern const fcom_filter wregfind_filt;
#else
static const fcom_filter wregfind_filt;
#endif
extern const fcom_filter txcnt_filt;
extern const fcom_filter utf8_filt;

static const struct oper cmds[] = {
	{ "copy", "file.copy", &f_copy_filt },
	{ "touch", "file.touch", &f_touch_filt },
	{ "rename", "file.rename", &f_rename_filt },
	{ "textcount", "file.textcount", &txcnt_filt },
	{ "utf8", "file.utf8", &utf8_filt },
	{ "crc", "file.crc", &f_crc_filt },
#ifdef FF_WIN
	{ "wregfind", "file.wregfind", &wregfind_filt },
#else
	{ NULL, "file.wregfind", &wregfind_filt },
#endif
	{ NULL, "file.ops", &f_ops_iface },
	{ "peinfo", "file.pe", &f_peinfo },
};

static const void* f_iface(const char *name)
{
	const struct oper *op;
	FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
		if (ffsz_eq(name, op->mod + FFSLEN("file.")))
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
		ffmem_init();
		com = core->iface("core.com");

		const struct oper *op;
		FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
			if (op->name != NULL
				&& 0 != com->reg(op->name, op->mod))
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


#define FILT_NAME  "makedir"
static int fop_mkdir(const char *fn, uint flags)
{
	if (flags & FOP_TEST)
		goto done;

	if (flags & FOP_RECURS) {
		if (0 != ffdir_rmake((char*)fn, 0))
			goto err;
	} else {
		if (0 != ffdir_make(fn))
			goto err;
	}

done:
	verblog("%s", fn);
	return 0;

err:
	syserrlog("%s", fn);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "remove"
static int fop_del(const char *fn, uint flags)
{
	const char *serr = NULL;

	if (flags & FOP_TEST)
		goto done;

	if (flags & FOP_DIR) {
		fffileinfo fi;
		if (0 != fffile_infofn(fn, &fi)) {
			serr = fffile_info_S;
			goto err;
		}
		if (fffile_isdir(fffile_infoattr(&fi)))
			flags |= FOP_DIRONLY;
	}

	if (flags & FOP_DIRONLY) {
		if (0 != ffdir_rm(fn)) {
			serr = ffdir_rm_S;
			goto err;
		}
	} else {
		if (0 != fffile_rm(fn)) {
			serr = fffile_rm_S;
			goto err;
		}
	}

done:
	verblog("%s", fn);
	return 0;

err:
	syserrlog("%s: %s", serr, fn);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "move"
static int fop_move(const char *src, const char *dst, uint flags)
{
	fffileinfo fi;
	if (!(flags & FOP_OVWR) && 0 == fffile_infofn(dst, &fi)) {
		errlog("%s => %s: target file exists", src, dst);
		goto err;
	}

	if (flags & FOP_TEST)
		goto done;

	if (0 != fffile_rename(src, dst)) {

		if ((flags & FOP_RECURS) && fferr_nofile(fferr_last())) {
			if (0 != fffile_makepath((char*)dst, 0))
				goto err;
			if (0 != fffile_rename(src, dst))
				goto err;
			goto done;
		}
	}

done:
	verblog("'%s' => '%s'", src, dst);
	return 0;

err:
	syserrlog("'%s' => '%s'", src, dst);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "ftime"
static int fop_time(const char *fn, const fftime *t, uint flags)
{
	int r = -1;
	fffd f = FF_BADFD;
	const char *serr = NULL;

	if (flags & FOP_TEST)
		goto done;

	if (FF_BADFD == (f = fffile_open(fn, O_WRONLY))) {
		serr = fffile_open_S;
		goto err;
	}
	if (0 != fffile_settime(f, t)) {
		serr = fffile_settime_S;
		goto err;
	}

done:
	verblog("%s", fn);
	r = 0;

err:
	if (r != 0)
		syserrlog("%s: %s", serr, fn);
	fffile_safeclose(f);
	return r;
}
#undef FILT_NAME


#define FILT_NAME  "copy"
static void* f_copy_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}
static void f_copy_close(void *p, fcom_cmd *cmd)
{
}
static int f_copy_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;
	if (cmd->output.fn == NULL)
		return FCOM_ERR;
	com->fcom_cmd_filtadd(cmd, FCOM_CMD_FILT_IN(cmd));
	com->fcom_cmd_filtadd(cmd, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_MORE;
}
#undef FILT_NAME


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

	com->fcom_cmd_filtadd(cmd, FCOM_CMD_FILT_OUT(cmd));

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


#define FILT_NAME  "f-rename"
static void* f_rename_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void f_rename_close(void *p, fcom_cmd *cmd)
{
}

/** For each input filename replace text within. */
static int f_rename_process(void *p, fcom_cmd *cmd)
{
	if (cmd->search.len == 0) {
		errlog("Use --replace argument to specify search and replace text", 0);
		return FCOM_ERR;
	}

	int r;
	ffarr newfn = {};
	const char *fn;
	ffstr sfn;

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, 0))) {
			r = FCOM_DONE;
			goto done;
		}

		ffstr_setz(&sfn, fn);

		if (-1 == ffstr_find(&sfn, cmd->search.ptr, cmd->search.len))
			continue;

		size_t n = sfn.len - cmd->search.len + cmd->replace.len;
		if (NULL == ffarr_realloc(&newfn, n + 1)) {
			r = FCOM_SYSERR;
			goto done;
		}

		r = ffstr_replace((ffstr*)&newfn, &sfn, &cmd->search, &cmd->replace, FFSTR_REPL_ICASE);
		if (r == 0)
			continue;

		if (NULL == ffarr_append(&newfn, "", 1))
			return FCOM_SYSERR;

		if (!cmd->read_only
			&& 0 != fffile_rename(fn, newfn.ptr)) {
			syserrlog("%s -> %s", fn, newfn.ptr);
			if (cmd->skip_err)
				continue;
			r = FCOM_ERR;
			goto done;
		}
		verblog("%s -> %s", fn, newfn.ptr);
	}

done:
	ffarr_free(&newfn);
	return r;
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
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
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


#define FILT_NAME  "peinfo"

struct peinfo {
	uint state;
	uint dd_idx;
	ffpe pe;
};

static void* f_pe_open(fcom_cmd *cmd)
{
	struct peinfo *p = ffmem_new(struct peinfo);
	if (p == NULL)
		return NULL;
	ffpe_open(&p->pe);
	return p;
}

static void f_pe_close(void *_p, fcom_cmd *cmd)
{
	struct peinfo *p = _p;
	ffpe_close(&p->pe);
	ffmem_free(p);
}

static int f_pe_process(void *_p, fcom_cmd *cmd)
{
	struct peinfo *p = _p;
	int r;

	enum { I_NEXTFILE, I_DATA, };

again:
	switch (p->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_FIN;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		p->state = I_DATA;
		return FCOM_MORE;

	case I_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		ffpe_input(&p->pe, cmd->in.ptr, cmd->in.len);

	for (;;) {
	r = ffpe_read(&p->pe);

	switch (r) {
	case FFPE_MORE:
		return FCOM_MORE;

	case FFPE_SEEK:
		fcom_cmd_seek(cmd, ffpe_offset(&p->pe));
		return FCOM_MORE;

	case FFPE_HDR: {
		const struct ffpe_info *i = &p->pe.info;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "header: machine:%xu  sections:%u  tm_created:%u  linker_ver:%u.%u  code_size:%xu  "
			"init_data_size:%xu  uninit_data_size:%xu  "
			"stack_size_res:%xU  stack_size_commit:%xU  "
			"entry_addr:%xu  pe32+:%u"
			, i->machine
			, i->sections
			, i->tm_created
			, i->linker_ver[0], i->linker_ver[1]
			, i->code_size
			, i->init_data_size, i->uninit_data_size
			, i->stack_size_res, i->stack_size_commit
			, i->entry_addr, i->pe32plus);
		break;
	}

	case FFPE_DD: {
		const struct ffpe_data_dir *dd = &p->pe.data_dir;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "dd[%u]:  vaddr:%xu  vsize:%xu"
			, ++p->dd_idx, dd->vaddr, dd->vsize);
		break;
	}

	case FFPE_SECT: {
		const struct ffpe_sect *s = &p->pe.section;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "section: %s  vaddr:%xu  vsize:%xu  raw_off:%xu  raw_size:%xu  flags:%xu"
			, s->name, s->vaddr, s->vsize, s->raw_off, s->raw_size, s->flags);
		break;
	}

	case FFPE_IMPDIR: {
		uint n = p->pe.imp_dir.len / sizeof(struct coff_imp_dir);
		const struct coff_imp_dir *id = (void*)p->pe.imp_dir.ptr;
		for (uint i = 0;  i != n;  i++) {
			core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "import dir: "
				"lookups_rva:%xu  "
				"unused1:%xu  "
				"forwarder:%xu  "
				"name_rva:%xu  "
				"addrs_rva:%xu"
				, ffint_ltoh32(id[i].lookups_rva)
				, ffint_ltoh32(id[i].unused1)
				, ffint_ltoh32(id[i].forwarder)
				, ffint_ltoh32(id[i].name_rva)
				, ffint_ltoh32(id[i].addrs_rva));
		}
		break;
	}

	case FFPE_IMPORT: {
		const struct ffpe_imp_ent *i = &p->pe.import;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "import: dll_name:%s  sym_name:%s  sym_ordinal:%U"
			, i->dll_name, i->sym_name, i->sym_ordinal);
		break;
	}

	case FFPE_DONE:
		p->state = I_NEXTFILE;
		ffpe_close(&p->pe);
		ffmem_tzero(&p->pe);
		ffpe_open(&p->pe);
		goto again;

	case FFPE_ERR:
	default:
		errlog("ffpe_read(): %s", ffpe_errstr(&p->pe));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME
