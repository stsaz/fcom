/** HTML parser
2022, Simon Zolin */

/*
htmlread_open htmlread_close
htmlread_process
*/

#include <ffbase/string.h>

typedef struct htmlread {
	ffuint state, nextstate;
	ffuint64 line;
	int out_preserve;
	int tag_lslash;
} htmlread;

void htmlread_open(htmlread *h)
{
	h->line = 1;
}

void htmlread_close(htmlread *h)
{
}

enum HTML_R {
	/** Need more input data (preserving the current data). */
	HTML_MORE,
	HTML_TEXT, // everything between tags
	HTML_TAG, // <tag>
	HTML_TAG_CLOSE, // </tag>
	HTML_TAG_CLOSE_SELF, // <.../>
	HTML_ATTR, // <tag attr=...>
	HTML_ATTR_VAL, // <tag attr=val>
};

/**
Skips invalid data inside tags.
Does NOT convert "&XXXX;"-escape sequences.
Return enum HTML_R */
int htmlread_process(htmlread *h, ffstr *in, ffstr *out)
{
	enum I {
		I_TEXT, I_TAG_1, I_TAG,
		I_WSPACE, I_ATTR, I_ATTR_EQ, I_ATTRVAL, I_ANGL_R,
	};
	int c;
	ffssize i = 0;

	for (;;) {
		switch (h->state) {

		case I_TEXT: // return all text until next '<'
			if (h->out_preserve)
				h->out_preserve = 0;
			else
				out->ptr = in->ptr;

			if (0 > (i = ffstr_findany(in, "<\n", 2))) {
				out->len = in->ptr + in->len - out->ptr;
				if (out->len == 0)
					return HTML_MORE;
				ffstr_shift(in, in->len);
				return HTML_TEXT;
			}

			if (in->ptr[i] == '\n') {
				h->line++;
				h->out_preserve = 1;
				i++;
				ffstr_shift(in, i);
				continue;
			}

			if (i != 0) {
				out->len = in->ptr + i - out->ptr;
				ffstr_shift(in, i);
				return HTML_TEXT;
			}

			ffstr_shift(in, 1);
			h->state = I_TAG_1;
			// fallthrough

		case I_TAG_1: // "<": handle "</"
			if (in->len != 0 && in->ptr[i] == '/') {
				h->tag_lslash = 1; // </...
				ffstr_shift(in, 1);
			}
			h->state = I_TAG;
			// fallthrough

		case I_TAG: // "<" or "</": return tag name
			if (0 > (i = ffs_skip_ranges(in->ptr, in->len, "09AZaz", 6)))
				return HTML_MORE;
			else if (i == 0) { // <.
				h->state = I_ANGL_R;
				continue;
			}

			ffstr_set(out, in->ptr, i);
			ffstr_shift(in, i);

			if (h->tag_lslash) { // </tag...
				h->tag_lslash = 0;
				h->state = I_ANGL_R;
				return HTML_TAG_CLOSE;
			}

			h->state = I_WSPACE,  h->nextstate = I_ATTR;
			return HTML_TAG;

		case I_WSPACE: // skip whitespace, count lines
			if (0 > (i = ffs_skip_ranges(in->ptr, in->len, "\x00\x09\x0b\x20", 4))) { // whitespace excluding '\n'
				ffstr_shift(in, in->len);
				return HTML_MORE;
			}
			if (in->ptr[i] == '\n') {
				h->line++;
				ffstr_shift(in, i + 1);
				continue;
			}
			ffstr_shift(in, i);
			h->state = h->nextstate;
			continue;

		case I_ATTR: // "<tag ": return attribute name
			if (0 > (i = ffs_skip_ranges(in->ptr, in->len, "09AZaz", 6))) {
				return HTML_MORE;
			} else if (i == 0) { // <tag .

				if (in->ptr[i] == '>') { // <tag...>
					ffstr_shift(in, 1);
					h->state = I_TEXT;
					continue;

				} else if (in->ptr[i] == '/') { // <tag.../
					ffstr_shift(in, 1);
					ffstr_null(out);
					h->state = I_ANGL_R;
					return HTML_TAG_CLOSE_SELF;
				}

				h->state = I_ANGL_R;
				continue;
			}

			ffstr_set(out, in->ptr, i);
			ffstr_shift(in, i);
			h->state = I_WSPACE,  h->nextstate = I_ATTR_EQ;
			return HTML_ATTR;

		case I_ATTR_EQ: // "<tag attr": handle "="
			if (in->ptr[0] != '=') { // "<tag attr "
				h->state = I_WSPACE,  h->nextstate = I_ATTR;
				continue;
			}
			ffstr_shift(in, 1);
			h->state = I_WSPACE,  h->nextstate = I_ATTRVAL;
			// fallthrough

		case I_ATTRVAL: // "<tag attr=": return attribute value
			if (!(in->ptr[0] == '"' || in->ptr[0] == '\'')) {
				//TODO
				// h->state = I_ATTR;
				continue;
			}

			if (0 > (i = ffs_findany(in->ptr + 1, in->len - 1, "\"\n", 2)))
				return HTML_MORE;
			//TODO line++

			ffstr_set(out, in->ptr + 1, i);
			ffstr_shift(in, i + 2);
			h->state = I_WSPACE,  h->nextstate = I_ATTR;
			return HTML_ATTR_VAL;

		case I_ANGL_R: // skip input until next '>'
			if (0 > (i = ffstr_findany(in, ">\n", 2))) {
				ffstr_shift(in, in->len);
				return HTML_MORE;
			}

			c = in->ptr[i];
			ffstr_shift(in, i + 1);
			if (c == '\n') {
				h->line++;
				continue;
			}

			h->state = I_TEXT;
			continue;
		}
	}
	return -1;
}
