#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

typedef enum {
	USTAR_NONE = 0,
	USTAR_PATHNAME,
	USTAR_UNAME,
	USTAR_GNAME,
	USTAR_UID,
	USTAR_GID,
	USTAR_MAX
} header_info_t;


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
	{ "gid", USTAR_GID }
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

/* -------------------------------------------------------------- */

size_t  num_infos_to_print = 0;
int*    infos_to_print = NULL;
int     uniq = 0;
int     fail_if_multi = 0;
const char* pathname_tarfile = NULL;

static int parse_args(int argc, const char **argv)
{
	int i;

	/* allocate info_to_print[argc] could be overkill, but sufficient */
	infos_to_print = (int *)malloc(argc * sizeof(int));
	memset(infos_to_print, 0, argc * sizeof(int));

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
			uniq = 1;
		else
		if (!strcasecmp("--fail-if-multi", argv[i]))
			fail_if_multi = 1;
		else
		if (argv[i][0] != '-' && !pathname_tarfile)
			pathname_tarfile = argv[i];

		if (id == USTAR_NONE)
			continue;
			
		infos_to_print[num_infos_to_print] = id;
		num_infos_to_print ++;
	}
	return num_infos_to_print;
}

#define USTAR_BLOCKSIZE 512
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


const char *get_hdr_data_by_id(ustar_header_t* hdr, header_info_t id)
{
	switch (id) {
	case USTAR_PATHNAME: return hdr->name;
	case USTAR_UID: return hdr->uid;
	case USTAR_GID: return hdr->gid;
	case USTAR_UNAME: return hdr->uname;
	case USTAR_GNAME: return hdr->gname;
	default: return NULL;
	}
}

int get_int_from_oct_str(const char* s, size_t len)
{
	int i;
	int r = 0;

	if (s[0] == '\0')
		return -1;

	for (i = 0; i < len; i++) {
		if (s[i] == '\0')
			return r;
		r = r * 8;
		r += (s[i] - '0');
	}
	return r;
}


size_t count_required_buff(ustar_header_t* hdr, size_t len_sep, int* failed)
{
	int    int_buff; /* scratch buff for oct->dec */
	char   dec_buff[9]; /* scratch buff for dec->str */
	size_t len_buff = 0;
	int i, j;
	for (i = 0; i < num_infos_to_print; i++) {
		size_t len_tok = 0;
		const char *dat = get_hdr_data_by_id(hdr, infos_to_print[i]);

		switch (infos_to_print[i]) {
		/* raw data should be printed */
		case USTAR_PATHNAME:
			len_tok = strnlen(dat, sizeof(hdr->name));
			break;
		case USTAR_UNAME:
		case USTAR_GNAME:
			len_tok = strnlen(dat, sizeof(hdr->uname));
			break;

		/* octal data should be converted decimal */
		case USTAR_UID:
		case USTAR_GID:
			int_buff = get_int_from_oct_str(dat, sizeof(hdr->uid));
			if (int_buff < 0) {
				*failed = -1;
				return 0;
			}

			/* parse octal value */
			snprintf(dec_buff, sizeof(dec_buff), "%d", int_buff);
			len_tok = strlen(dec_buff);
			break;
		default:
			continue;
		}

		if (0 < i)
			len_buff += len_sep;
		len_buff += len_tok;
	}
	return (len_buff + 1); // increment for NULL terminator
}

void fill_line_buff(char* line_buff, size_t len_line, ustar_header_t*  hdr, const char* sep)
{
	int i;
	char   dec_buff[9]; /* scratch buff for dec->str */

	line_buff[0] = '\0';
	for (i = 0; i < num_infos_to_print; i++) {
		const char *dat = get_hdr_data_by_id(hdr, infos_to_print[i]);

		switch (infos_to_print[i]) {
		/* raw data should be printed */
		case USTAR_PATHNAME:
			strncat(line_buff, dat, sizeof(hdr->name));
			break;
		case USTAR_UNAME:
		case USTAR_GNAME:
			strncat(line_buff, dat, sizeof(hdr->uname));
			break;

		/* octal data should be converted decimal */
		case USTAR_UID:
		case USTAR_GID:
			/* parse octal value */
			snprintf(dec_buff, sizeof(dec_buff), "%d", get_int_from_oct_str(dat, sizeof(hdr->uid)));
			strncat(line_buff, dec_buff, sizeof(dec_buff));
			break;
		default:
			continue;
		}

		if (i < num_infos_to_print - 1)
			strcat(line_buff, sep);
	}
}

long seek_to_next_block(FILE* fh, long block_size)
{
	long pos, overflow;
	pos = ftell(fh);
	overflow = (pos % block_size);
	if (overflow == 0)
		return 0;
	fseek(fh, block_size - overflow, SEEK_CUR);
		return (block_size - overflow);
}

size_t num_past_lines = 0;
typedef struct past_line_t {
	const char*  line;
	struct past_line_t* next;
} past_line_t;

past_line_t  past_lines = {NULL, NULL};
past_line_t* past_lines_begin;
past_line_t* past_lines_end;

void set_past_lines_begin_end()
{
	past_lines_begin = &past_lines;
	past_lines_end   =  past_lines_begin;
}

int search_past_lines(const char* s)
{
	int i;
	past_line_t *pl;
	for (pl = past_lines_begin, i = 0 ; pl < past_lines_end; pl = pl->next, i ++) {
		if (!strcmp(s, pl->line)) {
			return i;
		}	
	}
	return -1;
}

size_t feed_single_item_tarfile(FILE* fh, int* failed)
{
	ustar_header_t  hdr;
	size_t  len_line;
	size_t  len_content;
	char*  line_buff;
	int i;

	if (feof(fh))
		return 0;
	else
	if (1 != fread(&hdr, sizeof(hdr), 1, fh))
	{
		*failed = -1;
		return 0;
	}

	len_line = count_required_buff(&hdr, strlen("\t"), failed);
	if (*failed)
		return 0;

	if (len_line == 0) {
		puts("");
		return 0;
	}

	line_buff = malloc(len_line);
	fill_line_buff(line_buff, len_line, &hdr, "\t");

	if (!uniq) {
		puts(line_buff);
		free(line_buff);
	}
	else if (0 <= search_past_lines(line_buff)) {
		if (fail_if_multi)
			exit(2);
		free(line_buff);
	}
	else {
		past_lines_end->line = line_buff;
		past_lines_end->next = malloc(sizeof(past_line_t));
		past_lines_end->next->line = NULL;
		past_lines_end->next->next = NULL;
		past_lines_end = past_lines_end->next;
		puts(line_buff);
	}
	
	/* some padding between tar header and content */
	seek_to_next_block(fh, USTAR_BLOCKSIZE);

	/* skip content */
	len_content = get_int_from_oct_str(hdr.size, sizeof(hdr.size));
	if (len_content > 0 && 0 > fseek(fh, len_content, SEEK_CUR))
		*failed = -1;

	/* some padding after content */
	seek_to_next_block(fh, USTAR_BLOCKSIZE);
	return;
}

int main(int argc, const char **argv)
{
	FILE* fh;
	int chunk_length;
	int failed = 0;

	set_past_lines_begin_end();
	parse_args(argc, argv);

	/* check nothing is inappropriate test */
	if (!num_infos_to_print)
		exit(-1);

	if (!pathname_tarfile)
		fh = stdin;
	else
		fh = fopen(pathname_tarfile, "r");

	if (!fh)
		exit(-1);

	do {
		chunk_length = feed_single_item_tarfile(fh, &failed);
	} while (!failed);

	if (failed)
		exit(-2);

	exit(0);
}
