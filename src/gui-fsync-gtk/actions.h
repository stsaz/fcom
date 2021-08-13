
#ifndef ACTION_NAMES
	#define A(id)  id
#else
	#define A(id)  #id
#endif

	A(A_NONE),

	A(A_CMP),
	A(A_SYNC),
	A(A_DISP),
	A(A_EXEC),
	A(A_SHOWEQ),
	A(A_SHOWNEW),
	A(A_SHOWMOD),
	A(A_SHOWMOVE),
	A(A_SHOWDEL),
	A(A_SHOW_DIRS),
	A(A_SHOW_OLDER),
	A(A_SHOW_NEWER),

	A(A_CLIPFN_LEFT),
	A(A_CLIPFN_RIGHT),
	A(A_DEL_LEFT),
	A(A_DEL_RIGHT),

#undef A
