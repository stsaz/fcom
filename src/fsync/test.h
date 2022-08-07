
// #define TEST
#include <ffbase/../test/test.h>
void test_ffpath_parent()
{
#ifdef TEST
	ffstr a, b, parent;

	ffstr_setz(&a, "/d/x");
	ffstr_setz(&b, "/d/x");
	x(0 == ffpath_parent(&a, &b, &parent));
	xseq(&parent, "/d/x");

	ffstr_setz(&a, "/d/x");
	ffstr_setz(&b, "/d/x2");
	x(0 == ffpath_parent(&a, &b, &parent));
	xseq(&parent, "/d");

	ffstr_setz(&a, "/d/x/1");
	ffstr_setz(&b, "/d/x/2");
	x(0 == ffpath_parent(&a, &b, &parent));
	xseq(&parent, "/d/x");

	ffstr_setz(&a, "/d/x");
	ffstr_setz(&b, "/d/x/2");
	x(0 == ffpath_parent(&a, &b, &parent));
	xseq(&parent, "/d/x");

	ffstr_setz(&a, "/d/x/2");
	ffstr_setz(&b, "/d/x");
	x(0 == ffpath_parent(&a, &b, &parent));
	xseq(&parent, "/d/x");
#endif
}

void test_ffpath_cmp()
{
#ifdef TEST
	ffstr a, b;
	ffstr_setz(&a, "/x");
	ffstr_setz(&b, "/x/1");
	x(ffpath_cmp(&a, &b, 0) < 0);

	ffstr_setz(&a, "/x/1");
	ffstr_setz(&b, "/x-1");
	x(ffpath_cmp(&a, &b, 0) < 0);

	ffstr_setz(&a, "/x-1");
	ffstr_setz(&b, "/x/1");
	x(ffpath_cmp(&a, &b, 0) > 0);

	ffstr_setz(&a, "\\x-1");
	ffstr_setz(&b, "\\x\\1");
	x(ffpath_cmp(&a, &b, FFPATH_SUPPORT_BACKSLASH) > 0);
#endif
}
