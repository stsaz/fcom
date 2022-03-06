/** Binary tree.  Red-black tree.
Copyright (c) 2013 Simon Zolin
*/

#pragma once

#include "list.h"
#include <ffbase/rbtree.h>


typedef uint ffrbtkey;
typedef struct fftree_node fftree_node;

/** Binary tree node. */
struct fftree_node {
	fftree_node *left
		, *right
		, *parent;
	uint reserved;
	ffrbtkey key;
};

/** Search node.
Return NULL if not found.  'root' is set to the leaf node. */
FF_EXTERN fftree_node * fftree_findnode(ffrbtkey key, fftree_node **root, void *sentl);

/** Remove node. */
FF_EXTERN void ffrbt_rm(ffrbtree *tr, ffrbt_node *nod);

/** Node which holds pointers to its sibling nodes having the same key. */
typedef struct ffrbtl_node {
	ffrbt_node *left
		, *right
		, *parent;
	uint color;
	ffrbtkey key;

	fflist_item sib;
} ffrbtl_node;

/** Get RBT node by the pointer to its list item. */
#define ffrbtl_nodebylist(item)  FF_GETPTR(ffrbtl_node, sib, item)

/** Walk through sibling nodes.
The last node in row points to the first one - break the loop after this. */
#define FFRBTL_FOR_SIB(node, iter) \
for (iter = node \
	; iter != NULL \
	; ({ \
		iter = ffrbtl_nodebylist(iter->sib.next); \
		if (iter == node) \
			iter = NULL; \
		}) \
	)

/** Insert a new node or list-item. */
FF_EXTERN void ffrbtl_insert(ffrbtree *tr, ffrbtl_node *k);

static inline void ffrbtl_insert_withhash(ffrbtree *tr, ffrbtl_node *n, ffrbtkey hash)
{
	n->key = hash;
	ffrbtl_insert(tr, n);
}

#define ffrbtl_insert3(tr, nod, parent) \
do { \
	ffrbt_insert(tr, (ffrbt_node*)(nod), parent); \
	(nod)->sib.next = (nod)->sib.prev = &(nod)->sib; \
} while (0)
