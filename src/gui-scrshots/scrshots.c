/** Screenshot saver.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/gui/loader.h>
#include <FF/gui/winapi.h>
#include <FF/pic/bmp.h>
#include <FF/data/parse.h>
#include <FF/time.h>
#include <FF/path.h>


extern const fcom_core *core;
extern const fcom_command *com;

#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define infolog(fmt, ...)  fcom_infolog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)


static void wscrshot_action(ffui_wnd *wnd, int id);
static void scrshot_save(void *param);
struct opts;
static int opts_init(struct opts *c);
static int opts_load(struct opts *c);
static void opts_save(struct opts *c);
static void opts_show(const struct opts *c, ffui_view *v);
static void opts_set(struct opts *c, ffui_view *v, uint sub);
static int opts_hotkey_assign(struct opts *o);

// BMP output from GUI subsystem
static void* dcbmp_open(fcom_cmd *cmd);
static void dcbmp_close(void *p, fcom_cmd *cmd);
static int dcbmp_process(void *p, fcom_cmd *cmd);
const fcom_filter dcbmp_filt = { &dcbmp_open, &dcbmp_close, &dcbmp_process };


#define FILT_NAME  "gui.screenshots"

enum OPT_MODE {
	MODE_DESKTOP,
	MODE_ACTIVE_WND,
	MODE_ACTIVE_CLIENT,
};

struct opts {
	char *fn;
	char *path;
	char *key;
	ffui_hotkey hk;
	uint mode; //enum OPT_MODE
	int jpeg_quality;
	int png_compression;
	uint max_width;
	uint max_height;
};

enum VOPTS_COLUMNS {
	VOPTS_NAME,
	VOPTS_VAL,
	VOPTS_DESC,
};

struct wscrshot {
	ffui_wnd wscrshot;
	ffui_view vopts;
	ffui_paned pnopts;
	ffui_stbar stbar;
	ffui_trayicon tray;
	ffui_icon trayico;
};

struct ggui {
	ffui_menu mtray;
	struct wscrshot wscrshot;

	struct opts opts;
};

static struct ggui *gg;

static const ffui_ldr_ctl wscrshot_ctls[] = {
	FFUI_LDR_CTL(struct wscrshot, wscrshot),
	FFUI_LDR_CTL(struct wscrshot, vopts),
	FFUI_LDR_CTL(struct wscrshot, pnopts),
	FFUI_LDR_CTL(struct wscrshot, stbar),
	FFUI_LDR_CTL(struct wscrshot, tray),
	FFUI_LDR_CTL_END
};

static const ffui_ldr_ctl top_ctls[] = {
	FFUI_LDR_CTL3(struct ggui, wscrshot, wscrshot_ctls),
	FFUI_LDR_CTL(struct ggui, mtray),
	FFUI_LDR_CTL_END
};

static void* gui_getctl(void *udata, const ffstr *name)
{
	void *ctl = ffui_ldr_findctl(top_ctls, gg, name);
	return ctl;
}

enum CMDS {
	A_OPTS_CLICK = 1,
	A_WND_SHOW,
	A_QUIT,

	A_WND_MIN,
	A_OPTS_EDIT_DONE,
	A_SCRSHOT,
};

static const char* const cmds[] = {
	"A_OPTS_CLICK",
	"A_WND_SHOW",
	"A_QUIT",
};

static int gui_getcmd(void *udata, const ffstr *name)
{
	int r;
	if (0 > (r = ffs_findarrz(cmds, FFCNT(cmds), name->ptr, name->len)))
		return -1;
	return r + 1;
}


struct dcbmp {
	uint state;
	ffui_pos pos;
	ffarr buf;
	uint line;
};

static void* dcbmp_open(fcom_cmd *cmd)
{
	struct dcbmp *c;
	if (NULL == (c = ffmem_new(struct dcbmp)))
		return FCOM_OPEN_SYSERR;
	return c;
}

static void dcbmp_close(void *p, fcom_cmd *cmd)
{
	struct dcbmp *c = p;
	ffarr_free(&c->buf);
	ffmem_free(c);
}

/** Get raw bitmap data (BGR) from window DC.
Copy region from window DC to a temporary DC, then get raw data from temp DC. */
static int gui_wnd_dc2bmp(HWND h, void *out, size_t cap, const ffui_pos *pos)
{
	int r = -1;
	HDC src = NULL, dst = NULL;
	void *hbmp = NULL;

	if (cap < pos->cy * ff_align_ceil2(pos->cx * 3, 4))
		goto end;

	if (gg->opts.mode == MODE_ACTIVE_WND)
		src = GetWindowDC(h);
	else
		src = GetDC(h);
	if (src == NULL)
		goto end;
	if (NULL == (dst = CreateCompatibleDC(src)))
		goto end;
	if (NULL == (hbmp = CreateCompatibleBitmap(src, pos->cx, pos->cy)))
		goto end;

	SelectObject(dst, hbmp);
	if (0 == BitBlt(dst, 0, 0, pos->cx, pos->cy, src, pos->x, pos->y, SRCCOPY))
		goto end;

	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biWidth = pos->cx;
	bi.bmiHeader.biHeight = pos->cy;
	bi.bmiHeader.biPlanes = 1;
	if (0 == GetDIBits(dst, hbmp, 0, pos->cy, out, &bi, DIB_RGB_COLORS))
		goto end;

	r = 0;

end:
	DeleteObject(hbmp);
	DeleteDC(dst);
	ReleaseDC(h, src);
	return r;
}

