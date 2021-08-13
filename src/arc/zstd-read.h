/** zstd read
2021, Simon Zolin
*/

struct unzstd1 {
	zstd_decoder *z;
	ffstr in;
	uint64 in_total, out_total;
	ffvec buf;
};

static void* unzstd1_open(fcom_cmd *cmd)
{
	struct unzstd1 *z = ffmem_new(struct unzstd1);
	zstd_dec_conf zc = {};
	zstd_decode_init(&z->z, &zc);
	ffvec_alloc(&z->buf, 64*1024, 1);
	return z;
}

static void unzstd1_close(void *p, fcom_cmd *cmd)
{
	struct unzstd1 *z = p;
	zstd_decode_free(z->z);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static int unzstd1_process(void *p, fcom_cmd *cmd)
{
	struct unzstd1 *z = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		z->in = cmd->in;
		cmd->in.len = 0;
	}

	zstd_buf in, out;
	zstd_buf_set(&in, z->in.ptr, z->in.len);
	zstd_buf_set(&out, z->buf.ptr, z->buf.cap);
	int r = zstd_decode(z->z, &in, &out);
	ffstr_shift(&z->in, in.pos);
	ffstr_set(&cmd->out, z->buf.ptr, out.pos);
	z->in_total += in.pos;
	z->out_total += out.pos;

	fcom_dbglog(0, "unzstd1", "zstd_decode: %L (%U) -> %L (%U)"
		, in.pos, z->in_total, out.pos, z->out_total);

	if (r < 0) {
		fcom_errlog("unzstd1", "zstd_decode: %s", zstd_error(r));
		return FCOM_ERR;
	} else if (out.pos == 0) {
		if (cmd->flags & FCOM_CMD_FIRST)
			return FCOM_OUTPUTDONE;
		return FCOM_MORE;
	}

	return FCOM_DATA;
}

const fcom_filter unzstd1_filt = { unzstd1_open, unzstd1_close, unzstd1_process };
