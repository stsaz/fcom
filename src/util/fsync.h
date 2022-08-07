
uint fsync_cmp_status_swap(uint st)
{
	if ((st & _FSYNC_ST_MASK) == FSYNC_ST_SRC)
		st = FSYNC_ST_DEST | (st & ~_FSYNC_ST_MASK);
	else if ((st & _FSYNC_ST_MASK) == FSYNC_ST_DEST)
		st = FSYNC_ST_SRC | (st & ~_FSYNC_ST_MASK);

	if (st & FSYNC_ST_SMALLER)
		st = (st & ~FSYNC_ST_SMALLER) | FSYNC_ST_LARGER;
	else if (st & FSYNC_ST_LARGER)
		st = (st & ~FSYNC_ST_LARGER) | FSYNC_ST_SMALLER;

	if (st & FSYNC_ST_OLDER)
		st = (st & ~FSYNC_ST_OLDER) | FSYNC_ST_NEWER;
	else if (st & FSYNC_ST_NEWER)
		st = (st & ~FSYNC_ST_NEWER) | FSYNC_ST_OLDER;

	return st;
}