/** Save the whole screenshot in memory then return line by line. */
static int dcbmp_process(void *p, fcom_cmd *cmd)
{
	struct dcbmp *c = p;

	switch (c->state) {
	case 0: {
		ffui_ctl w;
		ffui_pos pos;
		uint f;
		if (gg->opts.mode == MODE_DESKTOP)
			ffui_desktop(&w);
		else
			ffui_wnd_front(&w);
		if (w.h == NULL)
			return FCOM_ERR;
		f = (gg->opts.mode == MODE_ACTIVE_CLIENT) ? FFUI_FPOS_CLIENT : 0;
		ffui_getpos2(&w, &pos, f);
		dbglog(0, "ffui_getpos2: x:%d y:%d cx:%u cy:%u"
			, pos.x, pos.y, pos.cx, pos.cy);
		if (gg->opts.mode == MODE_ACTIVE_WND)
			pos.x = 0, pos.y = 0;

		if (cmd->pic.width != 0)
			ffint_setmin(pos.cx, (int)cmd->pic.width);
		if (cmd->pic.height != 0)
			ffint_setmin(pos.cy, (int)cmd->pic.height);

		ffpic_info inf;
		inf.format = FFPIC_BGR;
		inf.width = pos.cx;
		inf.height = pos.cy;
		ffbmp_cook bmp = {0};
		bmp.info = inf;
		if (NULL == ffarr_alloc(&c->buf, ffbmp_wsize(&bmp)))
			return FCOM_SYSERR;

		if (0 != gui_wnd_dc2bmp(w.h, c->buf.ptr, c->buf.cap, &pos))
			return FCOM_ERR;

		cmd->pic.format = FFPIC_BGR;
		cmd->pic.width = inf.width;
		cmd->pic.height = inf.height;
		c->pos = pos;
		c->line = c->pos.cy;
		c->state = 1;
	}
	// fall through

	case 1:
		break;
	}

	c->line--;
	ffstr_set(&cmd->out, c->buf.ptr + c->line * ff_align_ceil2(c->pos.cx * 3, 4), c->pos.cx * 3);
	if (c->line == 0)
		return FCOM_DONE;
	return FCOM_DATA;
}

enum VARS {
	VAR_DATE,
	VAR_TIME,
};
static const char* const vars[] = {
	"date",
	"time",
};

/** Expand $-variables. */
static int Svar_expand(ffarr *out, const ffstr *in)
{
	int r, ivar, have_dt = 0;
	ffstr s = *in;
	ffsvar p;
	ffdtm dt;

	while (s.len != 0) {
		size_t n = s.len;
		r = ffsvar_parse(&p, s.ptr, &n);
		ffstr_shift(&s, n);

		switch ((enum FFSVAR)r) {
		case FFSVAR_S:
			ivar = ffszarr_findsorted(vars, FFCNT(vars), p.val.ptr, p.val.len);
			if (ivar < 0)
				return -1;

			switch (ivar) {
			case VAR_DATE:
			case VAR_TIME:
				if (!have_dt) {
					// get time only once
					fftime t;
					fftime_now(&t);
					fftime_split(&dt, &t, FFTIME_TZLOCAL);
					have_dt = 1;
				}
				break;
			}

			switch (ivar) {
			case VAR_DATE:
				if (0 == ffstr_catfmt(out, "%04u%02u%02u", dt.year, dt.month, dt.day))
					return -1;
				break;

			case VAR_TIME:
				if (0 == ffstr_catfmt(out, "%02u%02u%02u", dt.hour, dt.min, dt.sec))
					return -1;
				break;
			}
			break;

		case FFSVAR_TEXT:
			if (NULL == ffarr_append(out, p.val.ptr, p.val.len))
				return -1;
			break;
		}
	}

	if (NULL == ffarr_append(out, "", 1))
		return -1;
	out->len--;

	return 0;
}

