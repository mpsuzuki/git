#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef enum {
	USTAR_NONE = 0,
	USTAR_PATHNAME,
	USTAR_UNAME,
	USTAR_GNAME,
	USTAR_UID,
	USTAR_GID,
	USTAR_SIZE,
	USTAR_MAX
} header_info_t;

typedef struct {
	size_t  size;
	char*   buff;
} block_t;

typedef struct {
	FILE*	file;
	size_t	pos;
	block_t	block;
} file_handle_t;

typedef struct past_line_t {
	const char*  line;
	struct past_line_t* next;
} past_line_t;

typedef struct {
	past_line_t*	begin;
	past_line_t*	end;
} past_lines_t;

typedef struct {
	/* params from arguments */
	header_info_t*  infos;
	size_t          num_infos;

	int  		uniq;
	int  		fail_if_multi;

	const char*  	pathname_tarfile;

	/* internal things */
	file_handle_t	handle;
	past_lines_t	past_lines;
} global_params_t;

#define USTAR_BLOCKSIZE 512

static void init_global_params(global_params_t *gp)
{
	gp->num_infos = 0;
	gp->uniq = 0;
	gp->fail_if_multi = 0;
	gp->pathname_tarfile = NULL;

	gp->handle.file = NULL;
	gp->handle.pos = 0;
	gp->handle.block.size = USTAR_BLOCKSIZE;
	gp->handle.block.buff = malloc(USTAR_BLOCKSIZE);

	gp->past_lines.begin = malloc(sizeof(past_line_t));
	gp->past_lines.begin->line = NULL;
	gp->past_lines.begin->next = NULL;
	gp->past_lines.end = gp->past_lines.begin;
}

size_t max(size_t a, size_t b)
{
	return (a > b) ? a : b;
}

size_t min(size_t a, size_t b)
{
	return (a > b) ? b : a;
}


void help_exit()
{
	puts("parse-tar [<options>] [<pathname>]");
	puts("parse-tar --show=<uid|gid|uname|owner|gname|group|name|size>");
	puts("parse-tar --print=<uid|gid|uname|owner|gname|group|name|size>");
	puts("parse-tar --uniq");
	puts("parse-tar --fail-if-multi");
	exit(0);
};

/* -------------------------------------------------------------- */


typedef struct  {
	const char*    nick;
	header_info_t  id;
} header_info_nick_s;


header_info_nick_s header_info_nicks[] = {
	{ "name", USTAR_PATHNAME },
	{ "uname", USTAR_UNAME },
	{ "owner", USTAR_UNAME },
	{ "gname", USTAR_GNAME },
	{ "group", USTAR_GNAME },
	{ "uid", USTAR_UID },
	{ "gid", USTAR_GID },
	{ "size", USTAR_SIZE }
};

header_info_t get_info_enum_from_str(const char* s)
{
	size_t i;
	size_t last_nick_num = sizeof(header_info_nicks) / sizeof(header_info_nick_s);
	for (i = 0; i < last_nick_num + 1; i++) {
		if (0 == strcasecmp(s, header_info_nicks[i].nick))
			return header_info_nicks[i].id;
	}
	return USTAR_NONE;
}

/* ------------------- */
/* functions to setup  */
/* ------------------- */

static int parse_args(int argc, const char **argv, global_params_t* gp)
{
	int i;

	init_global_params(gp);

	/* allocate info_to_print[argc] could be overkill, but sufficient */
	gp->infos = (header_info_t *)malloc(argc * sizeof(header_info_t));
	memset(gp->infos, 0, argc * sizeof(header_info_t));

	for (i = 1; i < argc; i ++) {
		header_info_t  id = USTAR_NONE;
		if (!strcasecmp("-?", argv[i]))
			help_exit();
		else
		if (!strcasecmp("-h", argv[i]))
			help_exit();
		else
		if (!strcasecmp("--help", argv[i]))
			help_exit();
		else
		if (!strncasecmp("--show=", argv[i], strlen("--show=")))
			id = get_info_enum_from_str(argv[i] + strlen("--show="));
		else
		if (!strncasecmp("--print=", argv[i], strlen("--print=")))
			id = get_info_enum_from_str(argv[i] + strlen("--print="));
		else
		if (!strcasecmp("--uniq", argv[i]))
			gp->uniq = 1;
		else
		if (!strcasecmp("--fail-if-multi", argv[i]))
			gp->fail_if_multi = 1;
		else
		if (argv[i][0] != '-' && !gp->pathname_tarfile)
			gp->pathname_tarfile = argv[i];

		if (id == USTAR_NONE)
			continue;
			
		gp->infos[gp->num_infos] = id;
		gp->num_infos ++;
	}
	return gp->num_infos;
}

