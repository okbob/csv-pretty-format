#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>

#include "unicode.h"

#ifndef offsetof
#define offsetof(type, field)	((long) &((type *)0)->field)
#endif							/* offsetof */


typedef struct
{
	int		nfields;
	char   *fields[];
} RowType;

typedef struct _rowBucketType
{
	int			nrows;
	RowType	   *rows[1000];
	bool		multilines[1000];
	bool		allocated;
	struct _rowBucketType *next_bucket;
} RowBucketType;

typedef struct
{
	char	   *buffer;
	int			processed;
	int			used;
	int			size;
	int			nfields;
	int			maxfields;
	int			starts[1024];		/* start of first char of column (in bytes) */
	int			sizes[1024];		/* lenght of chars of column (in bytes) */
	int			widths[1024];		/* display width of column */
	char		multilines[1024];		/* true, when column has some multiline chars */
} LinebufType;

typedef struct
{
	int			border;
	char		linestyle;
	char		separator;
} ConfigType;

static void *
smalloc(int size, char *debugstr)
{
	char *result;

	result = malloc(size);
	if (!result)
		exit(1);

	return result;
}

static void
print_vertical_header(FILE *ofile, LinebufType *linebuf, ConfigType *config, char pos)
{
	int		i;
	int		border = config->border;

	if (config->linestyle == 'a')
	{
		if ((border == 0 || border == 1) && (pos != 'm'))
			return;

		if (border == 2)
			fprintf(ofile, "+-");
		else if (border == 1)
			fprintf(ofile, "-");

		for (i = 0; i < linebuf->maxfields; i++)
		{
			int		j;

			if (i > 0)
			{
				if (border == 0)
					fprintf(ofile, " ");
				else
					fprintf(ofile, "-+-");
			}

			for (j = 0; j < linebuf->widths[i]; j++)
				fprintf(ofile, "-");
		}

		if (border == 2)
			fprintf(ofile, "-+");
		else if (border == 1)
			fprintf(ofile, "-");
		else if (border == 0 && linebuf->multilines[linebuf->maxfields - 1])
			fputc(' ', ofile);

		fprintf(ofile, "\n");
	}
	else if (config->linestyle == 'u')
	{
		if ((border == 0 || border == 1) && (pos != 'm'))
			return;

		if (border == 2)
		{
			if (pos == 't')
				fprintf(ofile, "\342\224\214");
			else if (pos == 'm')
				fprintf(ofile, "\342\224\234");
			else
				fprintf(ofile, "\342\224\224");

			fprintf(ofile, "\342\224\200");
		}
		else if (border == 1)
			fprintf(ofile, "\342\224\200");

		for (i = 0; i < linebuf->maxfields; i++)
		{
			int		j;

			if (i > 0)
			{
				if (border == 0)
					fprintf(ofile, " ");
				else
				{
					fprintf(ofile, "\342\224\200");
					if (pos == 't')
						fprintf(ofile, "\342\224\254");
					else if (pos == 'm')
						fprintf(ofile, "\342\224\274");
					else
						fprintf(ofile, "\342\224\264");

					fprintf(ofile, "\342\224\200");
				}
			}

			for (j = 0; j < linebuf->widths[i]; j++)
				fprintf(ofile, "\342\224\200");
		}

		if (border == 2)
		{
			fprintf(ofile, "\342\224\200");
			if (pos == 't')
				fprintf(ofile, "\342\224\220");
			else if (pos == 'm')
				fprintf(ofile, "\342\224\244");
			else
				fprintf(ofile, "\342\224\230");
		}
		else if (border == 1)
			fprintf(ofile, "\342\224\200");

		fprintf(ofile, "\n");
	}
}

/*
 * Header detection - simple heuristic, when first row has all text fields
 * and second rows has any numeric field, then csv has header.
 */
static bool
is_header(RowBucketType *rb)
{
	RowType	   *row;
	int		i;

	if (rb->nrows < 2)
		return false;

	row = rb->rows[0];

	for (i = 0; i < row->nfields; i++)
	{
		if (row->fields[i][0] == '\0')
			return false;
		if (isdigit((row->fields[i])[0]))
			return false;
	}

	row = rb->rows[1];

	for (i = 0; i < row->nfields; i++)
	{
		if (row->fields[i][0] == '\0')
			return true;
		if (isdigit((row->fields[i])[0]))
			return true;
	}

	return false;
}

static char *
fput_line(char *str, bool multiline, FILE *ofile)
{
	char   *nextline = NULL;

	if (multiline)
	{
		char   *ptr = str;
		int		size = 0;
		int		chrl;

		while (*ptr)
		{
			if (*ptr == '\n')
			{
				nextline = ptr + 1;
				break;
			}

			chrl = utf8charlen(*ptr);
			size += chrl;
			ptr += chrl;
		}

		fwrite(str, 1, size, ofile);
	}
	else
		fputs(str, ofile);

	return nextline;
}