static const char exts[][4] = {
	"bmp",
	"jpg",
	"png",
};
static const char *const ofilters[] = {
	"pic.bmp-out",
	"pic.jpg-out",
	"pic.png-out",
};
static const char* filter_by_ext(const ffstr *ext)
{
	int i;
	if (0 > (i = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), ext->ptr, ext->len)))
		return NULL;
	return ofilters[i];
}

static void scrshot_done(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon mon_iface = { &scrshot_done };

struct sshot {
	char *fn;
};

/** Create fcom task to save the current screenshot to a file. */
static void scrshot_save(void *param)
{
	ffmem_free(param);

	fcom_cmd cmd = {0};
	ffarr a = {0};
	void *c;
	struct sshot *ss = NULL;

	if (NULL == (ss = ffmem_new(struct sshot)))
		goto end;

	ffstr s;
	ffstr_setz(&s, gg->opts.path);
	if (0 != Svar_expand(&a, &s))
		goto end;

	ffstr ext;
	ffpath_split3(a.ptr, a.len, NULL, NULL, &ext);
	const char *filt;
	if (NULL == (filt = filter_by_ext(&ext))) {
		errlog("unknown picture file extension .%S", &ext);
		goto end;
	}

	cmd.name = "scrshot";
	cmd.flags = FCOM_CMD_EMPTY;
	cmd.output.fn = a.ptr;
	cmd.jpeg_quality = gg->opts.jpeg_quality;
	cmd.png_comp = gg->opts.png_compression;
	cmd.pic_colors = -1;
	cmd.pic.width = gg->opts.max_width;
	cmd.pic.height = gg->opts.max_height;
	if (NULL == (c = com->create(&cmd)))
		goto end;
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "gui.dcbmp");
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, filt);
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "core.file-out");
	com->fcom_cmd_monitor(c, &mon_iface);
	ss->fn = a.ptr;
	ffatom_fence_rel(); //'ss' is complete
	com->ctrl(c, FCOM_CMD_SETUDATA, ss);
	com->ctrl(c, FCOM_CMD_RUNASYNC);
	return;

end:
	ffarr_free(&a);
}

static void scrshot_done(fcom_cmd *cmd, uint sig)
{
	struct sshot *ss = (void*)com->ctrl(cmd, FCOM_CMD_UDATA);
	ffatom_fence_acq(); //'ss' is complete
	infolog("saved file %s", ss->fn);
	ffmem_free(ss->fn);
	ffmem_free(ss);
}

static const char* const opts_desc[] = {
	"",
	"",
	"0:desktop, 1:active window, 2:active window (client area)",
	"",
	"",
	"",
	"",
};

#define OFF(m)  FF_OFF(struct opts, m)
static const ffconf_arg opts_args[] = {
	{ "Target path",	FFCONF_TSTRZ, OFF(path) },
	{ "Global Key",	FFCONF_TSTRZ, OFF(key) },
	{ "Mode",	FFCONF_TINT32, OFF(mode) },
	{ "Max Width",	FFCONF_TINT32, OFF(max_width) },
	{ "Max Height",	FFCONF_TINT32, OFF(max_height) },
	{ "JPEG Quality",	FFCONF_TINT32 | FFCONF_FSIGN, OFF(jpeg_quality) },
	{ "PNG Compression",	FFCONF_TINT32 | FFCONF_FSIGN, OFF(png_compression) },
	{}
};
#undef OFF

static int opts_init(struct opts *c)
{
	if (NULL == (c->path = core->env_expand(NULL, 0, "%USERPROFILE%\\Pictures\\scr-$date-$time.jpg")))
		return -1;
	c->key = ffsz_alcopyz("F11");
	c->mode = MODE_DESKTOP;
	c->jpeg_quality = -1;
	c->png_compression = -1;
	c->max_width = 0;
	c->max_height = 0;
	return 0;
}

