/** File synchronization by user filter.
Copyright (c) 2019 Simon Zolin
*/

#include <util/path.h>
#include <util/misc.h>


struct sync_ctx {
	struct fsync_cmp *cmp;
	char *fnL, *fnR;
	uint row_index;
};

static void sync_cmdmon_onsig(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon sync_cmdmon = { &sync_cmdmon_onsig };
static void sync(struct sync_ctx *sc);

/** Start synchronization. */
void gsync_sync(void *udata)
{
	struct sync_ctx *sc = ffmem_new(struct sync_ctx);
	if (sc == NULL)
		return;
	sc->row_index = -1;
	sync(sc);
}

static void sync_reset(struct sync_ctx *sc)
{
	ffmem_free0(sc->fnL);
	ffmem_free0(sc->fnR);
	sc->cmp = NULL;
}

static void sync_free(struct sync_ctx *sc)
{
	sync_reset(sc);
	ffmem_free(sc);
}

/** Operation on a file is complete. */
static void sync_result(struct sync_ctx *sc, int r)
{
	if (r != 0) {
		sc->cmp->status |= FSYNC_ST_ERROR;
	} else {
		sc->cmp->status &= ~FSYNC_ST_CHECKED;
		sc->cmp->status |= FSYNC_ST_DONE;
		gg->nchecked--;
	}
	ffui_view_redraw(&gg->wsync.vlist, sc->row_index, sc->row_index);

	update_status();
}

static void sync_cmdmon_onsig(fcom_cmd *cmd, uint sig)
{
	struct sync_ctx *sc = (void*)com->ctrl(cmd, FCOM_CMD_UDATA);
	int r = (!cmd->err) ? 0 : -1;
	sync_result(sc, r);
	sync_reset(sc);
	sync(sc);
}

/** Start file copy task. */
static int sync_copy(struct sync_ctx *sc, const char *src, const char *dst, uint flags)
{
	fcom_cmd cmd = {};
	cmd.name = "fcopy";
	cmd.flags = FCOM_CMD_EMPTY;
	cmd.input.fn = src;
	cmd.output.fn = dst;
	if (flags & FOP_KEEPDATE)
		cmd.out_preserve_date = 1;
	if (flags & FOP_OVWR)
		cmd.out_overwrite = 1;
	void *c;
	if (NULL == (c = com->create(&cmd)))
		return FCOM_ERR;
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "core.file-in");
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "core.file-out");
	com->fcom_cmd_monitor(c, &sync_cmdmon);
	com->ctrl(c, FCOM_CMD_SETUDATA, sc);
	com->ctrl(c, FCOM_CMD_RUNASYNC);
	return FCOM_ASYNC;
}

/**
"src/path/file" -> "dst/path/file"

For wildcards:
1. "\diff-src\d*": "\diff-src\d1/new" -> "d1/new"
2. "\diff-tgt\d*": "\diff-tgt" + "d1/new" -> "\diff-tgt\d1/new" */
static char* dst_fn(const char *fnL)
{
	ffstr fn, src, dst;
	ffstr_setz(&fn, fnL);
	ffstr_setz(&src, gg->opts.srcfn);
	ffstr_setz(&dst, gg->opts.dstfn);

	ffstr dir_src;
	if (0 != ffpath_parent(&src, &fn, &dir_src)) {
		FF_ASSERT(0);
		return NULL;
	}

	const char *star = ffs_findc(dst.ptr, dst.len, '*');

	if (NULL == ffs_findc(src.ptr, src.len, '*')) {
		if (star != NULL) {
			errlog("not supported: destination dir is a wildcard", 0);
			return NULL;
		}
		ffstr_shift(&fn, dir_src.len + FFSLEN("/"));
		return ffsz_alfmt("%S\\%S", &dst, &fn);
	}

	if (0 != ffs_wildcard(src.ptr, src.len, fn.ptr, fn.len, 0)) {
		FF_ASSERT(0);
		return NULL;
	}
	ffstr_shift(&fn, dir_src.len + FFSLEN("/"));

	if (star == NULL) {
		errlog("not supported: destination dir isn't a wildcard", 0);
		return NULL;
	}
	dst.len = star - dst.ptr;
	const char *sl = ffpath_rfindslash(dst.ptr, dst.len);
	dst.len = sl - dst.ptr;

	return ffsz_alfmt("%S\\%S", &dst, &fn);
}


