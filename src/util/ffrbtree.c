/**
Copyright (c) 2013 Simon Zolin
*/

#include "rbtree.h"
#include "array.h"


/** Make node become a left child of 'parent'. */
#define link_leftchild(parnt, left_child) \
do { \
	(parnt)->left = (left_child); \
	(left_child)->parent = (parnt); \
} while (0)

/** Make node become a right child of 'parent'. */
#define link_rightchild(parnt, right_child) \
do { \
	(parnt)->right = (right_child); \
	(right_child)->parent = (parnt); \
} while (0)

/** Make node become a left/right child of parent of 'nold'. */
static inline void relink_parent(fftree_node *nold, fftree_node *nnew, fftree_node **proot, void *sentl) {
	fftree_node *p = nold->parent;
	if (p == sentl)
		*proot = nnew;
	else if (nold == p->left)
		p->left = nnew;
	else
		p->right = nnew;
	nnew->parent = p;
}


fftree_node * fftree_findnode(ffrbtkey key, fftree_node **root, void *sentl)
{
	fftree_node *nod = *root;
	while (nod != sentl) {
		*root = nod;
		if (key < nod->key)
			nod = nod->left;
		else if (key > nod->key)
			nod = nod->right;
		else //key == nod->key
			return nod;
	}
	return NULL;
}

void ffrbtl_insert(ffrbtree *tr, ffrbtl_node *k)
{
	ffrbt_node *n = tr->root;
	ffrbtl_node *found = (ffrbtl_node*)fftree_findnode(k->key, (fftree_node**)&n, &tr->sentl);

	if (found == NULL)
		ffrbtl_insert3(tr, k, n);
	else {
		ffchain_append(&k->sib, &found->sib);
		tr->len++;
	}
}
