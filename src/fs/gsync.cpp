/** fcom: GUI for synchronizing directories
2024, Simon Zolin */

static const char* gsync_help()
{
	return "\
Show GUI for synchronizing directories\n\
Usage:\n\
  `fcom gsync` [SOURCE TARGET]\n\
";
}

#ifdef _WIN32
	#include <util/windows-shell.h>
#endif

#include <fcom.h>
#include <util/util.hpp>
#ifdef FF_WIN
	#include <ffgui/winapi/loader.h>
#else
	#include <util/unix-shell.h>
	#include <ffgui/gtk/loader.h>
#endif
#include <ffgui/loader.h>
#include <ffgui/gui.hpp>
#include <ffsys/thread.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>
#include <ffsys/process.h>

static const fcom_core *core;
static void gsync_signal(fcom_op *op, uint signal);

#define INCLUDE_ACTIONS(_) \
	_(A_WND_NEW), \
	_(A_SCAN_CMP), \
	_(A_SYNC), \
	_(A_SYNC_DATE), \
	_(A_SWAP), \
	_(A_INCLUDE_CHANGE), \
	_(A_EXCLUDE_CHANGE), \
	_(A_RECENTDAYS_CHANGE), \
	_(A_SHOW_LEFT), \
	_(A_SHOW_EQ), \
	_(A_SHOW_NEQ), \
	_(A_SHOW_MOV), \
	_(A_SHOW_RIGHT), \
	_(A_SHOW_DIR), \
	_(A_SHOW_DONE), \
	_(A_SHOW_NEQ_DATE), \
	_(A_SRC_EXEC), \
	_(A_DST_EXEC), \
	_(A_SRC_SHOW_DIR), \
	_(A_DST_SHOW_DIR), \
	_(A_SEL_ALL), \
	_(A_LIST_DISPLAY), \
	_(A_QUIT), \
	_(A_ONCLOSE),

#define _(id) id
enum {
	A_NONE,
	INCLUDE_ACTIONS(_)
};
#undef _

enum {
	COL_STATUS,
	COL_LDIR,
	COL_LNAME,
	COL_RDIR,
	COL_RNAME,
	COL_LSIZE,
	COL_RSIZE,
	COL_LDATE,
	COL_RDATE,
};

static void bit_toggle(uint *i, uint val)
{
	if (*i & val)
		*i &= ~val;
	else
		*i |= val;
}

static ffstr fftime_to_str(fftime t, xxstr_buf<100>& buf, uint flags)
{
	ffdatetime dt;
	fftime_split1(&dt, &t);
	buf.len = fftime_tostr1(&dt, buf.ptr, 100, flags);
	return buf;
}

/** "A;B" -> ["A", "B"] */
static ffslice str_split_array(ffstr s, char by, xxvec *dst)
{
	dst->len = 0;
	while (s.len) {
		ffstr it;
		ffstr_splitby(&s, by, &it, &s);
		if (it.len)
			*dst->push<ffstr>() = it;
	}
	return dst->slice();
}

struct sync_el {
	void *id;
	uint i;
};

struct gsync {
	fcom_cominfo	cominfo;

	fcom_cominfo *cmd;
	const fcom_sync_if *sync_if;
	fcom_sync_snapshot *lsnap, *rsnap;
	fcom_sync_diff *diff;
	struct fcom_sync_props props;

	uint		show_flags; // enum FCOM_SYNC
	xxvec		include, exclude; // ffstr[]
	xxvec		include_str, exclude_str;
	fcom_timer	timer;
	xxvec		sync_items; // struct sync_el[]
	uint		sync_item_i;
	fcom_task	core_task;
	uint		sync_flags;

	ffthread	thread;
	ffui_menu	mfile, mlist, mpopup;
	struct wmain {
		ffui_windowxx	wnd;
		ffui_menu		mmenu;
		ffui_labelxx	llpath, lrpath;
		ffui_editxx		lpath, rpath;
		ffui_labelxx	linclude, lexclude, ldays;
		ffui_editxx		include, exclude, recent_days;
		ffui_checkboxxx	show_left, show_eq, show_neq, show_mov, show_right, show_dir, show_done, show_neq_date;
		ffui_viewxx		view;
		ffui_statusbarxx stbar;
	} wmain;

