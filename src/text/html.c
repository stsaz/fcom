/** fcom: Parse HTML data
2023, Simon Zolin */

static const char* html_help()
{
	return "\
Parse HTML data\n\
Usage:\n\
  `fcom html` INPUT... --filter TAG.ATTR [-o OUTPUT]\n\
OPTIONS\n\
  `--filter` TAG.ATTR    Print all values of an HTML tag's attribute\n\
";
}

#include <fcom.h>
#include <util/html.h>
#include <ffsys/globals.h>

static const fcom_core *core;

struct h_conf {
	ffstr tag, attr;
};

struct html {
	fcom_cominfo cominfo;

	uint st;
	uint stop;
	fcom_cominfo *cmd;

	ffstr iname;
	ffvec fdata;
	ffstr idata;

	htmlread html;
	ffvec buf;
	fcom_file_obj *out;

	struct h_conf conf;
};

#define O(member)  (void*)FF_OFF(struct html, member)

static int args_parse(struct html *h, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--filter",		'S',	O(conf.tag) },
		{}
	};
	if (core->com->args_parse(cmd, args, h, FCOM_COM_AP_INOUT))
		return -1;

	ffstr_splitby(&h->conf.tag, '.', &h->conf.tag, &h->conf.attr);

	if (!h->cmd->output.len)
		h->cmd->stdout = 1;

	return 0;
}

#undef O

static void html_close(fcom_op *op)
{
	struct html *h = op;
	core->file->destroy(h->out);
	ffvec_free(&h->fdata);
	ffvec_free(&h->buf);
	ffmem_free(h);
}

static fcom_op* html_create(fcom_cominfo *cmd)
{
	struct html *h = ffmem_new(struct html);
	h->cmd = cmd;

	if (0 != args_parse(h, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	h->out = core->file->create(&fc);

	return h;

end:
	html_close(h);
	return NULL;
}

static int html_find_tagattr(struct h_conf *conf, ffstr *in, ffstr *out, htmlread *html)
{
	uint state = 0;
	for (;;) {
		int r = htmlread_process(html, in, out);

		fcom_dbglog("htmlread_process: %d %S", r, out);

		switch (r) {
		case HTML_TAG:
			state = 0;
			if (ffstr_ieq2(out, &conf->tag))
				state = 1;
			break;

		case HTML_TAG_CLOSE:
		case HTML_TAG_CLOSE_SELF:
		case HTML_TEXT:
			break;

		case HTML_ATTR:
			if (state == 2)
				state = 1;
			if (state == 1 && ffstr_ieq2(out, &conf->attr))
				state = 2;
			break;

		case HTML_ATTR_VAL:
			if (state == 2)
				return 0;
			break;

		default:
			return 1;
		}
	}
	return 1;
}

static void html_run(fcom_op *op)
{
	struct html *h = op;
	int r, rc = 1;
	enum { I_IN, I_READ, I_PROC, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(h->stop)) {
		switch (h->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(h->cmd, &h->iname, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}

			if (!!core->com->input_allowed(h->cmd, h->iname, FCOM_COM_IA_AUTO))
				continue;

			h->st = I_READ;
			// fallthrough

		case I_READ:
			if (fffile_readwhole(h->iname.ptr, &h->fdata, (uint64)-1))
				goto end;
			h->idata = *(ffstr*)&h->fdata;
			h->st = I_PROC;
			// fallthrough

		case I_PROC:
			htmlread_open(&h->html);
			while (h->idata.len) {
				ffstr val;
				if (html_find_tagattr(&h->conf, &h->idata, &val, &h->html))
					break;
				ffvec_addstr(&h->buf, &val);
				ffvec_addchar(&h->buf, '\n');
			}
			htmlread_close(&h->html);
			h->st = I_OUT_OPEN;
			// fallthrough

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(h->cmd);
			r = core->file->open(h->out, h->cmd->output.ptr, oflags);
			if (r == FCOM_FILE_ERR) goto end;
			h->st = I_WRITE;
		}
			// fallthrough

		case I_WRITE:
			r = core->file->write(h->out, *(ffstr*)&h->buf, -1);
			if (r == FCOM_FILE_ERR) goto end;
			core->file->close(h->out);

			h->st = I_IN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = h->cmd;
	html_close(h);
	core->com->complete(cmd, rc);
	}
}

static void html_signal(fcom_op *op, uint signal)
{
	struct html *h = op;
	FFINT_WRITEONCE(h->stop, 1);
}

static const fcom_operation fcom_op_html = {
	html_create, html_close,
	html_run, html_signal,
	html_help,
};

FCOM_MOD_DEFINE(html, fcom_op_html, core)
