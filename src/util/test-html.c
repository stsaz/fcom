/** html.h test
2022, Simon Zolin */

#include "html.h"
#include <ffbase/../test/test.h>
#include <FFOS/ffos-extern.h>

void test_htmlread_read_simple()
{
	htmlread h = {};
	htmlread_open(&h);
	ffstr in, out;

	ffstr_setz(&in, "<tag>");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "<tag/>");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_TAG_CLOSE_SELF, htmlread_process(&h, &in, &out));
	xseq(&out, "");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "</tag>");
	xieq(HTML_TAG_CLOSE, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "<tag name=\"val\" name2=\"val2\"/>");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_ATTR, htmlread_process(&h, &in, &out));
	xseq(&out, "name");
	xieq(HTML_ATTR_VAL, htmlread_process(&h, &in, &out));
	xseq(&out, "val");
	xieq(HTML_ATTR, htmlread_process(&h, &in, &out));
	xseq(&out, "name2");
	xieq(HTML_ATTR_VAL, htmlread_process(&h, &in, &out));
	xseq(&out, "val2");
	xieq(HTML_TAG_CLOSE_SELF, htmlread_process(&h, &in, &out));
	xseq(&out, "");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "<tag name=\"val\">");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_ATTR, htmlread_process(&h, &in, &out));
	xseq(&out, "name");
	xieq(HTML_ATTR_VAL, htmlread_process(&h, &in, &out));
	xseq(&out, "val");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "text");
	xieq(HTML_TEXT, htmlread_process(&h, &in, &out));
	xseq(&out, "text");

	htmlread_close(&h);
}

void test_htmlread_read_whitespace()
{
	htmlread h = {};
	htmlread_open(&h);
	ffstr in, out;

	ffstr_setz(&in, "<tag \n >");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "<tag \n />");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_TAG_CLOSE_SELF, htmlread_process(&h, &in, &out));
	xseq(&out, "");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "</tag \n >");
	xieq(HTML_TAG_CLOSE, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, "<tag \n name \n = \n \"val\" \n />");
	xieq(HTML_TAG, htmlread_process(&h, &in, &out));
	xseq(&out, "tag");
	xieq(HTML_ATTR, htmlread_process(&h, &in, &out));
	xseq(&out, "name");
	xieq(HTML_ATTR_VAL, htmlread_process(&h, &in, &out));
	xseq(&out, "val");
	xieq(HTML_TAG_CLOSE_SELF, htmlread_process(&h, &in, &out));
	xseq(&out, "");
	xieq(HTML_MORE, htmlread_process(&h, &in, &out));
	x(in.len == 0);

	ffstr_setz(&in, " \n text \n ");
	xieq(HTML_TEXT, htmlread_process(&h, &in, &out));
	xseq(&out, " \n text \n ");

	htmlread_close(&h);
}

int main()
{
	test_htmlread_read_simple();
	test_htmlread_read_whitespace();
	return 0;
}