	~gsync() {
		diff_reset();
	}

	void error(const char *e)
	{
		fcom_errlog(e);
		this->wmain.stbar.text(e);
	}

	void diff_reset()
	{
		this->sync_if->diff_free(this->diff);
		this->diff = NULL;
		this->sync_if->snapshot_free(this->lsnap);
		this->lsnap = NULL;
		this->sync_if->snapshot_free(this->rsnap);
		this->rsnap = NULL;
	}

	void redraw()
	{
		if (!this->diff) return;

		this->props.include = str_split_array(this->include_str.acquire(this->wmain.include.text()).str(), ';', &this->include);
		this->props.exclude = str_split_array(this->exclude_str.acquire(this->wmain.exclude.text()).str(), ';', &this->exclude);

		this->props.since_time.sec = 0;
		uint days = xxvec(this->wmain.recent_days.text()).str().uint16(0);
		if (days) {
			this->props.since_time = core->clock(NULL, FCOM_CORE_UTC);
			this->props.since_time.sec -= days * 60*60*24;
		}

		uint n = this->sync_if->view(this->diff, &this->props, this->show_flags);

		this->wmain.view.length(n, 1);

		xxstr_buf<100> buf;
		this->wmain.stbar.text(buf.zfmt("%u / %u", n, this->props.stats.entries));
	}

	void stats_redraw()
	{
		uint l = this->props.stats.left;
		uint r = this->props.stats.right;
		if (this->show_flags & FCOM_SYNC_SWAP) {
			l = this->props.stats.right;
			r = this->props.stats.left;
		}

		xxstr_buf<100> buf;
		this->wmain.show_left.text(buf.zfmt("%s (%u)"
			, "LEFT", l));
		this->wmain.show_eq.text(buf.zfmt("%s (%u)"
			, "EQ", this->props.stats.eq));
		this->wmain.show_neq.text(buf.zfmt("%s (%u)"
			, "NEQ", this->props.stats.neq));
		this->wmain.show_mov.text(buf.zfmt("%s (%u)"
			, "MOV", this->props.stats.moved));
		this->wmain.show_right.text(buf.zfmt("%s (%u)"
			, "RIGHT", r));
	}

	fcom_sync_snapshot* scan(xxstr path)
	{
		uint flags = 0;
		if (path.find_char('*') >= 0)
			return this->sync_if->scan_wc(path, flags);
		return this->sync_if->scan(path, flags);
	}

	static void scan_and_compare__worker(void *param)
	{
		gsync *g = (gsync*)param;
		g->diff_reset();

		ffui_thd_post(scan_status_update, g);
		if (!(g->lsnap = g->scan(xxvec(g->wmain.lpath.text()).str()))) {
			g->error("ERROR scanning Source tree");
			return;
		}

		ffui_thd_post(scan_status_update, g);
		if (!(g->rsnap = g->scan(xxvec(g->wmain.rpath.text()).str()))) {
			g->error("ERROR scanning Target tree");
			return;
		}

		ffui_thd_post(scan_status_update, g);
		uint f = FCOM_SYNC_DIFF_LEFT_PATH_STRIP
			| FCOM_SYNC_DIFF_RIGHT_PATH_STRIP
			| FCOM_SYNC_DIFF_NO_ATTR
			// | FCOM_SYNC_DIFF_NO_TIME
			| FCOM_SYNC_DIFF_TIME_2SEC;
		g->diff = g->sync_if->diff(g->lsnap, g->rsnap, &g->props, f);

		g->stats_redraw();
		g->redraw();
	}