/* --------------------------------------- */
/* functions to process the loaded header  */
/* --------------------------------------- */

typedef struct {
	char name[100];         /*   0 */
	char mode[8];           /* 100 */
	char uid[8];            /* 108 */
	char gid[8];            /* 116 */
	char size[12];          /* 124 */
	char mtime[12];         /* 136 */
	char chksum[8];         /* 148 */
	char typeflag[1];       /* 156 */
	char linkname[100];     /* 157 */
	char magic[6];          /* 257 */
	char version[2];        /* 263 */
	char uname[32];         /* 265 */
	char gname[32];         /* 297 */
	char devmajor[8];       /* 329 */
	char devminor[8];       /* 337 */
	char prefix[155];       /* 345 */
} ustar_header_t;

int is_empty_header(const ustar_header_t* h)
{
	int i;
	const char* c = (const char*)h;
	for (i = 0; i < sizeof(ustar_header_t); i ++) {
		if (c[i])
			return 0;
	}
	return 1;
}

const char *get_hdr_data_by_id(ustar_header_t* hdr, header_info_t id)
{
	switch (id) {
	case USTAR_PATHNAME: return hdr->name;
	case USTAR_UID:      return hdr->uid;
	case USTAR_GID:      return hdr->gid;
	case USTAR_UNAME:    return hdr->uname;
	case USTAR_GNAME:    return hdr->gname;
	case USTAR_SIZE:     return hdr->size;
	default: return NULL;
	}
}

size_t get_hdr_data_size_by_id(header_info_t id)
{
	switch (id) {
	case USTAR_PATHNAME: return sizeof(((ustar_header_t *)NULL)->name);
	case USTAR_UID:      return sizeof(((ustar_header_t *)NULL)->uid);
	case USTAR_GID:      return sizeof(((ustar_header_t *)NULL)->gid);
	case USTAR_UNAME:    return sizeof(((ustar_header_t *)NULL)->uname);
	case USTAR_GNAME:    return sizeof(((ustar_header_t *)NULL)->gname);
	case USTAR_SIZE:     return sizeof(((ustar_header_t *)NULL)->size);
	default: return 0;
	}
}

size_t get_dec_str_from_oct_str(char* buff, size_t buff_size, const char* oct_buff, int* failed)
{
	char*          end_of_oct;
	unsigned long  dec;           /* scratch buff for oct->dec */
	char           dec_buff[22];  /* scratch buff for dec->str: strlen(printf("%d", UINT64_MAX)) */

	/* parse octal value */
	dec = strtoul(oct_buff, &end_of_oct, 8);
	if (end_of_oct - oct_buff != strlen(oct_buff)) {
		fprintf(stderr, "*** cannot parse \"%s\" as octal numerical\n", oct_buff);
		*failed = -1;
		return 0;
	}

	/* make decimal expression */
	snprintf(dec_buff, sizeof(dec_buff), "%d", dec);
	if (buff_size < strlen(dec_buff) + 1) {
		fprintf(stderr, "*** too large number %d to write to the buffer[%d]\n", dec, buff_size);
		*failed = -1;
		return 0;
	}

	memset(buff, 0, buff_size);
	strncpy(buff, dec_buff, buff_size);
	return strlen(buff);
}


