/** Archives.
Copyright (c) 2017 Simon Zolin
*/

#include <arc/arc.h>

#include <util/path.h>


const fcom_core *core;
const fcom_command *com;

// MODULE
static int arc_sig(uint signo);
static const void* arc_iface(const char *name);
static const fcom_mod arc_mod = {
	.sig = &arc_sig, .iface = &arc_iface,
	.ver = FCOM_VER,
	.name = "Archiver", .desc = "Pack/unpack archives .gz, .xz, .tar, .zip, .7z, .iso, .ico",
};

extern const fcom_filter zstd1_filt;
extern const fcom_filter unzstd1_filt;
extern const fcom_filter unzstd_filt;
extern const fcom_filter zstd_filt;

extern const fcom_filter gzip_filt;
extern const fcom_filter gzip1_filt;
extern const fcom_filter ungz_filt;
extern const fcom_filter ungz1_filt;

extern const fcom_filter unxz_filt;
extern const fcom_filter unxz1_filt;
extern const fcom_filter tar_filt;
extern const fcom_filter untar_filt;
extern const fcom_filter zip_filt;
extern const fcom_filter unzip_filt;
extern const fcom_filter iso_filt;
extern const fcom_filter uniso_filt;
extern const fcom_filter un7z_filt;
extern const fcom_filter icoi_filt;
extern const fcom_filter unzip1_filt;
extern const fcom_filter untar1_filt;

// UNPACK
static void* unpack_open(fcom_cmd *cmd);
static void unpack_close(void *p, fcom_cmd *cmd);
static int unpack_process(void *p, fcom_cmd *cmd);
static const fcom_filter unpack_filt = { &unpack_open, &unpack_close, &unpack_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &arc_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};

static const struct cmd cmds[] = {
	{ "zstd1", NULL, &zstd1_filt },
	{ "unzstd1", NULL, &unzstd1_filt },
	{ "zst", "arc.zst", &zstd_filt },
	{ "unzst", "arc.unzst", &unzstd_filt },

	{ "gz", "arc.gz", &gzip_filt },
	{ "gz1", NULL, &gzip1_filt },
	{ "ungz", "arc.ungz", &ungz_filt },
	{ "ungz1", NULL, &ungz1_filt },

	{ "unxz", "arc.unxz", &unxz_filt },
	{ "unxz1", NULL, &unxz1_filt },
	{ "tar", "arc.tar", &tar_filt },
	{ "untar", "arc.untar", &untar_filt },
	{ "zip", "arc.zip", &zip_filt },
	{ "unzip", "arc.unzip", &unzip_filt },
	{ "unzip1", NULL, &unzip1_filt },
	{ "untar1", NULL, &untar1_filt },
	{ "un7z", "arc.un7z", &un7z_filt },
	{ "iso", "arc.iso", &iso_filt },
	{ "uniso", "arc.uniso", &uniso_filt },
	{ "ico-in", NULL, &icoi_filt },
	{ "unpack", "arc.unpack", &unpack_filt },
};

