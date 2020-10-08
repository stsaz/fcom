/** fcom: .gz reader
2020, Simon Zolin
*/

#include <ffpack/gzread.h>

#define FILT_NAME  "arc.ungz"

typedef struct ungz {
	uint state;
	ffgzread gz;
	ffarr fn;
	ffstr in;
	ffuint64 fsize;
	ffuint64 outsize;
	ffuint next_chunk :1;
} ungz;

static void* ungz1_open(fcom_cmd *cmd)
{
	ungz *g;
	if (NULL == (g = ffmem_new(ungz)))
		return FCOM_OPEN_SYSERR;

	cmd->output.mtime = cmd->input.mtime;
	return g;
}

static void ungz1_close(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	ffarr_free(&g->fn);
	ffgzread_close(&g->gz);
	ffmem_free(g);
}

static int ungz1_process(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	int r;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, };

again:
	switch ((enum E)g->state) {
	case R_FIRST:
	if (cmd->in.len == 0) {
		g->state = R_INIT;
		/* chain: file.in -> ungz -> untar
		. untar returns MORE
		. ungz must return MORE because file.in isn't yet initialized */
		return FCOM_MORE;
	}
	//fall through

	case R_INIT: {
		ffint64 fsize = cmd->input.size;
		if (g->next_chunk)
			fsize = -1;
		if (0 != ffgzread_open(&g->gz, fsize))
			return FCOM_ERR;
		g->state = R_DATA;
	}
	// fallthrough

	case R_DATA:
		break;

	case R_EOF:
		if (g->in.len == 0) {
			if (cmd->in_last)
				return FCOM_DONE;
			return FCOM_MORE;
		}
		ffgzread_close(&g->gz);
		g->next_chunk = 1;
		g->state = R_INIT;
		cmd->flags &= ~FCOM_CMD_FWD;
		goto again;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		g->in = cmd->in;
		cmd->in.len = 0;
	}

	for (;;) {

		r = ffgzread_process(&g->gz, &g->in, &cmd->out);

		switch ((enum FFGZREAD_R)r) {

		case FFGZREAD_INFO: {
			ffgzread_info *info = ffgzread_getinfo(&g->gz);
			ffstr gzfn = info->name;
			cmd->output.size = info->uncompressed_size;

			if (cmd->output.fn == NULL) {
				ffstr name;
				if (gzfn.len == 0) {
					// "/path/file.txt.gz" -> "file.txt"
					ffstr_setz(&name, cmd->input.fn);
					ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
				} else {
					ffpath_split2(gzfn.ptr, gzfn.len, NULL, &name);
				}

				if (FCOM_DATA != (r = fn_out(cmd, &name, &g->fn)))
					return r;
				cmd->output.fn = g->fn.ptr;
			}

			fcom_dbglog(0, FILT_NAME, "info: name:%S  mtime:%u  osize:%U  crc32:%xu"
				, &gzfn, (int)info->mtime, info->uncompressed_size, info->uncompressed_crc);
			continue;
		}

		case FFGZREAD_DATA:
			g->outsize += cmd->out.len;
			return FCOM_DATA;

		case FFGZREAD_DONE: {
			ffgzread_info *info = ffgzread_getinfo(&g->gz);
			fcom_verblog(FILT_NAME, "finished: %U => %U (%u%%)"
				, info->compressed_size, g->outsize
				, (int)(info->compressed_size * 100 / g->outsize));

			FF_CMPSET(&cmd->output.fn, g->fn.ptr, NULL);

			g->state = R_EOF;
			goto again;
		}

		case FFGZREAD_MORE:
			return FCOM_MORE;

		case FFGZREAD_SEEK:
			cmd->input.offset = ffgzread_offset(&g->gz);
			cmd->in_seek = 1;
			return FCOM_MORE;

		case FFGZREAD_WARNING:
			fcom_warnlog(FILT_NAME, "%s  offset:0x%xU", ffgzread_error(&g->gz), cmd->input.offset);
			continue;
		case FFGZREAD_ERROR:
			fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffgzread_error(&g->gz), cmd->input.offset);
			return FCOM_ERR;
		}
	}
}

#undef FILT_NAME