size_t get_printable_token(char* buff, size_t buff_size, ustar_header_t* hdr, header_info_t inf, int* failed)
{
	const char*    raw;

	char           oct_buff[200]; /* sufficient to cover the longest data in the header */

	raw = get_hdr_data_by_id(hdr, inf);

	switch (inf) {

	/* raw data should be printed */
	case USTAR_PATHNAME:
	case USTAR_UNAME:
	case USTAR_GNAME:
        	/*
		 * good tar file should null-terminated strings in the headers,
		 * but we prepare the case of non-terminated string cases.
		 */
		memset(buff, 0, buff_size);
		memcpy(buff, raw, min(buff_size - 1, get_hdr_data_size_by_id(inf)));
		return strlen(buff);

	/* octal data should be converted decimal */
	case USTAR_UID:
	case USTAR_GID:
	case USTAR_SIZE:
		memset(oct_buff, 0, sizeof(oct_buff));
		memcpy(oct_buff, raw, min(sizeof(oct_buff) - 1, get_hdr_data_size_by_id(inf)));
		return get_dec_str_from_oct_str(buff, buff_size, oct_buff, failed);

	default:
		return 0;
	}
}


size_t count_required_buff(ustar_header_t* hdr, size_t len_sep, global_params_t* gp, int* failed)
{
	char   buff[200]; /* sufficient to cover the longest data in the header */
	size_t len = 0;
	int i;

	for (i = 0; i < gp->num_infos; i++) {
		len += get_printable_token(buff, sizeof(buff), hdr, gp->infos[i], failed);
		if (0 < i)
			len += len_sep;
	}
	return (len + 1); // increment for NULL terminator
}

void fill_line_buff(char* buff, size_t buff_size, ustar_header_t*  hdr, const char* sep, global_params_t* gp, int *failed)
{
	size_t len_sep;
	char* cur;
	char* end;
	int i;

	len_sep = strlen(sep);
	memset(buff, 0, buff_size);
	cur = buff;
	end = buff + buff_size;
	for (i = 0; i < gp->num_infos; i++) {
		cur += get_printable_token(cur, (end - cur), hdr, gp->infos[i], failed);

		if (i < gp->num_infos - 1) {
			strncpy(cur, sep, (end - cur));
			cur += len_sep;
		}
	}
}

/* -------------------------------------------- */
/* functions to collect and search past output  */
/* -------------------------------------------- */

int no_past_lines(past_lines_t* pls)
{
	if (pls->begin->line != NULL)
		return 0; /* has a line, at least */
	else
		return 1;
}


int search_past_lines(const char* s, past_lines_t* pls)
{
	int i;
	past_line_t *pl;
	for (pl = pls->begin, i = 0 ; pl->line != NULL; pl = pl->next, i ++) {
		if (!strcmp(s, pl->line)) {
			return i;
		}	
	}
	return -1;
}

void append_past_line(past_lines_t* pls, char* buff)
{
	pls->end->line = buff;
	pls->end->next = malloc(sizeof(past_line_t));
	pls->end->next->line = NULL;
	pls->end->next->next = NULL;
	pls->end = pls->end->next;
}

/* -------------------------------- */
/* functions to process the stream  */
/* -------------------------------- */

size_t seek_to_next_block(file_handle_t* fh, int *failed)
{
	size_t overflow = (fh->pos % fh->block.size);
	size_t skip_size;

	if (overflow == 0)
		return 0;
	skip_size = fh->block.size - overflow;
	if (1 != fread(fh->block.buff, skip_size, 1, fh->file)) {
		*failed = -1;
		return -1;
	}
	fh->pos += skip_size;
	return skip_size;
}

size_t try_to_get_single_header(file_handle_t* fh, ustar_header_t* hdr, int* num_empty, int* failed)
{
	size_t  hdr_begin = fh->pos;

	if (feof(fh->file))
		return 0;
	else
	if (1 != fread(fh->block.buff, fh->block.size, 1, fh->file))
	{
		fprintf(stderr, "*** not EOF but cannot load a header from %08o\n", hdr_begin);
		*failed = -1;
		return 0;
	}
	fh->pos += fh->block.size;

	memcpy(hdr, fh->block.buff, sizeof(ustar_header_t));

	if (is_empty_header(hdr)) {
		fprintf(stderr, "*** empty header found at %08o, skip to next block\n", hdr_begin);
		seek_to_next_block(fh, failed);
		*num_empty = *num_empty + 1;
		return 0;
	}
	return fh->block.size;
}

