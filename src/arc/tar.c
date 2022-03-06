/** .tar pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <arc/arc.h>
#include <arc/tar-read.h>
#include <ffpack/tarwrite.h>
#include <util/path.h>
#include <util/time.h>


// TAR
static void* tar_open(fcom_cmd *cmd);
static void tar_close(void *p, fcom_cmd *cmd);
static int tar_process(void *p, fcom_cmd *cmd);
const fcom_filter tar_filt = { &tar_open, &tar_close, &tar_process };

// UNTAR
static void* untar_open(fcom_cmd *cmd);
static void untar_close(void *p, fcom_cmd *cmd);
static int untar_process(void *p, fcom_cmd *cmd);
const fcom_filter untar_filt = { &untar_open, &untar_close, &untar_process };

#define FILT_NAME  "arc.tar"

typedef struct tar {
	uint state;
	fftarwrite tar;
	ffstr in;
	const char *fn;
	uint64 out_total;
} tar;

static void* tar_open(fcom_cmd *cmd)
{
	tar *t;
	if (NULL == (t = ffmem_new(tar)))
		return FCOM_OPEN_SYSERR;

	fftarwrite_init(&t->tar);

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "Output file name must be specified", 0);
		goto end;
	}
	ffstr ext;
	ffpath_split3(cmd->output.fn, ffsz_len(cmd->output.fn), NULL, NULL, &ext);
	const char *comp_mod = NULL;
	if (ffstr_ieqcz(&ext, "tgz") || ffstr_ieqcz(&ext, "gz"))
		comp_mod = "arc.gz1";
	else if (ffstr_ieqcz(&ext, "zst"))
		comp_mod = "arc.zstd1";
	if (comp_mod != NULL)
		if (0 == com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, comp_mod))
			goto end;

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return t;

end:
	tar_close(t, cmd);
	return NULL;
}

static void tar_close(void *p, fcom_cmd *cmd)
{
	tar *t = p;
	fftarwrite_destroy(&t->tar);
	ffmem_free(t);
}

static int tar_process(void *p, fcom_cmd *cmd)
{
	tar *t = p;
	int r;
	enum E { W_NEXT, W_NEXT2, W_NEWFILE, W_DATA, W_EOF, W_FIN, };

again:
	switch ((enum E)t->state) {

	case W_EOF:
		t->state = W_NEXT2;
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_USECURFILE))) {
			fftarwrite_finish(&t->tar);
			t->state = W_FIN;
		}
		return FCOM_MORE; // close previous file.in filter

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0))) {
			fftarwrite_finish(&t->tar);
			t->state = W_FIN;
			break;
		}
		// fallthrough

	case W_NEXT2:
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		t->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		fftarwrite_conf f = {};
		ffstr_setz(&f.name, cmd->input.fn);
#ifdef FF_UNIX
		f.attr_unix = cmd->input.attr;
		// conf.uid = ;
		// conf.gid = ;
#else
		f.attr_unix = (fffile_isdir(cmd->input.attr)) ? FFUNIX_FILE_DIR | 0755 : 0644;
#endif
		if (!fffile_isdir(cmd->input.attr))
			f.size = cmd->input.size;
		f.mtime = cmd->input.mtime;
		if (0 != (r = fftarwrite_fileadd(&t->tar, &f))) {
			if (r == -2) {
				t->state = W_EOF;
				goto again;
			}
			fcom_errlog(FILT_NAME, "%s: %s", cmd->input.fn, fftarwrite_error(&t->tar));
			return FCOM_ERR;
		}
		t->fn = cmd->input.fn;
		cmd->input.fn = NULL; // arc.gz1 won't use input filename in .gz header
		t->state = W_DATA;
	}
		//fall through

	case W_DATA:
		if (cmd->flags & FCOM_CMD_FWD) {
			if (cmd->in_last)
				fftarwrite_filefinish(&t->tar);
			t->in = cmd->in;
		}
		break;

	case W_FIN:
		break;
	}

	for (;;) {

		r = fftarwrite_process(&t->tar, &t->in, &cmd->out);
		switch (r) {

		case FFTARWRITE_DATA:
			t->out_total += cmd->out.len;
			fcom_dbglog(0, "tar", "total:%U"
				, t->out_total);
			return FCOM_DATA;

		case FFTARWRITE_FILEDONE:
			fcom_verblog(FILT_NAME, "added %s: %U", t->fn, t->tar.fsize);
			t->state = W_EOF;
			goto again;

		case FFTARWRITE_MORE:
			return FCOM_MORE;

		case FFTARWRITE_DONE:
			return FCOM_DONE;

		case FFTARWRITE_ERROR:
			fcom_errlog(FILT_NAME, "%s", fftarwrite_error(&t->tar));
			return FCOM_ERR;
		}
	}
}

#undef FILT_NAME


static void* untar_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void untar_close(void *p, fcom_cmd *cmd)
{
}

static void task_onfinish(fcom_cmd *cmd, uint sig, void *param)
{
	com->ctrl(param, FCOM_CMD_RUNASYNC);
}

static int untar_process(void *p, fcom_cmd *cmd)
{
	const char *ifn;
	if (NULL == (ifn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	ffstr ext;
	ffpath_split3(ifn, ffsz_len(ifn), NULL, NULL, &ext);
	const char *comp = NULL;
	if (ffstr_ieqcz(&ext, "tgz") || ffstr_ieqcz(&ext, "gz"))
		comp = "arc.ungz1";
	else if (ffstr_ieqcz(&ext, "zst"))
		comp = "arc.unzstd1";
	else if (ffstr_ieqcz(&ext, "txz") || ffstr_ieqcz(&ext, "xz"))
		comp = "arc.unxz1";
	if (comp != NULL) {
		if (cmd->output.fn == NULL)
			cmd->output.fn = (void*)1; // decompression filters won't set output filename
	}

	if (FCOM_DONE != task_create_run(cmd, comp, "arc.untar1", ifn, task_onfinish, cmd))
		return FCOM_ERR;
	return FCOM_ASYNC;
}
