/** GUI for file synchronization.
Copyright (c) 2019 Simon Zolin
*/

#include <util/gui-winapi/winapi-shell.h>
#include <fcom.h>
#include <util/gui-winapi/loader.h>
#include <util/gui-winapi/winapi.h>


/** Extensions to enum FSYNC_ST. */
enum {
	FSYNC_ST_ERROR = 1 << 28,
	FSYNC_ST_PENDING = 1 << 29, /** Operation on a file is pending */
	FSYNC_ST_CHECKED = 1 << 30,
	FSYNC_ST_DONE = 1 << 31,
};

enum {
	// 1<<FSYNC_ST_EQ
	// 1<<FSYNC_ST_SRC
	// 1<<FSYNC_ST_DEST
	// 1<<FSYNC_ST_MOVED
	// 1<<FSYNC_ST_NEQ
	SHOWMASK_NEWER = 1<<28,
	SHOWMASK_OLDER = 1<<29,
	SHOWMASK_DIRS = 1<<30,
};

#define fsfile(/* struct file* */f)  ((struct fsync_file*)(f))
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)


struct opts {
	char *fn;
	char *srcfn;
	char *dstfn;
	ffstr filter_name;
	ffstr filter;
	ffstr exclude;
	ffstr include;
	uint showmask; //enum FSYNC_ST
	uint show_modmask; //enum FSYNC_ST flags for FSYNC_ST_NEQ
	byte show_dirs_only;
	byte show_done;
	ffvec list_col_width; // uint[]
};

enum VOPTS_COLUMNS {
	VOPTS_NAME,
	VOPTS_VAL,
	VOPTS_DESC,
};

int opts_init(struct opts *c);
void opts_destroy(struct opts *o);
int opts_load(struct opts *c);
void opts_save(struct opts *c);
void opts_show(const struct opts *c, ffui_view *v);
int opts_set(struct opts *c, ffui_view *v, uint sub);
void list_cols_width_write(ffconfw *conf);
void wsync_pos_write(ffconfw *conf);
void wsync_opts_show();

struct wsync {
	ffui_wnd wsync;
	ffui_edit e1;
	ffui_edit e2;
	ffui_checkbox cbeq, cbnew, cbmod, cbdel, cbmov;
	ffui_checkbox cbshowdirs, cbshowolder, cbshownewer;
	ffui_label lexclude, linclude;
	ffui_edit eexclude, einclude;
	ffui_view vopts;
	ffui_view tdirs;
	ffui_view vlist;
	ffui_stbar stbar;
	ffui_menu mm;
};

struct wtree {
	ffui_wnd wnd;
	ffui_view tdirs;
	ffui_edit eaddr;
	ffui_view vlist;
	ffui_paned pn;
};

enum GST {
	GST_NONE,
	GST_READY,
	GST_SYNCING,
};

struct ggui {
	ffui_menu mcmd;
	ffui_menu mfile;
	struct wsync wsync;
	struct wtree wtree;
	ffui_dialog dlg;

	fsync_dir *src;
	fsync_dir *dst;
	ffarr cmptbl; // comparison results.  struct fsync_cmp[]
	ffarr cmptbl_filter; // filtered entries only.  struct fsync_cmp*[]
	struct opts opts;
	uint nchecked;
	char *filter_dirname; // full directory name to show entries in

	fftimerqueue_node tmr_excl_incl;
	fftask tsk;
	uint gstatus; // enum GST
	ffvec wndpos;

	fsync_dir *tree_dirs[50];
	uint ntree_dirs;
};

/** Get comparison result entry by index. */
#define list_getobj(i) \
	*ffarr_itemT(&gg->cmptbl_filter, i, struct fsync_cmp*)

enum CMDS {
#include "actions.h"
};


extern struct ggui *gg;
extern const fcom_core *core;
extern const fcom_command *com;
extern const fcom_fsync *fsync;
extern const fcom_fops *fops;


#define FILT_NAME  "gui.gsync"
#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define infolog(fmt, ...)  fcom_infolog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)


void update_status();
void gsync_sync(void *udata);

void tree_preinit();
void tree_init();
void tree_show(fsync_dir *d);
