/** Linked list.
Copyright (c) 2013 Simon Zolin
*/

#pragma once
#include "string.h"
#include "chain.h"
#include <ffbase/list.h>


typedef ffchain_item fflist_item;

/** Call func() for every item in list.
A list item pointer is translated (by offset in a structure) into an object.
Example:
fflist mylist;
struct mystruct_t {
	...
	fflist_item sibling;
};
FFLIST_ENUMSAFE(&mylist, ffmem_free, struct mystruct_t, sibling); */
#define FFLIST_ENUMSAFE(lst, func, struct_name, member_name) \
do { \
	fflist_item *li; \
	for (li = (lst)->root.next;  li != fflist_sentl(lst); ) { \
		void *p = FF_GETPTR(struct_name, member_name, li); \
		li = li->next; \
		func(p); \
	} \
} while (0)