	static const char* entry_status_str(xxstr_buf<100> *buf, uint status)
	{
		if (status & FCOM_SYNC_SYNCING)
			return "...";

		if (status & FCOM_SYNC_ERROR)
			return "ERROR";

		if (status & FCOM_SYNC_DONE)
			return "DONE";

		switch (status & FCOM_SYNC_MASK) {
		case FCOM_SYNC_LEFT:
			return "LEFT";

		case FCOM_SYNC_RIGHT:
			return "RIGHT";

		case FCOM_SYNC_NEQ: {
			int as = '.', ad = '.', aa = '.';
			if (status & FCOM_SYNC_NEWER)
				as = 'N';
			else if (status & FCOM_SYNC_OLDER)
				as = 'O';

			if (status & FCOM_SYNC_LARGER)
				ad = '>';
			else if (status & FCOM_SYNC_SMALLER)
				ad = '<';

			if (status & FCOM_SYNC_ATTR)
				aa = 'A';

			return buf->zfmt("NEQ[%c%c%c]", as, ad, aa);
		}

		case FCOM_SYNC_MOVE:
			return "MOVE";

		case FCOM_SYNC_EQ:
			return "EQ";
		}
		return NULL;
	}

	const struct fcom_sync_diff_entry* entry_at(uint i)
	{
		uint f = (this->show_flags & FCOM_SYNC_SWAP);
		return this->sync_if->info(this->diff, i, f);
	}

	void list_cell_draw(ffui_view_disp *disp)
	{
		if (!this->diff) return;

	#ifdef FF_WIN
		if (!(disp->mask & LVIF_TEXT))
			return;
	#endif

		const struct fcom_sync_diff_entry *ent = this->entry_at(ffui_view_dispinfo_index(disp));

		ffstr s = {};
		xxstr_buf<100> buf;

		switch (ffui_view_dispinfo_subindex(disp)) {
		case COL_STATUS:
			s = FFSTR_Z(entry_status_str(&buf, ent->status)); break;

		case COL_LDIR:
			ffpath_splitpath_str(ent->lname, &s, NULL); break;

		case COL_RDIR:
			ffpath_splitpath_str(ent->rname, &s, NULL); break;

		case COL_LNAME:
			ffpath_splitpath_str(ent->lname, NULL, &s); break;

		case COL_RNAME:
			ffpath_splitpath_str(ent->rname, NULL, &s); break;

		case COL_LSIZE:
			if (ent->left)
				s = buf.fmt("%,U", ent->left->size);
			break;

		case COL_RSIZE:
			if (ent->right)
				s = buf.fmt("%,U", ent->right->size);
			break;

		case COL_LDATE:
			if (ent->left)
				s = fftime_to_str(ent->left->mtime, buf, FFTIME_YMD);
			break;

		case COL_RDATE:
			if (ent->right)
				s = fftime_to_str(ent->right->mtime, buf, FFTIME_YMD);
			break;
		}

		if (s.len)
			ffui_view_dispinfo_settext(disp, s.ptr, s.len);
	}

	void src_dst_swap()
	{
		xxvec lp = this->wmain.lpath.text();
		this->wmain.lpath.text(this->wmain.rpath.text());
		this->wmain.rpath.text(lp.str());
		bit_toggle(&this->show_flags, FCOM_SYNC_SWAP);
		this->stats_redraw();
		this->redraw();
	}

	void sync_begin(uint flags)
	{
		if (this->sync_items.len) {
			this->error("sync already in progress");
			return;
		}

		this->sync_flags = flags;
		this->sync_prepare();
		if (this->sync_items.len)
			this->core_task_add(sync__worker);
	}

	void sync_prepare()
	{
		xxvec indices = this->wmain.view.selected();
		uint *it;
		FFSLICE_WALK(&indices, it) {
			const fcom_sync_diff_entry *de = this->entry_at(*it);
			if (!(de->status & FCOM_SYNC_DONE)) {
				struct sync_el *el = this->sync_items.push<struct sync_el>();
				el->id = de->id;
				el->i = *it;
			}
		}
		this->sync_item_i = 0;
	}

	static void scan_status_update(void *param)
	{
		struct gsync *g = (gsync*)param;
		const char *s = (!g->lsnap) ? "Scanning Source..."
			: (!g->rsnap) ? "Scanning Target..."
			: (!g->diff) ? "Comparing..."
			: NULL;
		if (!s)
			return;
		g->wmain.stbar.text(s);
	}

	static void sync_status_update(void *param)
	{
		struct gsync *g = (gsync*)param;
		g->wmain.stbar.text(xxstr_buf<100>().zfmt("Synchronizing: %u / %L"
			, g->sync_item_i, g->sync_items.len));
		g->stats_redraw();
	}