int
main(int argc, char *argv[])
{
	FILE   *ifile = stdin;
	FILE   *ofile = stdout;

	LinebufType	linebuf;
	RowBucketType	rowbucket, *current;
	ConfigType		config;

	bool	skip_initial;
	bool	closed = false;
	bool	printed_headline = false;
	int		c;
	int		first_nw = 0;
	int		last_nw = 0;
	int		pos = 0;
	int		printed_rows = 0;
	int		instr = false;

	bool	last_multiline_column;
	int		last_column;

	setlocale(LC_ALL, "");

	memset(&linebuf, 0, sizeof(linebuf));

	linebuf.buffer = malloc(1024);
	linebuf.used = 0;
	linebuf.size = 1024;
	linebuf.nfields = 0;

	/* for debug purposes */
	memset(linebuf.buffer, 0, 1024);

	config.separator = -1;
	config.linestyle = 'a';
	config.border = 0;

	rowbucket.nrows = 0;
	rowbucket.allocated = false;
	rowbucket.next_bucket = NULL;

	current = &rowbucket;

	skip_initial = true;

	c = fgetc(ifile);
	do
	{
		if (c != EOF && (c != '\n' || instr))
		{
			int		l;

			if (skip_initial)
			{
				if (c == ' ')
					goto next_char;

				skip_initial = false;
				last_nw = first_nw;
			}

			if (linebuf.used >= linebuf.size)
			{
				linebuf.size += linebuf.size < (10 * 1024) ? linebuf.size  : (10 * 1024);
				linebuf.buffer = realloc(linebuf.buffer, linebuf.size);

				/* for debug purposes */
				memset(linebuf.buffer + linebuf.used, 0, linebuf.size - linebuf.used);
			}

			if (c == '"')
			{
				if (instr)
				{
					int     c2 = fgetc(ifile);

					if (c2 == '"')
					{
						/* double double quotes */
						linebuf.buffer[linebuf.used++] = c;
						pos = pos + 1;
					}
					else
					{
						/* start of end of string */
						ungetc(c2, ifile);
						instr = false;
					}
				}
				else
					instr = true;
			}
			else
			{
				linebuf.buffer[linebuf.used++] = c;
				pos = pos + 1;
			}

			if (config.separator == -1 && !instr)
			{
				/*
				 * Automatic separator detection - now it is very simple, first win.
				 * Can be enhanced in future by more sofisticated mechanism.
				 */
				if (c == ',')
					config.separator = ',';
				else if (c == ';')
					config.separator = ';';
				else if (c == '|')
					config.separator = '|';
			}

			if (config.separator != -1 && c == config.separator && !instr)
			{
				if (!skip_initial)
				{
					linebuf.sizes[linebuf.nfields] = last_nw - first_nw;
					linebuf.starts[linebuf.nfields++] = first_nw;
				}
				else
				{
					linebuf.sizes[linebuf.nfields] = 0;
					linebuf.starts[linebuf.nfields++] = -1;
				}

				skip_initial = true;
				first_nw = pos;
			}
			else if (instr || c != ' ')
			{
				last_nw = pos;
			}

			l = utf8charlen(c);
			if (l > 1)
			{
				int		i;

				/* read othe chars */
				for (i = 1; i < l; i++)
				{
					c = fgetc(ifile);
					if (c == EOF)
					{
						fprintf(stderr, "unexpected quit, broken unicode char\n");
						break;
					}

					linebuf.buffer[linebuf.used++] = c;
					pos = pos + 1;
				}
				last_nw = pos;
			}
		}
		else
		{
			char	   *locbuf;
			RowType	   *row;
			int			i;
			int			data_size;
			bool		multiline;

			if (!skip_initial)
			{
				linebuf.sizes[linebuf.nfields] = last_nw - first_nw;
				linebuf.starts[linebuf.nfields++] = first_nw;
			}
			else
			{
				linebuf.sizes[linebuf.nfields] = 0;
				linebuf.starts[linebuf.nfields++] = -1;
			}

			/* move row from linebuf to rowbucket */
			if (current->nrows >= 1000)
			{
				RowBucketType *new = smalloc(sizeof(RowBucketType), "RowBucketType");

				new->nrows = 0;
				new->allocated = true;
				new->next_bucket = NULL;

				current->next_bucket = new;
				current = new;
			}

			if (!linebuf.used)
				goto next_row;

			data_size = 0;
			for (i = 0; i < linebuf.nfields; i++)
				data_size += linebuf.sizes[i] + 1;

			locbuf = smalloc(data_size, "locbuf");
			memset(locbuf, 0, data_size);

			row = smalloc(offsetof(RowType, fields) + (linebuf.nfields * sizeof(char*)), "RowType");
			row->nfields = linebuf.nfields;

			multiline = false;

			for (i = 0; i < linebuf.nfields; i++)
			{
				int		width;
				bool	_multiline;

				row->fields[i] = locbuf;

				if (linebuf.sizes[i] > 0)
					memcpy(locbuf, linebuf.buffer + linebuf.starts[i], linebuf.sizes[i]);

				locbuf[linebuf.sizes[i]] = '\0';
				locbuf += linebuf.sizes[i] + 1;

				width = utf_string_dsplen_multiline(row->fields[i], linebuf.sizes[i], &_multiline, false);
				if (width > linebuf.widths[i])
					linebuf.widths[i] = width;

				multiline |= _multiline;
				linebuf.multilines[i] |= _multiline;
			}

			if (linebuf.nfields > linebuf.maxfields)
				linebuf.maxfields = linebuf.nfields;

			current->multilines[current->nrows] = multiline;
			current->rows[current->nrows++] = row;

next_row:

			linebuf.used = 0;
			linebuf.nfields = 0;

			linebuf.processed += 1;

			skip_initial = true;
			first_nw = 0;
			last_nw = 0;
			pos = 0;

			closed = c == EOF;
		}

next_char:

		c = fgetc(ifile);
	}
	while (!closed);

	current = &rowbucket;

	print_vertical_header(ofile, &linebuf, &config, 't');

	last_multiline_column = linebuf.multilines[linebuf.maxfields - 1];
	last_column = linebuf.maxfields - 1;

	while (current)
	{
		int		i;

		for (i = 0; i < current->nrows; i++)
		{
			int		j;
			bool	isheader = false;
			RowType	   *row;
			bool	free_row;
			bool	more_lines = true;
			bool	multiline = current->multilines[i];

			/*
			 * For multilines we can modify pointers so do copy now
			 */
			if (multiline)
			{
				RowType	   *source = current->rows[i];
				int			size;

				size = offsetof(RowType, fields) + (source->nfields * sizeof(char*));

				row = smalloc(size, "RowType");
				memcpy(row, source, size);

				free_row = true;
			}
			else
			{
				row = current->rows[i];
				free_row = false;
			}

			while (more_lines)
			{
				more_lines = false;

				if (config.border == 2)
				{
					if (config.linestyle == 'a')
						fprintf(ofile, "| ");
					else
						fprintf(ofile, "\342\224\202 ");
				}
				else if (config.border == 1)
					fprintf(ofile, " ");

				isheader = printed_rows == 0 ? is_header(&rowbucket) : false;

				for (j = 0; j < row->nfields; j++)
				{
					int		width;
					int		spaces;
					char   *field;
					bool	_more_lines = false;

					if (j > 0)
					{
						if (config.border != 0)
						{
							if (config.linestyle == 'a')
								fprintf(ofile, "| ");
							else
								fprintf(ofile, "\342\224\202 ");
						}
					}

					field = row->fields[j];

					if (field && *field != '\0')
					{
						bool	_isdigit = isdigit(field[0]);

						if (multiline)
						{
							width = utf_string_dsplen_multiline(field, -1, &_more_lines, true);
							more_lines |= _more_lines;
						}
						else
							width = utf_string_dsplen(field, -1);

						spaces = linebuf.widths[j] - width;

						/* left spaces */
						if (isheader)
							printf("%*s", spaces / 2, "");
						else if (_isdigit)
							printf("%*s", spaces, "");

						if (multiline)
							row->fields[j] = fput_line(row->fields[j], multiline, ofile);
						else
							(void) fput_line(row->fields[j], multiline, ofile);

						/* right spaces */
						if (isheader)
							printf("%*s", spaces - (spaces / 2), "");
						else if (!_isdigit)
							printf("%*s", spaces, "");
					}
					else
						printf("%*s", linebuf.widths[j], "");

					if (_more_lines)
					{
						if (config.linestyle == 'a')
							fputc('+', ofile);
						else
							fputs("\342\206\265", ofile);
					}
					else
					{
						if (config.border != 0 || j < last_column || last_multiline_column)
							fputc(' ', ofile);
					}
				}

				for (j = row->nfields; j < linebuf.maxfields; j++)
				{
					bool	addspace;

					if (j > 0)
					{
						if (config.border != 0)
						{
							if (config.linestyle == 'a')
								fprintf(ofile, "| ");
							else
								fprintf(ofile, "\342\224\202 ");
						}
					}

					addspace = config.border != 0 || j < last_column || last_multiline_column;

					fprintf(ofile, "%*s", linebuf.widths[j] + (addspace ? 1 : 0), "");
				}

				if (config.border == 2)
				{
					if (config.linestyle == 'a')
						fprintf(ofile, "|");
					else
						fprintf(ofile, "\342\224\202");
				}

				fprintf(ofile, "\n");

				if (isheader)
				{
					print_vertical_header(ofile, &linebuf, &config, 'm');
					printed_headline = true;
				}

				printed_rows += 1;
			}

			if (free_row)
				free(row);
		}

		current = current->next_bucket;
	}

	print_vertical_header(ofile, &linebuf, &config, 'b');

	fprintf(ofile, "(%d rows)\n", linebuf.processed - (printed_headline ? 1 : 0));
}
