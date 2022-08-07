/** fcom fsync
2022, Simon Zolin
*/

static struct file* cur_nextfile(struct cursor *c);

static void cur_init(struct cursor *c, struct dir *d)
{
	c->ictx = 0;
	c->ctx[0].d = d;
	c->ctx[0].next = NULL;
	c->f = (void*)d->files.ptr;
	c->cur = cur_nextfile(c);
}

#define cur_get(c)  ((c)->cur)

#define cur_dir(c)  (c)->ctx[(c)->ictx].d

/** Get the next file at this level. */
static struct file* cur_nextfile(struct cursor *c)
{
	const struct cur_ctx *cx = &c->ctx[c->ictx];
	if (c->f == ffarr_endT(&cx->d->files, struct file)) {
		c->cur = NULL;
		return NULL;
	}
	c->cur = c->f++;
	return c->cur;
}

/** Get the next directory. */
static struct file* cur_nextdir(struct cursor *c)
{
	struct file *f;
	for (;;) {
		f = cur_nextfile(c);
		if (f == NULL)
			return NULL;
		if (isdir(f->attr))
			break;
	}
	return f;
}

static void cur_push(struct cursor *c, struct dir *d)
{
	c->ictx++;
	FF_ASSERT(c->ictx != FFCNT(c->ctx));
	c->ctx[c->ictx].d = d;
	c->ctx[c->ictx].next = NULL;
	c->f = (void*)d->files.ptr;
}

static void* cur_pop(struct cursor *c)
{
	if (c->ictx == 0)
		return NULL;
	c->ctx[c->ictx].next = NULL;
	c->ictx--;
	return &c->ctx[c->ictx];
}

/** Get next entry (post-increment algorithm).
Phase 1: Return all entries at this level
Phase 2: Recursively return all entries from sub-directories
Return NULL if no more entries. */
static struct file* cur_next(struct cursor *c)
{
	struct file *f;
	struct cur_ctx *cx;

	for (;;) {
		cx = &c->ctx[c->ictx];

		if (cx->next == NULL) {
			f = cur_nextfile(c);
			if (f != NULL)
				return f;

			c->f = (void*)cx->d->files.ptr;
			if (c->f == NULL) {
				// empty dir
				if (NULL == cur_pop(c))
					return NULL;
				continue;
			}
			cx->next = c->f;
			continue;
		}

		c->f = cx->next;
		f = cur_nextdir(c);
		if (f == NULL) {
			if (NULL == cur_pop(c))
				return NULL;
			continue;
		}
		cx->next = c->f;
		if (f->dir != NULL)
			cur_push(c, f->dir);
	}
}

static void cur_init2(struct cursor *c, struct dir *d)
{
	c->ictx = 0;
	c->ctx[0].d = d;
	c->ctx[0].next = (void*)d->files.ptr;
}

static void cur_push2(struct cursor *c, struct dir *d)
{
	c->ictx++;
	FF_ASSERT(c->ictx != FFCNT(c->ctx));
	c->ctx[c->ictx].d = d;
	c->ctx[c->ictx].next = (void*)d->files.ptr;
}

static struct file* cur_nextfile2(struct cursor *c)
{
	struct cur_ctx *cx = &c->ctx[c->ictx];
	if (cx->next == ffarr_endT(&cx->d->files, struct file))
		return NULL;
	return cx->next++;
}

/** Walk through a file tree.
. If file is a directory, enter it (increase level)
. Return file (not directory) entries at this level
. After the last entry at this level, decrease level, return the parent file entry
*/
static struct file* cur_next2(struct cursor *c)
{
	struct file *f;
	for (;;) {
		f = cur_nextfile2(c);
		if (f == NULL) {
			if (NULL == cur_pop(c))
				return NULL;
			return c->ctx[c->ictx].next - 1;
		}

		if (f->dir != NULL) {
			cur_push2(c, f->dir);
			continue;
		}

		return f;
	}
}
