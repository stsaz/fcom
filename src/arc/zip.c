/** .zip pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <arc/arc.h>
#include <arc/zip-read.h>
#include <ffpack/zipwrite.h>
#include <FF/number.h>
#include <FF/time.h>


// ZIP
static void* zip_open(fcom_cmd *cmd);
static void zip_close(void *p, fcom_cmd *cmd);
static int zip_process(void *p, fcom_cmd *cmd);
const fcom_filter zip_filt = { &zip_open, &zip_close, &zip_process };

// UNZIP
static void* unzip_open(fcom_cmd *cmd);
static void unzip_close(void *p, fcom_cmd *cmd);
static int unzip_process(void *p, fcom_cmd *cmd);
const fcom_filter unzip_filt = { &unzip_open, &unzip_close, &unzip_process };


#define FILT_NAME  "arc.zip"

enum {
	BUFSIZE = 64 * 1024,
	MAX_STORED_SIZE = 32,
};

typedef struct zip {
	uint state;
	ffzipwrite zip;
	ffstr in;
} zip;

static void* zip_open(fcom_cmd *cmd)
{
	zip *z;
	if (NULL == (z = ffmem_new(zip)))
		return FCOM_OPEN_SYSERR;

	z->zip.timezone_offset = core->conf->tz.real_offset;

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "Output file name must be specified", 0);
		goto err;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_OUT(cmd));
	return z;

err:
	zip_close(z, cmd);
	return NULL;
}

static void zip_close(void *p, fcom_cmd *cmd)
{
	zip *z = p;
	ffzipwrite_destroy(&z->zip);
	ffmem_free(z);
}

static int zip_process(void *p, fcom_cmd *cmd)
{
	zip *z = p;
	int r;
	enum E { W_NEXT, W_NEXT2, W_NEWFILE, W_DATA, W_EOF, W_FIN, };

again:
	switch ((enum E)z->state) {

	case W_EOF:
		z->state = W_NEXT2;
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_USECURFILE))) {
			ffzipwrite_finish(&z->zip);
			z->state = W_FIN;
		}
		return FCOM_MORE; // close previous file.in filter

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0))) {
			ffzipwrite_finish(&z->zip);
			z->state = W_FIN;
			break;
		}
		// fallthrough

	case W_NEXT2:
		if (0 == com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd)))
			return FCOM_ERR;

		z->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		ffzipwrite_conf conf = {};
		ffstr_setz(&conf.name, cmd->input.fn);
		conf.mtime = cmd->input.mtime;
#ifdef FF_WIN
		conf.attr_win = cmd->input.attr;
#else
		conf.attr_unix = cmd->input.attr;
		// conf.uid = ;
		// conf.gid = ;
#endif
		conf.deflate_level = (cmd->deflate_level != 255) ? cmd->deflate_level : 0;
		conf.zstd_level = cmd->zstd_level;
		conf.zstd_workers = cmd->zstd_workers;

		switch (cmd->comp_method) {
		case FCOM_COMP_STORE:
			conf.compress_method = ZIP_STORED; break;
		case 255:
		case FCOM_COMP_DEFLATE:
			conf.compress_method = ZIP_DEFLATED;  break;
		case FCOM_COMP_ZSTD:
			conf.compress_method = ZIP_ZSTANDARD;  break;
		default:
			errlog("unsupported compression method");
			return FCOM_ERR;
		}

		if (cmd->input.size < MAX_STORED_SIZE)
			conf.compress_method = ZIP_STORED;

		if (0 != ffzipwrite_fileadd(&z->zip, &conf)) {
			errlog("%s", ffzipwrite_error(&z->zip));
			return FCOM_ERR;
		}
		z->state = W_DATA;
	}
		//fall through

	case W_DATA:
		if (cmd->flags & FCOM_CMD_FWD) {
			if (cmd->in_last)
				ffzipwrite_filefinish(&z->zip);
			z->in = cmd->in;
		}
		break;

	case W_FIN:
		break;
	}

	for (;;) {

	r = ffzipwrite_process(&z->zip, &z->in, &cmd->out);
	switch (r) {

	case FFZIPWRITE_DATA:
		return FCOM_DATA;

	case FFZIPWRITE_FILEDONE:
		fcom_verblog(FILT_NAME, "%s: %U => %U (%u%%)"
			, cmd->input.fn, z->zip.file_rd, z->zip.file_wr
			, (uint)FFINT_DIVSAFE(z->zip.file_wr * 100, z->zip.file_rd));
		z->state = W_EOF;
		goto again;

	case FFZIPWRITE_SEEK:
		fcom_cmd_outseek(cmd, ffzipwrite_offset(&z->zip));
		break;

	case FFZIPWRITE_MORE:
		return FCOM_MORE;

	case FFZIPWRITE_DONE:
		return FCOM_DONE;

	case FFZIPWRITE_ERROR:
		fcom_errlog(FILT_NAME, "%s", ffzipwrite_error(&z->zip));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


static void* unzip_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void unzip_close(void *p, fcom_cmd *cmd)
{
}

static void task_onfinish(fcom_cmd *cmd, uint sig, void *param)
{
	com->ctrl(param, FCOM_CMD_RUNASYNC);
}

static int unzip_process(void *p, fcom_cmd *cmd)
{
	const char *ifn;
	if (NULL == (ifn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	if (FCOM_DONE != task_create_run(cmd, NULL, "arc.unzip1", ifn, task_onfinish, cmd))
		return FCOM_ERR;
	return FCOM_ASYNC;
}
