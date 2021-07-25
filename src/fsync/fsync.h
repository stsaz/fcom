/** File synchornizer internal declarations.
Copyright (c) 2019 Simon Zolin */

#include <fcom.h>
#include <FF/data/conf2-writer.h>


struct dir;
struct file;

struct dir* dir_new(const ffstr *name);
struct file* dir_newfile(struct dir *d);
const char* dir_path(struct dir *d);
struct file* tree_file_find(struct dir *d, const ffstr *name);

struct file {
	// struct fsync_file
	char *name;
	uint64 size;
	fftime mtime;
	uint attr;

	struct dir *parent;
	struct dir *dir;
	ffchain_item sib;
	ffrbtl_node nod_name, nod_props;

	/** The file is paired with another.
	The flag prevents the same file from being used twice. */
	uint moved :1;
};


struct dir* snapshot_load(const char *name, uint flags);
void snapshot_writedir(ffconfw *cw, struct dir *d, ffbool close);
void snapshot_writefile(ffconfw *cw, const struct file *f);

extern const fcom_core *core;
extern const fcom_fsync fsync_if;


#define FILT_NAME  "fsync"

#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define warnlog(fmt, ...)  fcom_warnlog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