	void sync_entry_status_update__worker(const struct sync_el *el, uint st)
	{
		this->wmain.view.update(el->i, 0);

		uint status = this->sync_if->status(this->diff, el->id
			, FCOM_SYNC_SYNCING | FCOM_SYNC_ERROR | FCOM_SYNC_DONE, st);

		if (status & FCOM_SYNC_LEFT) {
			if (this->show_flags & FCOM_SYNC_SWAP)
				this->props.stats.right--;
			else
				this->props.stats.left--;
		}
		if (status & FCOM_SYNC_NEQ)
			this->props.stats.neq--;
		else if (status & FCOM_SYNC_MOVE)
			this->props.stats.moved--;
	}

	static void sync_entry_complete__worker(void *param, int result)
	{
		struct gsync *g = (gsync*)param;
		const struct sync_el *el = g->sync_items.at<struct sync_el>(g->sync_item_i);
		g->sync_entry_status_update__worker(el, (!result) ? FCOM_SYNC_DONE : FCOM_SYNC_ERROR);
		g->sync_item_i++;
		ffui_thd_post(sync_status_update, g);
		sync__worker(g);
	}

	static void sync__worker(void *param)
	{
		struct gsync *g = (gsync*)param;
		for (;  g->sync_item_i < g->sync_items.len;) {
			const struct sync_el *el = g->sync_items.at<struct sync_el>(g->sync_item_i);
			uint f = g->sync_flags
				| (g->show_flags & FCOM_SYNC_SWAP);
			int r = g->sync_if->sync(g->diff, el->id, f, sync_entry_complete__worker, g);
			uint st = 0;
			switch (r) {
			case 1:
				g->sync_if->status(g->diff, el->id, FCOM_SYNC_SYNCING, FCOM_SYNC_SYNCING);
				return; // wait for on_complete() to be called
			case 0:
				st = FCOM_SYNC_DONE; break;
			case -1:
				st = FCOM_SYNC_ERROR; break;
			}
			g->sync_entry_status_update__worker(el, st);
			g->sync_item_i++;
			ffui_thd_post(sync_status_update, g);
		}

		ffui_thd_post(sync_done, g);
	}

	static void sync_done(void *param)
	{
		struct gsync *g = (gsync*)param;
		g->sync_items.len = 0;
		g->wmain.stbar.text("Sync complete");
	}

	void quit()
	{
		ffui_post_quitloop();
	}

	void show_x(ffui_checkboxxx &cb, uint flag)
	{
		if (cb.checked())
			this->show_flags |= flag;
		else
			this->show_flags &= ~flag;
		this->redraw();
	}

	static void filter_change(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		g->redraw();
	}

	static void filter_change_timer(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		ffui_thd_post(filter_change, g);
	}

	void selected_file_exec(uint id)
	{
		int i = this->wmain.view.selected_first();
		if (i < 0)
			return;

		const fcom_sync_diff_entry *de = this->entry_at(i);
		ffstr name = {};
		if (id == A_SRC_EXEC)
			name = de->lname;
		else if (id == A_DST_EXEC)
			name = de->rname;

		if (name.len) {
			char *zname = ffsz_dupstr(&name);
			ffui_exec(zname);
			ffmem_free(zname);
		}
	}

	void selected_file_dir_show(uint id)
	{
		int i = this->wmain.view.selected_first();
		if (i < 0)
			return;

		const fcom_sync_diff_entry *de = this->entry_at(i);
		ffstr dir = {};
		if (id == A_SRC_SHOW_DIR && de->lname.len)
			ffpath_splitpath_str(de->lname, &dir, NULL);
		else if (id == A_DST_SHOW_DIR && de->rname.len)
			ffpath_splitpath_str(de->rname, &dir, NULL);

		if (dir.len) {
			char *zdir = ffsz_dupstr(&dir);
			ffui_openfolder1(zdir);
			ffmem_free(zdir);
		}
	}