/** Synchronize checked files (src => dst).
FSYNC_ST_NEQ: if file size has changed or contents don't match, copy file;
  otherwise just set attributes.
Suspend processing while file copy task is running asynchronously.
*/
static void sync(struct sync_ctx *sc)
{
	struct fsync_cmp *cmp;
	struct fsync_file *f;
	int r;
	ffvec toremove = {};

	for (uint i = sc->row_index + 1;  i != gg->cmptbl_filter.len;  i++) {

		sc->row_index = i;
		sc->cmp = cmp = list_getobj(i);
		if (!(cmp->status & FSYNC_ST_CHECKED))
			continue;

		if ((cmp->status & _FSYNC_ST_MASK) == FSYNC_ST_EQ)
			continue;

		cmp->status |= FSYNC_ST_PENDING;
		ffui_view_redraw(&gg->wsync.vlist, i, i);

		switch (cmp->status & _FSYNC_ST_MASK) {

		case FSYNC_ST_SRC:
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->left);
			f = fsfile(cmp->left);
			sc->fnR = dst_fn(sc->fnL);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;

			if (isdir(f->attr))
				r = fops->mkdir(sc->fnR, 0);
			else
				r = sync_copy(sc, sc->fnL, sc->fnR, FOP_KEEPDATE);
			break;

		case FSYNC_ST_NEQ:
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->left);
			sc->fnR = fsync->get(FSYNC_FULLNAME, cmp->right);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;

			if (cmp->status & (FSYNC_ST_SMALLER | FSYNC_ST_LARGER)
				|| !!fffile_cmp(sc->fnL, sc->fnR, 0)) {

				r = sync_copy(sc, sc->fnL, sc->fnR, FOP_OVWR | FOP_KEEPDATE);

			} else {
				fffileinfo fi;
				if (0 == (r = fffile_infofn(sc->fnL, &fi))) {

					if (cmp->status & (FSYNC_ST_OLDER | FSYNC_ST_NEWER)) {
						fftime t = fffile_infomtime(&fi);
						r = fops->time(sc->fnR, &t, 0);
					}

					if (cmp->status & FSYNC_ST_ATTR) {
						uint attr = fffile_infoattr(&fi);
						fffile_attrsetfn(sc->fnR, attr);
					}
				}
			}
			break;

		case FSYNC_ST_MOVED: {
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->right);
			char *L = fsync->get(FSYNC_FULLNAME, cmp->left);
			if (L == NULL)
				goto end;
			sc->fnR = dst_fn(L);
			ffmem_free(L);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;
			r = fops->move(sc->fnL, sc->fnR, FOP_OVWR | FOP_RECURS);
			break;
		}

		case FSYNC_ST_DEST:
			sc->fnR = fsync->get(FSYNC_FULLNAME, cmp->right);
			if (sc->fnR == NULL)
				goto end;
			if (toremove.len == 20) {
				r = fops->del_many(toremove.ptr, toremove.len, FOP_TRASH);
				char **it;
				FFSLICE_WALK(&toremove, it) {
					ffmem_free(*it);
				}
				toremove.len = 0;
			}
			*ffvec_pushT(&toremove, char*) = sc->fnR;
			sc->fnR = NULL;
			break;

		default:
			r = -1;
		}

		if (r == FCOM_ASYNC) {
			goto end2;
		}

		sync_result(sc, r);
		sync_reset(sc);
	}

end:
	sync_free(sc);

end2:
	if (toremove.len != 0) {
		fops->del_many(toremove.ptr, toremove.len, FOP_TRASH);
	}
	char **it;
	FFSLICE_WALK(&toremove, it) {
		ffmem_free(*it);
	}
	ffvec_free(&toremove);
}
