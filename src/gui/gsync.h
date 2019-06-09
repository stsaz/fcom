/** GUI for file synchronization.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>

#include <FF/gui/loader.h>
#include <FF/gui/winapi.h>


/** Extensions to enum FSYNC_ST. */
enum {
	FSYNC_ST_ERROR = 1 << 28,
	FSYNC_ST_PENDING = 1 << 29, /** Operation on a file is pending */
	FSYNC_ST_CHECKED = 1 << 30,
	FSYNC_ST_DONE = 1 << 31,
};

#define fsfile(/* struct file* */f)  ((struct fsync_file*)(f))
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)


struct opts {
	char *fn;
	char *srcfn;
	char *dstfn;
	ffstr filter;
	uint showmask; //enum FSYNC_ST
	uint show_modmask; //enum FSYNC_ST flags for FSYNC_ST_NEQ
	byte show_dirs;
	byte time_diff_sec;
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

struct wsync {
	ffui_wnd wsync;
	ffui_view vopts;
	ffui_view tdirs;
	ffui_view vlist;
	ffui_paned pn;
	ffui_stbar stbar;
	ffui_menu mm;
};

struct ggui {
	ffui_menu mcmd;
	ffui_menu mfile;
	struct wsync wsync;
	ffui_dialog dlg;

	fsync_dir *src;
	fsync_dir *dst;
	ffarr cmptbl; // comparison results.  struct fsync_cmp[]
	ffarr cmptbl_filter; // filtered entries only.  struct fsync_cmp*[]
	struct opts opts;
	uint nchecked;
	char *filter_dirname; // full directory name to show entries in

	fftask tsk;
};

/** Get comparison result entry by index. */
#define list_getobj(i) \
	*ffarr_itemT(&gg->cmptbl_filter, i, struct fsync_cmp*)


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
