/** Path.
Copyright (c) 2013 Simon Zolin
*/

#pragma once

#include "array.h"
#include <FFOS/path.h>


enum FFPATH_FLAGS {
	/** Merge duplicate slashes.
	 "/1///2" -> "/1/2" */
	FFPATH_MERGESLASH = 1,

	/** Handle "." and ".." in path.
	 "./1/./2/3" -> "1/2/3"
	 "/../1/../2/3" -> "/2/3"
	Note: ".." in relative paths such as below aren't merged:
	 "../1/../2/3" -> "../1/../2/3" */
	_FFPATH_MERGEDOTS = 2,
	FFPATH_MERGEDOTS = _FFPATH_MERGEDOTS | FFPATH_MERGESLASH,

	/** Fail if path is out of bounds.
	 "/../1" or "../1" -> ERROR */
	_FFPATH_STRICT_BOUNDS = 4,
	FFPATH_STRICT_BOUNDS = _FFPATH_STRICT_BOUNDS | FFPATH_MERGESLASH,

	/** Disable automatic FFPATH_WINDOWS. */
	FFPATH_NOWINDOWS = 8,

	/** Support "\" backslash and Windows disk drives "x:".  Always enabled on Windows.
	Fail if found a character prohibited to use in filename on Windows: *?:" */
	FFPATH_WINDOWS = 0x10,

	/** Convert all "\" slashes to "/".
	 "c:\1\2" -> "c:/1/2" */
	FFPATH_FORCESLASH = 0x20,

	/** Convert all "/" slashes to "\".
	 "c:/1/2" -> "c:\1\2" */
	FFPATH_FORCEBKSLASH = 0x40,

	/** Convert path to relative.
	 "/path" -> "path"
	 "../1/2" -> "1/2"
	 "c:/path" -> "path" (with FFPATH_WINDOWS) */
	FFPATH_TOREL = 0x100,
};

/** Process an absolute or relative path.
@dstcap: always safe if it's at least @len bytes
@flags: enum FFPATH_FLAGS;  default: FFPATH_MERGESLASH | FFPATH_MERGEDOTS.
Return the number of bytes written in 'dst';  0 on error. */
FF_EXTERN size_t ffpath_norm(char *dst, size_t dstcap, const char *path, size_t len, int flags);

/** Replace characters that can not be used in a filename. */
FF_EXTERN size_t ffpath_makefn_full(char *dst, size_t dstcap, const char *src, size_t len, uint flags);

/** Make filename: [out_dir/] [in_dir/] in_name .out_ext
idir: the path is converted to relative and normalized */
FF_EXTERN int ffpath_makefn_out(ffarr *fn, const ffstr *idir, const ffstr *iname, const ffstr *odir, const ffstr *oext);

#if defined FF_UNIX
#define ffpath_findslash(path, len)  ffs_find(path, len, '/')

/** Find the last slash in path. */
#define ffpath_rfindslash(path, len)  ffs_rfind(path, len, '/')

#else
#define ffpath_findslash(path, len)  ffs_findof(path, len, "/\\", 2)
#define ffpath_rfindslash(path, len)  ffs_rfindof(path, len, "/\\", 2)
#endif

/** Get filename and directory (without the last slash). */
static inline const char* ffpath_split2(const char *fn, size_t len, ffstr *dir, ffstr *name)
{
	ffssize r = ffpath_splitpath(fn, len, dir, name);
	if (r < 0)
		return NULL;
	return &fn[r];
}

static inline const char* ffpath_split3(const char *fullname, size_t len, ffstr *path, ffstr *name, ffstr *ext)
{
	ffstr nm;
	const char *slash = ffpath_split2(fullname, len, path, &nm);
	ffpath_splitname(nm.ptr, nm.len, name, ext);
	return slash;
}

/** Get the next file path part. */
FF_EXTERN ffstr ffpath_next(ffstr *path);

enum FFPATH_CASE {
	FFPATH_CASE_DEF,
	FFPATH_CASE_SENS,
	FFPATH_CASE_ISENS,
};

enum {
	FFPATH_SUPPORT_BACKSLASH = 0x10,
};

/** Compare normalized file paths.
@flags: enum FFPATH_CASE | FFPATH_SUPPORT_BACKSLASH
Return 0 if equal;  <0 if p1 < p2;  >0 otherwise. */
FF_EXTERN int ffpath_cmp(const ffstr *p1, const ffstr *p2, uint flags);

/** Get max. shared parent directory (without the last slash).
p1,p2: Normalized paths
Return !=0 if the paths don't have a shared parent. */
FF_EXTERN int ffpath_parent(const ffstr *p1, const ffstr *p2, ffstr *dir);

/** Check if a path is a parent of another path.
@flags: enum FFPATH_CASE
Return TRUE if match. */
FF_EXTERN ffbool ffpath_match(const ffstr *path, const ffstr *match, uint flags);