	static void wnd_new__worker(void *param)
	{
		static const char *argv[] = {
			"fcom", "gsync", NULL
		};
		const char *exe_name = "fcom";
#ifdef FF_WIN
		exe_name = "fcom.exe";
#endif
		ffps ps = ffps_exec(xxvec(core->path(exe_name)).strz(), argv, core->env);
		fcom_dbglog("spawned PID %u", ffps_id(ps));
		ffps_close(ps);
	}

	void core_task_add(fcom_task_func func)
	{
		core->task(&this->core_task, func, this);
	}

	static void wmain_action(ffui_window *wnd, int id)
	{
		struct wmain *wmain = FF_STRUCTPTR(struct wmain, wnd, wnd);
		struct gsync *g = FF_STRUCTPTR(struct gsync, wmain, wmain);

		switch (id) {
		case A_WND_NEW:
			g->core_task_add(wnd_new__worker); break;

		case A_SCAN_CMP:
			g->core_task_add(scan_and_compare__worker); break;

		case A_SYNC:
			g->sync_begin(0); break;
		case A_SYNC_DATE:
			g->sync_begin(FCOM_SYNC_REPLACE_DATE); break;

		case A_SWAP:
			g->src_dst_swap(); break;

		case A_INCLUDE_CHANGE:
		case A_EXCLUDE_CHANGE:
		case A_RECENTDAYS_CHANGE:
			core->timer(&g->timer, -500, filter_change_timer, g); break;

		case A_SHOW_LEFT:
			g->show_x(g->wmain.show_left, FCOM_SYNC_LEFT); break;
		case A_SHOW_EQ:
			g->show_x(g->wmain.show_eq, FCOM_SYNC_EQ); break;
		case A_SHOW_NEQ:
			g->show_x(g->wmain.show_neq, FCOM_SYNC_NEQ); break;
		case A_SHOW_MOV:
			g->show_x(g->wmain.show_mov, FCOM_SYNC_MOVE); break;
		case A_SHOW_RIGHT:
			g->show_x(g->wmain.show_right, FCOM_SYNC_RIGHT); break;
		case A_SHOW_DIR:
			g->show_x(g->wmain.show_dir, FCOM_SYNC_DIR); break;
		case A_SHOW_DONE:
			g->show_x(g->wmain.show_done, FCOM_SYNC_DONE); break;
		case A_SHOW_NEQ_DATE:
			g->show_x(g->wmain.show_neq_date, FCOM_SYNC_NEWER | FCOM_SYNC_OLDER); break;

		case A_SRC_EXEC:
		case A_DST_EXEC:
			g->selected_file_exec(id); break;

		case A_SRC_SHOW_DIR:
		case A_DST_SHOW_DIR:
			g->selected_file_dir_show(id); break;

		case A_SEL_ALL:
			g->wmain.view.select_all(); break;

		case A_LIST_DISPLAY:
			g->list_cell_draw(g->wmain.view.dispinfo_item); break;

		case A_QUIT:
			g->wmain.wnd.close(); break;

		case A_ONCLOSE:
			g->quit(); break;
		}
	}

	static void* gui_ctl_find(void *udata, const ffstr *name)
	{
		#define _(m) FFUI_LDR_CTL(struct wmain, m)
		static const ffui_ldr_ctl wmain_ctls[] = {
			_(wnd),
			_(mmenu),
			_(llpath), _(lpath),
			_(lrpath), _(rpath),
			_(linclude), _(include),
			_(lexclude), _(exclude),
			_(ldays), _(recent_days),
			_(show_left), _(show_eq), _(show_neq), _(show_mov), _(show_right), _(show_dir), _(show_done), _(show_neq_date),
			_(view),
			_(stbar),
			FFUI_LDR_CTL_END
		};
		#undef _

		static const ffui_ldr_ctl top_ctls[] = {
			FFUI_LDR_CTL(struct gsync, mfile),
			FFUI_LDR_CTL(struct gsync, mlist),
			FFUI_LDR_CTL(struct gsync, mpopup),
			FFUI_LDR_CTL3(struct gsync, wmain, wmain_ctls),
			FFUI_LDR_CTL_END
		};
		return ffui_ldr_findctl(top_ctls, udata, name);
	}

