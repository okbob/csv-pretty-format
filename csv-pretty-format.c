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
	int			starts[1024];
	int			sizes[1024];
	int			widths[1024];
	char		types[1024];
} LinebufType;

static void *
smalloc(int size, char *debugstr)
{
	char *result;

	result = malloc(size);
	if (!result)
		exit(1);
}

static void
print_vertical_header(LinebufType *linebuf)
{
	int		i;

	printf("+-");

	for (i = 0; i < linebuf->maxfields; i++)
	{
		int		j;

		if (i > 0)
			printf("-+-");

		for (j = 0; j < linebuf->widths[i]; j++)
			printf("-");
	}

	printf("-+\n");
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


int
main(int argc, char *argv[])
{
	FILE   *ifile = stdin;
	FILE   *ofile = stdout;

	LinebufType	linebuf;
	RowBucketType	rowbucket, *current;
	bool	skip_initial;
	int		c;
	int		first_nw = 0;
	int		last_nw = 0;
	int		pos = 0;
	int		printed_rows = 0;

	setlocale(LC_ALL, "");

	memset(&linebuf, 0, sizeof(linebuf));

	linebuf.buffer = malloc(1024);
	linebuf.used = 0;
	linebuf.size = 1024;
	linebuf.nfields = 0;

	/* for debug purposes */
	memset(linebuf.buffer, 0, 1024);

	rowbucket.nrows = 0;
	rowbucket.allocated = false;
	rowbucket.next_bucket = NULL;

	current = &rowbucket;

	skip_initial = true;

	c = fgetc(ifile);
	while (c != EOF)
	{
		bool	already_counted = false;

		if (c != '\n')
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

			linebuf.buffer[linebuf.used++] = c;
			pos = pos + 1;

			if (c == ',')
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
			else if (c != ' ')
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

			if (!skip_initial)
			{
				linebuf.sizes[linebuf.nfields] = last_nw - first_nw + 1;
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

			for (i = 0; i < linebuf.nfields; i++)
			{
				int		width;

				row->fields[i] = locbuf;

				if (linebuf.sizes[i] > 0)
					memcpy(locbuf, linebuf.buffer + linebuf.starts[i], linebuf.sizes[i]);

				locbuf[linebuf.sizes[i]] = '\0';
				locbuf += linebuf.sizes[i] + 1;

				width = utf_string_dsplen(row->fields[i], linebuf.sizes[i]);
				if (width > linebuf.widths[i])
					linebuf.widths[i] = width;
			}

			if (linebuf.nfields > linebuf.maxfields)
				linebuf.maxfields = linebuf.nfields;

			current->rows[current->nrows++] = row;

next_row:

			linebuf.used = 0;
			linebuf.nfields = 0;

			linebuf.processed += 1;

			skip_initial = true;
			first_nw = 0;
			last_nw = 0;
			pos = 0;
		}

next_char:

		c = fgetc(ifile);
	}


	printf("processed rows: %d\n", linebuf.processed);

	current = &rowbucket;

	print_vertical_header(&linebuf);

	while (current)
	{
		int		i;

		for (i = 0; i < current->nrows; i++)
		{
			int		j;
			bool	isheader = false;

			printf("| ");

			isheader = printed_rows == 0 ? is_header(&rowbucket) : false;

			for (j = 0; j < current->rows[i]->nfields; j++)
			{
				int		width;
				int		spaces;
				char   *field = current->rows[i]->fields[j];

				if (j > 0)
					printf(" | ");

				if (*field != '\0')
				{
					width = utf_string_dsplen(field, -1);
					spaces = linebuf.widths[j] - width;

					if (isheader)
					{
						printf("%*s", spaces / 2, "");
						printf("%s", field);
						printf("%*s", spaces - (spaces / 2), "");
					}
					else if (isdigit(field[0]))
					{
						printf("%*s", spaces, "");
						printf("%s", field);
					}
					else
					{
						printf("%s", field);
						printf("%*s", spaces, "");
					}
				}
				else
					printf("%*s", linebuf.widths[j], "");
			}

			for (j = current->rows[i]->nfields; j < linebuf.maxfields; j++)
			{
				if (j > 0)
					printf(" | ");

				printf("%*s", linebuf.widths[j], "");
			}

			printf(" |\n");

			if (isheader)
				print_vertical_header(&linebuf);

			printed_rows += 1;
		}

		current = current->next_bucket;
	}

	print_vertical_header(&linebuf);
}