static int arc_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		com = core->iface("core.com");
		const struct cmd *c;
		FFARRS_FOREACH(cmds, c) {
			if (c->mod != NULL && 0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static const void* arc_iface(const char *name)
{
	const struct cmd *cmd;
	FFARRS_FOREACH(cmds, cmd) {
		if (ffsz_eq(name, cmd->name))
			return cmd->iface;
	}
	return NULL;
}


/** Get output filename.  Handle user-defined output directory. */
int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf)
{
	size_t n;
	char *p, *end;

	n = input->len + 1;
	n += (cmd->outdir != NULL) ? ffsz_len(cmd->outdir) + FFSLEN("/") : 0;
	if (NULL == ffvec_grow(buf, n, 1))
		return FCOM_SYSERR;

	p = buf->ptr;
	end = ffarr_edge(buf);
	if (cmd->outdir != NULL) {
		p = ffs_copyz(p, end, cmd->outdir);
		p = ffs_copyc(p, end, '/');
	}

	ffstr in = *input;
	if (!ffutf8_valid(in.ptr, in.len)) {
		ffssize r = ffutf8_from_cp(p, end - p, in.ptr, in.len, core->conf->codepage);
		if (r < 0) {
			fcom_errlog_ctx(cmd, "arc.unpack", "ffutf8_from_cp");
			return FCOM_ERR;
		}
		ffstr_set(&in, p, r);
	}

	if (0 == (n = ffpath_normalize(p, end - p, in.ptr, in.len, FFPATH_SIMPLE | FFPATH_NO_DISK_LETTER | FFPATH_SLASH_BACKSLASH | FFPATH_FORCE_SLASH))) {
		fcom_errlog_ctx(cmd, "arc.unpack", "ffpath_norm: %S", input);
		return FCOM_ERR;
	}

	n = ffpath_makefn_full(p, end - p, p, n, '_');
	p += n;
	*p = '\0';

	return FCOM_DATA;
}

/** Create hard link. */
int out_hlink(fcom_cmd *cmd, ffstr target, const char *linkname)
{
	int r = FCOM_ERR;
	char *tgt = ffsz_dupn(target.ptr, target.len);

	if (0 != fffile_hardlink(tgt, linkname)) {
		fcom_syserrlog("arc", "fffile_hardlink(): %s -> %s"
			, linkname, tgt);
		if (!cmd->skip_err)
			goto end;
	} else
		fcom_dbglog(0, "arc", "created hard link: %s -> %s"
			, linkname, tgt);

	r = FCOM_DONE;
end:
	ffmem_free(tgt);
	return r;
}

/** Create symbolic link. */
int out_slink(fcom_cmd *cmd, ffstr target, const char *linkname)
{
	int r = FCOM_ERR;
	char *tgt = ffsz_dupn(target.ptr, target.len);

	if (0 != fffile_symlink(tgt, linkname)) {
		fcom_syserrlog("arc", "fffile_symlink(): %s -> %s"
			, linkname, tgt);
		if (!cmd->skip_err)
			goto end;
	} else
		fcom_dbglog(0, "arc", "created symbolic link: %s -> %s"
			, linkname, tgt);

	r = FCOM_DONE;
end:
	ffmem_free(tgt);
	return r;
}

/** Check if --member items contain wildcards. */
ffbool arc_members_wildcard(const ffslice *members)
{
	const char **pm;
	FFSLICE_WALK(members, pm) {
		size_t n = ffsz_len(*pm);
		if (*pm + n != ffs_findof(*pm, n, "*?", 2))
			return 1;
	}
	return 0;
}

/** Check if --member item matches the file name. */
ffbool arc_need_member(const ffslice *members, ffbool member_wildcard, const ffstr *fn)
{
	if (members->len == 0)
		return 1;

	const char **pm;
	FFSLICE_WALK(members, pm) {
		ffstr m;
		ffstr_setz(&m, *pm);
		if (0 == ffs_wildcard(m.ptr, m.len, fn->ptr, fn->len, FFS_WC_ICASE)
			|| ffpath_match(fn, &m, FFPATH_CASE_ISENS)) {
			return 1;
		}
	}
	return 0;
}


struct unpack {
	uint state;
};

static void* unpack_open(fcom_cmd *cmd)
{
	struct unpack *p = ffmem_new(struct unpack);
	if (p == NULL)
		return FCOM_OPEN_SYSERR;
	cmd->skip_err = 1;
	return p;
}

static void unpack_close(void *_p, fcom_cmd *cmd)
{
	struct unpack *p = _p;
	ffmem_free(p);
}

static const char arc_exts[][4] = {
	"7z",
	"gz",
	"ico",
	"iso",
	"tar",
	"tgz",
	"txz",
	"xz",
	"zip",
	"zipx",
	"zst",
};
static const char *const arc_filts[] = {
	"arc.un7z",
	"arc.ungz",
	"arc.ico-in",
	"arc.uniso",
	"arc.untar",
	"arc.untar",
	"arc.untar",
	"arc.unxz",
	"arc.unzip",
	"arc.unzip",
	"arc.unzst",
};

/*
.tar.gz, .tar.xz are passed to .tar handler
.ico: *we* set input for its handling module.  Other modules read input by themselves.
Note: we can't mix archives of different types - not supported.
*/
static int unpack_process(void *_p, fcom_cmd *cmd)
{
	struct unpack *p = _p;
	int r;
	ffstr nm, ext;

again:
	switch (p->state) {
	case 0:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_PEEK)))
			return FCOM_DONE;
		ffstr_setz(&nm, cmd->input.fn);
		ffpath_split3(nm.ptr, nm.len, NULL, &nm, &ext);
		if (ffstr_ieqcz(&ext, "ico")) {
			p->state = 1;
			goto again;
		}
		break;

	case 1:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;

		ffstr_setz(&nm, cmd->input.fn);
		ffpath_split3(nm.ptr, nm.len, NULL, &nm, &ext);
		if (0 > (r = ffcharr_findsorted(arc_exts, FFCNT(arc_exts), sizeof(arc_exts[0]), ext.ptr, ext.len))) {
			fcom_errlog_ctx(cmd, "arc.unpack", "unknown archive file extension .%S", &ext);
			return FCOM_ERR;
		}

		if (!ffstr_ieqcz(&ext, "ico")) {
			fcom_errlog_ctx(cmd, "arc.unpack", "unsupported: can't switch to another archive type", 0);
			return FCOM_ERR;
		}

		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, arc_filts[r]);
		// Note: "unpack *.ico" isn't supported
		return FCOM_DONE;
	}

	if ((ffstr_ieqcz(&ext, "gz") || ffstr_ieqcz(&ext, "xz") || ffstr_ieqcz(&ext, "zst"))
		&& ffstr_irmatchcz(&nm, ".tar"))
		ffstr_setz(&ext, "tar");

	if (0 > (r = ffcharr_findsorted(arc_exts, FFCNT(arc_exts), sizeof(arc_exts[0]), ext.ptr, ext.len))) {
		fcom_errlog_ctx(cmd, "arc.unpack", "unknown archive file extension .%S", &ext);
		return FCOM_ERR;
	}

	const char *name;
	name = arc_filts[r];
	com->fcom_cmd_filtadd(cmd, name);
	return FCOM_DONE;
}