	static int gui_cmd_find(void *udata, const ffstr *name)
	{
		static const char action_str[][24] = {
			#define _(id)  #id
			INCLUDE_ACTIONS(_)
			#undef _
		};

		for (uint i = 0;  i != FF_COUNT(action_str);  i++) {
			if (ffstr_eqz(name, action_str[i]))
				return i + 1;
		}
		return 0;
	}

	int load_ui()
	{
		int r = -1;
		char *fn = core->path("ops/gsync.ui");
		if (!fn)
			return -1;

		ffui_loader ldr;
		ffui_ldr_init(&ldr, gui_ctl_find, gui_cmd_find, this);

		if (ffui_ldr_loadfile(&ldr, fn)) {
			fcom_errlog("parsing gsync.ui: %s", ffui_ldr_errstr(&ldr));
			goto done;
		}

		r = 0;

	done:
		ffui_ldr_fin(&ldr);
		ffmem_free(fn);
		return r;
	}

	static void wmain_show(void *param)
	{
		struct gsync *g = (struct gsync*)param;

		if (g->cmd->input.len > 0)
			g->wmain.lpath.text(ffslice_itemT(&g->cmd->input, 0, ffstr)->ptr);

		if (g->cmd->input.len > 1)
			g->wmain.rpath.text(ffslice_itemT(&g->cmd->input, 1, ffstr)->ptr);

		g->show_flags = FCOM_SYNC_LEFT
			| FCOM_SYNC_RIGHT
			| FCOM_SYNC_NEQ
			| FCOM_SYNC_MOVE
			// | FCOM_SYNC_EQ
			// | FCOM_SYNC_DIR
			// | FCOM_SYNC_DONE
			| FCOM_SYNC_NEWER | FCOM_SYNC_OLDER
			;
		g->wmain.show_left.check(1);
		// g->wmain.show_eq.check(1);
		g->wmain.show_neq.check(1);
		g->wmain.show_mov.check(1);
		g->wmain.show_right.check(1);
		// g->wmain.show_dir.check(1);
		// g->wmain.show_done.check(1);
		g->wmain.show_neq_date.check(1);

		g->wmain.wnd.show(1);
	}

	static int FFTHREAD_PROCCALL gui_thread(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		ffui_init();
		if (g->load_ui())
			goto end;

		ffui_thd_post(wmain_show, g);

		fcom_dbglog("entering GUI loop");
		ffui_run();
		fcom_dbglog("exited GUI loop");

	end:
		ffui_uninit();
		gsync_signal((fcom_op*)g, 0);
		return 0;
	}

	void run()
	{
	#ifdef FF_WIN
		this->wmain.wnd.top = 1;
		this->wmain.wnd.manual_close = 1;
	#endif
		this->wmain.wnd.on_action = wmain_action;
		this->wmain.wnd.onclose_id = A_ONCLOSE;
		this->wmain.view.dispinfo_id = A_LIST_DISPLAY;

		if (FFTHREAD_NULL == (this->thread = ffthread_create(gui_thread, this, 0)))
			FCOM_ASSERT(0);
	}
};

static void gsync_close(fcom_op *op)
{
	struct gsync *s = (struct gsync*)op;
	s->~gsync();
	ffmem_free(s);
}

static fcom_op* gsync_create(fcom_cominfo *cmd)
{
	struct gsync *s = new(ffmem_new(struct gsync)) struct gsync;
	s->cmd = cmd;
	if (!(s->sync_if = (const fcom_sync_if*)core->com->provide("sync.if", 0)))
		goto end;

	static const struct ffarg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, s, FCOM_COM_AP_INOUT))
		goto end;

	return s;

end:
	gsync_close(s);
	return NULL;
}

static void gsync_run(fcom_op *op)
{
	struct gsync *s = (struct gsync*)op;
	s->run();
}

static void gsync_signal(fcom_op *op, uint signal)
{
	struct gsync *s = (struct gsync*)op;
	fcom_cominfo *cmd = s->cmd;
	gsync_close(s);
	core->com->complete(cmd, 0);
}

static const fcom_operation fcom_op_gsync = {
	gsync_create, gsync_close,
	gsync_run, gsync_signal,
	gsync_help,
};

FCOM_MOD_DEFINE(gsync, fcom_op_gsync, core)