int print_single_header_if_uniq(global_params_t* gp, char* buff, int *failed)
{
	if (0 <= search_past_lines(buff, &(gp->past_lines))) {
		/* found same line in the past, do not print */
		free(buff);
		return 0;
	}

	/* "--uniq" is given, but no same line in the past */
	if (gp->fail_if_multi && !no_past_lines(&(gp->past_lines))) {
		fprintf(stderr, "*** line \"%s\" differs from past \"%s\"\n", buff, gp->past_lines.begin->line);
		*failed = -2;
	} else {
		append_past_line(&(gp->past_lines), buff);
		puts(buff);
	}
	return strlen(buff);
}

/*
 *   -1: if we cannot calculate the length of line to print
 * >= 0: length of printed line (0 means nothing printed)
 */
int try_to_print_single_header(global_params_t* gp, ustar_header_t* hdr, int* failed)
{
	size_t  len;
	char*   buff;

	len = count_required_buff(hdr, strlen("\t"), gp, failed);
	if (*failed) {
		fprintf(stderr, "*** cannot calculate required size to print\n");
		return -1;
	}

	if (len == 0) {
		puts("");
		return 0;
	}

	buff = malloc(len);
	fill_line_buff(buff, len, hdr, "\t", gp, failed);

	if (gp->uniq)
		return print_single_header_if_uniq(gp, buff, failed);

	puts(buff);
	free(buff);
	return len;
}


size_t get_content_len_from_hdr(block_t* blk, ustar_header_t* hdr, int *failed)
{
	get_printable_token(blk->buff, blk->size, hdr, USTAR_SIZE, failed);
	if (*failed)
		return 0;

	return atol(blk->buff);
}

size_t skip_content(file_handle_t* fh, ustar_header_t* hdr, int *failed)
{
	int     l;
	size_t  hdr_begin, len_content;

	/* assume we used block for ustar header */
        hdr_begin = fh->pos - fh->block.size;

	len_content = get_content_len_from_hdr(&(fh->block), hdr, failed);
	if (len_content == 0)
		return sizeof(fh->block.size);

	for (l = 0; l < len_content; l += fh->block.size) {
		if (1 != fread(fh->block.buff, fh->block.size, 1, fh->file)) {
			fprintf(stderr, "*** fail in skipping the content\n");
			*failed = -1;
			return (fh->pos - hdr_begin);
		}
		fh->pos += fh->block.size;
	}

	/* skip the last half-filled block */
	seek_to_next_block(fh, failed);
	if (*failed)
		fprintf(stderr, "*** fail in seeking to the next block");

	return (fh->pos - hdr_begin);
}

size_t feed_single_item_tarfile(global_params_t* gp, int* num_empty, int* failed)
{
	ustar_header_t  hdr;
	size_t          hdr_begin;
	int             i;
	
	hdr_begin = gp->handle.pos;
	if (!try_to_get_single_header(&(gp->handle), &hdr, num_empty, failed) || *failed)
		return 0;

	/* non-empty header, reset length of empty headers */
	*num_empty = 0;
	if (0 > try_to_print_single_header(gp, &hdr, failed) || *failed)
		return sizeof(gp->handle.block.size);
	
	skip_content(&(gp->handle), &hdr, failed);
	return (gp->handle.pos - hdr_begin);
}

/* ----- */
/* main  */
/* ----- */

int main(int argc, const char **argv)
{
	int chunk_length;
	int failed = 0;
	int num_empty = 0;
	global_params_t gp;

	parse_args(argc, argv, &gp);

	/* check nothing is inappropriate test */
	if (!gp.num_infos)
		exit(-1);

	if (!gp.pathname_tarfile)
		gp.handle.file = stdin;
	else
		gp.handle.file = fopen(gp.pathname_tarfile, "r");

	if (!gp.handle.file) {
		if (gp.pathname_tarfile)
			fprintf(stderr, "*** cannot open %s\n", gp.pathname_tarfile);
		exit(-1);
	}

	do {
		chunk_length = feed_single_item_tarfile(&gp, &num_empty, &failed);
		if (chunk_length == 0 && num_empty > 1) {
			fprintf(stderr, "*** 2 empty headers found, take them as the end of tar\n");
			break;
		}
	} while (!failed);

	fclose(gp.handle.file);
	if (failed) {
		fprintf(stderr, "*** parse failed\n");
		exit(-2);
	}

	exit(0);
}