/** Load options from a file. */
static int opts_load(struct opts *c)
{
	if (NULL == (c->fn = core->env_expand(NULL, 0, "%APPDATA%\\fcom\\screenshots.conf")))
		return -1;
	dbglog(0, "reading %s", c->fn);
	ffstr errmsg = {};
	int r = ffconf_parse_file(opts_args, c, c->fn, 0, &errmsg);
	if (r != 0 && !fferr_nofile(fferr_last())) {
		errlog("%S", &errmsg);
	}
	ffstr_free(&errmsg);
	return r;
}

/** Save options to a file. */
static void opts_save(struct opts *c)
{
	ffconfw conf = {};
	const ffconf_arg *a;
	ffconfw_init(&conf, FFCONFW_FCRLF);
	FFARRS_FOREACH(opts_args, a) {
		if (a->name == NULL)
			break;
		ffconfw_addkeyz(&conf, a->name);
		void *ptr = FF_PTR(c, a->dst);
		switch (a->flags & 0x0f) {
		case FFCONF_TSTRZ:
			ffconfw_addstrz(&conf, *(char**)ptr);
			break;
		case _FFCONF_TINT:
			if (a->flags & FFCONF_FSIGN)
				ffconfw_addint(&conf, *(int*)ptr);
			else
				ffconfw_addint(&conf, *(uint*)ptr);
			break;
		default:
			FF_ASSERT(0);
		}
	}
	ffconfw_fin(&conf);
	ffstr buf;
	ffconfw_output(&conf, &buf);
	fffile_writeall(c->fn, buf.ptr, buf.len, 0);
	dbglog(0, "saved settings to %s", c->fn);
}

/* struct opts -> GUI */
static void opts_show(const struct opts *c, ffui_view *v)
{
	ffui_viewitem it = {0};
	char buf[128];

	for (uint i = 0;  i != FFCNT(opts_args);  i++) {
		const ffconf_arg *a = &opts_args[i];
		if (a->name == NULL)
			break;

		void *ptr = FF_PTR(c, a->dst);

		ffui_view_settextz(&it, a->name);
		ffui_view_append(v, &it);

		switch (a->flags & 0x0f) {
		case FFCONF_TSTRZ:
			ffui_view_settextz(&it, *(char**)ptr);
			break;
		case _FFCONF_TINT: {
			uint f = 0;
			ffuint64 k = *(uint*)ptr;
			if (a->flags & FFCONF_FSIGN) {
				f = FFS_INTSIGN;
				k = *(int*)ptr;
			}
			uint n = ffs_fromint(k, buf, sizeof(buf), f);
			ffui_view_settext(&it, buf, n);
			break;
		}
		default:
			FF_ASSERT(0);
		}

		ffui_view_set(v, VOPTS_VAL, &it);

		ffui_view_settextz(&it, opts_desc[i]);
		ffui_view_set(v, VOPTS_DESC, &it);
	}
	ffui_view_itemreset(&it);
}

/** Register and associate a global hotkey with the main window. */
static int opts_hotkey_assign(struct opts *c)
{
	ffstr s;
	ffstr_setz(&s, c->key);
	ffui_hotkey hk;
	if (0 == (hk = ffui_hotkey_parse(s.ptr, s.len))) {
		errlog("invalid key %S", &s);
		return -1;
	}
	c->hk = hk;
	if (0 != ffui_wnd_ghotkey_reg(&gg->wscrshot.wscrshot, c->hk, A_SCRSHOT)) {
		syserrlog("%s", "ffui_wnd_ghotkey_reg()");
		return -1;
	}
	infolog("registered global hotkey %S", &s);
	return 0;
}

