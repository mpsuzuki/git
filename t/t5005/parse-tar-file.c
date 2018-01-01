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

typedef struct past_line_t {
	const char*  line;
	struct past_line_t* next;
} past_line_t;

typedef struct {
	header_info_t*  infos;
	size_t          num_infos;

	int  		uniq;
	int  		fail_if_multi;

	const char*  	pathname_tarfile;

	size_t		block_size;
	char*		block_buff;

	FILE*		fh;
	size_t		pos;

	/* linked list to cache past lines */
	past_line_t	past_lines;
	past_line_t*	past_lines_begin;
	past_line_t*	past_lines_end;

} global_params_t;

#define USTAR_BLOCKSIZE 512

static void init_global_params(global_params_t *gp)
{
	gp->num_infos = 0;
	gp->uniq = 0;
	gp->fail_if_multi = 0;
	gp->pathname_tarfile = NULL;

	gp->block_size = USTAR_BLOCKSIZE;
	gp->block_buff = malloc(USTAR_BLOCKSIZE);

	gp->fh = NULL;
	gp->pos = 0;

	gp->past_lines.line = NULL;
	gp->past_lines.next = NULL;
	gp->past_lines_begin = &(gp->past_lines);
	gp->past_lines_end = &(gp->past_lines);
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
	puts("parse-tar --show=<uid|gid|uname|owner|gname|group|name>");
	puts("parse-tar --print=<uid|gid|uname|owner|gname|group|name>");
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

char block_buff[USTAR_BLOCKSIZE];
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

int is_empty_header(const char* h)
{
	int i;
	for (i = 0; i < sizeof(ustar_header_t); i ++) {
		if (h[i])
			return 0;
	}
	return 1;
}

const char *get_hdr_data_by_id(ustar_header_t* hdr, header_info_t id)
{
	switch (id) {
	case USTAR_PATHNAME: return hdr->name;
	case USTAR_UID: return hdr->uid;
	case USTAR_GID: return hdr->gid;
	case USTAR_UNAME: return hdr->uname;
	case USTAR_GNAME: return hdr->gname;
	case USTAR_SIZE: return hdr->size;
	default: return NULL;
	}
}

int get_printable_token(char* buff, size_t buff_size, ustar_header_t* hdr, header_info_t inf, int* failed)
{
	const char*   raw;
	char*         end_of_oct;
	char          oct_buff[200]; /* sufficient to cover the longest data in the header */
	unsigned long dec;           /* scratch buff for oct->dec */
	char          dec_buff[22];  /* scratch buff for dec->str: strlen(printf("%d", UINT64_MAX)) */

	raw = get_hdr_data_by_id(hdr, inf);

	switch (inf) {
	/* raw data should be printed */
	case USTAR_PATHNAME:
		strncpy(buff, raw, min(buff_size, sizeof(hdr->name)));
		return strlen(buff);
	case USTAR_UNAME:
	case USTAR_GNAME:
		memset(buff, 0, buff_size);
		strncpy(buff, raw, min(buff_size, sizeof(hdr->uname)));
		return strlen(buff);

	/* octal data should be converted decimal */
	case USTAR_UID:
	case USTAR_GID:
		memset(oct_buff, 0, sizeof(oct_buff));
		strncpy(oct_buff, raw, min(sizeof(oct_buff), sizeof(hdr->uid)));
		break;

	case USTAR_SIZE:
		memset(oct_buff, 0, sizeof(oct_buff));
		strncpy(oct_buff, raw, min(sizeof(oct_buff), sizeof(hdr->size)));
		break;

	default:
		return 0;
	}

	/* handle octal numerics */
	dec = strtoul(oct_buff, &end_of_oct, 8);
	if (end_of_oct - oct_buff != strlen(oct_buff)) {
		fprintf(stderr, "*** cannot parse UID/GID/SIZE from \"%s\"\n", raw);
		*failed = -1;
		return 0;
	}

	/* parse octal value */
	snprintf(dec_buff, sizeof(dec_buff), "%d", dec);
	if (buff_size < strlen(dec_buff) + 1) {
		fprintf(stderr, "*** too large number %d to write to the buffer[%d]\n", dec, buff_size);
		*failed = -1;
	}
	memset(buff, 0, buff_size);
	strncpy(buff, dec_buff, min(buff_size, strlen(dec_buff) + 1));
	return strlen(buff);
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

int search_past_lines(const char* s, global_params_t* gp)
{
	int i;
	past_line_t *pl;
	for (pl = gp->past_lines_begin, i = 0 ; pl->line != NULL; pl = pl->next, i ++) {
		if (!strcmp(s, pl->line)) {
			return i;
		}	
	}
	return -1;
}

void append_past_line(global_params_t* gp, char* buff)
{
	gp->past_lines_end->line = buff;
	gp->past_lines_end->next = malloc(sizeof(past_line_t));
	gp->past_lines_end->next->line = NULL;
	gp->past_lines_end->next->next = NULL;
	gp->past_lines_end = gp->past_lines_end->next;
}

/* -------------------------------- */
/* functions to process the stream  */
/* -------------------------------- */

size_t seek_to_next_block(global_params_t* gp, int *failed)
{
	size_t overflow = (gp->pos % gp->block_size);
	size_t skip_size;

	if (overflow == 0)
		return 0;
	skip_size = gp->block_size - overflow;
	if (1 != fread(gp->block_buff, skip_size, 1, gp->fh)) {
		*failed = -1;
		return -1;
	}
	gp->pos += skip_size;
	return skip_size;
}

size_t feed_single_item_tarfile(global_params_t* gp, int* num_empty, int* failed)
{
	ustar_header_t  hdr;
	size_t          len;
	char*           buff;
	size_t          len_content;
	size_t          hdr_begin = gp->pos;

	int             i;


	if (feof(gp->fh))
		return 0;
	else
	if (1 != fread(gp->block_buff, gp->block_size, 1, gp->fh))
	{
		fprintf(stderr, "*** not EOF but cannot load a header from %08o\n", hdr_begin);
		*failed = -1;
		return 0;
	}
	gp->pos += gp->block_size;

	memcpy(&hdr, gp->block_buff, sizeof(hdr));
	if (is_empty_header((const char*)&hdr)) {
		fprintf(stderr, "*** empty header found at %08o, skip to next block\n", hdr_begin);
		seek_to_next_block(gp, failed);
		*num_empty = *num_empty + 1;
		return 0;
	}

	/* non-empty header, reset length of empty headers */
	*num_empty = 0;
	len = count_required_buff(&hdr, strlen("\t"), gp, failed);
	if (*failed) {
		fprintf(stderr, "*** cannot calculate required size to print\n");
		return sizeof(hdr);
	}

	if (len == 0) {
		puts("");
		return sizeof(hdr);
	}

	buff = malloc(len);
	fill_line_buff(buff, len, &hdr, "\t", gp, failed);

	if (!gp->uniq) {
		puts(buff);
		free(buff);
	} else
	if (0 <= search_past_lines(buff, gp)) {
		/* found same line in the past, do not print */
		free(buff);
	}
	else {
		/* "--uniq" is given, but no same line in the past */
		if (gp->fail_if_multi && gp->past_lines_begin->line != NULL) {
			fprintf(stderr, "*** line \"%s\" differs from past \"%s\"\n", buff, gp->past_lines_begin->line);
			*failed = -2;
			return sizeof(hdr);
		}
		append_past_line(gp, buff);
		puts(buff);
	}
	
	/* skip content */
	memset(gp->block_buff, 0, gp->block_size);
	memcpy(gp->block_buff, hdr.size, sizeof(hdr.size));
	len_content = strtoul(gp->block_buff, NULL, 8);
	if (len_content > 0) {
		int l;
		for (l = 0; l < len_content; l += gp->block_size) {
			if (1 != fread(gp->block_buff, gp->block_size, 1, gp->fh)) {
				fprintf(stderr, "*** fail in skipping the content\n");
				*failed = -1;
				return (gp->pos - hdr_begin);
			}
			gp->pos += gp->block_size;
		}

		/* skip the last half-filled block */
		seek_to_next_block(gp, failed);
		if (*failed)
			fprintf(stderr, "*** fail in seeking to the next block");
	}

	return (gp->pos - hdr_begin);
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
		gp.fh = stdin;
	else
		gp.fh = fopen(gp.pathname_tarfile, "r");

	if (!gp.fh) {
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

	fclose(gp.fh);
	if (failed) {
		fprintf(stderr, "*** parse failed\n");
		exit(-2);
	}

	exit(0);
}