/* GUI -> struct opts */
static void opts_set(struct opts *c, ffui_view *v, uint sub)
{
	int i = ffui_view_focused(v);
	FF_ASSERT(i >= 0);
	ffstr text;

	const ffconf_arg *a = &opts_args[i];
	void *ptr = FF_PTR(c, a->dst);
	switch (a->flags & 0x0f) {
	case FFCONF_TSTRZ:
		*(char**)ptr = ffsz_dup(v->text);
		break;
	case _FFCONF_TINT:
		ffstr_setz(&text, v->text);
		if (a->flags & FFCONF_FSIGN)
			(void)ffstr_to_int32(&text, (uint*)ptr);
		else
			(void)ffstr_to_uint32(&text, (uint*)ptr);
		break;
	default:
		FF_ASSERT(0);
	}

	if (ffsz_eq(a->name, "Global Key")) {
		ffui_wnd_ghotkey_unreg(&gg->wscrshot.wscrshot);
		if (0 != opts_hotkey_assign(c))
			return;
		ffui_stbar_settextz(&gg->wscrshot.stbar, 0, "");
	}

	ffui_view_edit_set(v, i, sub);
}

static void wscrshot_action(ffui_wnd *wnd, int id)
{
	switch (id) {

	case A_OPTS_CLICK:
		ffui_view_edit_hittest(&gg->wscrshot.vopts, VOPTS_VAL);
		break;

	case A_OPTS_EDIT_DONE:
		opts_set(&gg->opts, &gg->wscrshot.vopts, VOPTS_VAL);
		break;

	case A_SCRSHOT: {
		fftask *t = ffmem_new(fftask);
		if (t == NULL) {
			syserrlog("", 0);
			break;
		}
		fftask_set(t, &scrshot_save, t);
		core->task(FCOM_TASK_ADD, t);
		break;
	}

	case A_WND_SHOW:
		ffui_show(&gg->wscrshot.wscrshot, 1);
		ffui_wnd_setfront(&gg->wscrshot.wscrshot);
		ffui_tray_show(&gg->wscrshot.tray, 0);
		break;

	case A_WND_MIN:
		ffui_tray_show(&gg->wscrshot.tray, 1);
		ffui_show(&gg->wscrshot.wscrshot, 0);
		break;

	case A_QUIT:
		ffui_wnd_close(&gg->wscrshot.wscrshot);
		break;
	}
}

static void opts_destroy(struct opts *o)
{
	ffmem_safefree(o->fn);
	ffmem_safefree(o->key);
	ffmem_safefree(o->path);
}

static void wscrshot_destroy(ffui_wnd *wnd)
{
	opts_save(&gg->opts);
	opts_destroy(&gg->opts);
	ffui_icon_destroy(&gg->wscrshot.trayico);
	ffui_tray_show(&gg->wscrshot.tray, 0);
	ffui_wnd_ghotkey_unreg(wnd);
	ffmem_free0(gg);
}

int scrshots_create(void)
{
	int rc = -1;
	if (NULL == (gg = ffmem_new(struct ggui)))
		return -1;

	char *path = NULL;
	ffui_loader ldr;
	ffui_ldr_init(&ldr);

	ldr.getctl = &gui_getctl;
	ldr.getcmd = &gui_getcmd;
	ldr.udata = gg;
	if (NULL == (path = core->getpath(FFSTR("scrshots.gui"))))
		goto end;
	if (0 != ffui_ldr_loadfile(&ldr, path)) {
		errlog("GUI loader: %s", ffui_ldr_errstr(&ldr));
		goto end;
	}

	gg->wscrshot.wscrshot.top = 1;
	gg->wscrshot.wscrshot.on_action = &wscrshot_action;
	gg->wscrshot.wscrshot.on_destroy = &wscrshot_destroy;
	gg->wscrshot.wscrshot.onminimize_id = A_WND_MIN;
	gg->wscrshot.vopts.edit_id = A_OPTS_EDIT_DONE;

	char *fn;
	if (NULL == (fn = core->getpath(FFSTR("screenshots.ico"))))
		goto end;
	ffui_icon_load(&gg->wscrshot.trayico, fn, 0, FFUI_ICON_SMALL);
	ffmem_free(fn);
	ffui_tray_seticon(&gg->wscrshot.tray, &gg->wscrshot.trayico);
	ffui_tray_settooltipz(&gg->wscrshot.tray, "fcom screenshot saver");

	if (0 != opts_init(&gg->opts))
		goto end;
	opts_load(&gg->opts);
	opts_show(&gg->opts, &gg->wscrshot.vopts);

	if (0 != opts_hotkey_assign(&gg->opts))
		ffui_stbar_settextz(&gg->wscrshot.stbar, 0, "Error");

	rc = 0;

end:
	ffui_ldr_fin(&ldr);
	ffmem_safefree(path);
	return rc;
}
