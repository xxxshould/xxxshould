#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <argp.h>
#include <inttypes.h>
#include <err.h>

#include "version.h"
#include "libutils.h"
#include "libdevs.h"
#include "utils.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 BREW " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<DISK_DEV>";

static char doc[] = "F3 Block REad and Write -- assess the media of "
	"a block device writing blocks, resetting the drive, and "
	"reading the blocks back";

static struct argp_option options[] = {
	{"debug",		'd',	NULL,		OPTION_HIDDEN,
		"Enable debugging; only needed if none --debug-* option used",
		1},
	{"debug-real-size",	'r',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Real size of the emulated drive",	0},
	{"debug-fake-size",	'f',	"SIZE_BYTE",	OPTION_HIDDEN,
		"Fake size of the emulated drive",	0},
	{"debug-wrap",		'w',	"N",		OPTION_HIDDEN,
		"Wrap parameter of the emulated drive",	0},
	{"debug-block-order",	'b',	"ORDER",	OPTION_HIDDEN,
		"Block size of the emulated drive is 2^ORDER Bytes",	0},
	{"debug-keep-file",	'k',	NULL,		OPTION_HIDDEN,
		"Don't remove file used for emulating the drive",	0},
	{"reset-type",		's',	"TYPE",		0,
		"Reset method to use during the probe",		2},
	{"start-at",		'h',	"BLOCK",	0,
		"Where test begins; the default is block zero",	0},
	{"end-at",		'e',	"BLOCK",	0,
		"Where test ends; the default is the very last block",	0},
	{"do-not-write",	'W',	NULL,		0,
		"Do not write blocks",				0},
	{"do-not-read",		'R',	NULL,		0,
		"Do not read blocks",				0},
	{ 0 }
};

struct args {
	char		*filename;

	/* Debugging options. */
	bool		debug;
	bool		keep_file;

	/* Behavior options. */
	enum reset_type	reset_type;
	bool test_write;
	bool test_read;
	/* 3 free bytes. */

	/* Geometry. */
	uint64_t	real_size_byte;
	uint64_t	fake_size_byte;
	int		wrap;
	int		block_order;

	/* What to do. */
	uint64_t	first_block;
	uint64_t	last_block;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		args->debug = true;
		break;

	case 'r':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"Real size must be greater or equal to zero");
		args->real_size_byte = ll;
		args->debug = true;
		break;

	case 'f':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"Fake size must be greater or equal to zero");
		args->fake_size_byte = ll;
		args->debug = true;
		break;

	case 'w':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0 || ll >= 64)
			argp_error(state,
				"Wrap must be in the interval [0, 63]");
		args->wrap = ll;
		args->debug = true;
		break;

	case 'b':
		ll = arg_to_ll_bytes(state, arg);
		if (ll != 0 && (ll < 9 || ll > 20))
			argp_error(state,
				"Block order must be in the interval [9, 20] or be zero");
		args->block_order = ll;
		args->debug = true;
		break;

	case 'k':
		args->keep_file = true;
		args->debug = true;
		break;

	case 's':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0 || ll >= RT_MAX)
			argp_error(state,
				"Reset type must be in the interval [0, %i]",
				RT_MAX - 1);
		args->reset_type = ll;
		break;

	case 'h':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"The first block must be greater or equal to zero");
		args->first_block = ll;
		break;

	case 'e':
		ll = arg_to_ll_bytes(state, arg);
		if (ll < 0)
			argp_error(state,
				"The last block must be greater or equal to zero");
		args->last_block = ll;
		break;

	case 'W':
		args->test_write = false;
		break;

	case 'R':
		args->test_read = false;
		break;

	case ARGP_KEY_INIT:
		args->filename = NULL;
		break;

	case ARGP_KEY_ARG:
		if (args->filename)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->filename = arg;
		break;

	case ARGP_KEY_END:
		if (!args->filename)
			argp_error(state,
				"The disk device was not specified");
		if (args->debug &&
			!dev_param_valid(args->real_size_byte,
				args->fake_size_byte, args->wrap,
				args->block_order))
			argp_error(state,
				"The debugging parameters are not valid");

		if (args->first_block > args->last_block)
			argp_error(state,
				"The first block parameter must be less or equal to the last block parameter. They are now: first_block=%"
				PRIu64 " > last_block=%" PRIu64,
				args->first_block, args->last_block);

		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

/* XXX Avoid code duplication. This function is copied from f3write.c. */
static uint64_t fill_buffer(void *buf, size_t size, uint64_t offset)
{
	uint8_t *p, *ptr_next_sector, *ptr_end;
	uint64_t rn;

	assert(size > 0);
	assert(size % SECTOR_SIZE == 0);
	assert(SECTOR_SIZE >= sizeof(offset) + sizeof(rn));
	assert((SECTOR_SIZE - sizeof(offset)) % sizeof(rn) == 0);

	p = buf;
	ptr_end = p + size;
	while (p < ptr_end) {
		rn = offset;
		memmove(p, &offset, sizeof(offset));
		ptr_next_sector = p + SECTOR_SIZE;
		p += sizeof(offset);
		for (; p < ptr_next_sector; p += sizeof(rn)) {
			rn = random_number(rn);
			memmove(p, &rn, sizeof(rn));
		}
		assert(p == ptr_next_sector);
		offset += SECTOR_SIZE;
	}

	return offset;
}

static void write_blocks(char *stamp_blk, struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t sector_offset = first_block << dev_get_block_order(dev);
	uint64_t i;

	for (i = first_block; i <= last_block; i++) {
		sector_offset =
			fill_buffer(stamp_blk, block_size, sector_offset);
		if (dev_write_block(dev, stamp_blk, i))
			warn("Failed writing block 0x%" PRIx64, i);
	}
}

/* XXX Avoid code duplication. Some code of this function is copied from
 * f3read.c.
 */
/* XXX Group the results so it is not too verbose.
 * For now, the less important reports are commented.
 */
#define TOLERANCE	2
static void validate_sector(uint64_t expected_sector_offset,
	const char *sector)
{
	uint64_t sector_offset, rn;
	const char *p, *ptr_end;
	int error_count;

	sector_offset = *((__typeof__(sector_offset) *) sector);
	rn = sector_offset;
	p = sector + sizeof(sector_offset);
	ptr_end = sector + SECTOR_SIZE;
	error_count = 0;
	for (; error_count <= TOLERANCE && p < ptr_end; p += sizeof(rn)) {
		rn = random_number(rn);
		if (rn != *((__typeof__(rn) *) p))
			error_count++;
	}

	if (sector_offset == expected_sector_offset) {
		if (error_count == 0)
			/*printf("GOOD sector 0x%" PRIx64 "\n",
				expected_sector_offset)*/;
		else if (error_count <= TOLERANCE)
			printf("Changed sector 0x%" PRIx64 "\n",
				expected_sector_offset);
		else
			printf("BAD matching sector 0x%" PRIx64 "\n",
				expected_sector_offset);
	} else if (error_count == 0) {
		printf("Overwritten sector 0x%" PRIx64
			", found 0x%" PRIx64 "\n",
			expected_sector_offset, sector_offset);
	} else if (error_count <= TOLERANCE) {
		printf("Overwritten and changed sector 0x%" PRIx64
			", found 0x%" PRIx64 "\n",
			expected_sector_offset, sector_offset);
	} else {
		/*printf("BAD sector 0x%" PRIx64 "\n", expected_sector_offset)*/;
	}
}

static void validate_block(uint64_t expected_sector_offset,
	const char *probe_blk, int block_size)
{
	const char *sector = probe_blk;
	const char *stop_sector = sector + block_size;

	assert(block_size % SECTOR_SIZE == 0);

	while (sector < stop_sector) {
		validate_sector(expected_sector_offset, sector);
		expected_sector_offset += SECTOR_SIZE;
		sector += SECTOR_SIZE;
	}
}

static void read_blocks(char *probe_blk, struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_size = dev_get_block_size(dev);
	uint64_t expected_sector_offset =
		first_block << dev_get_block_order(dev);
	uint64_t i;

	for (i = first_block; i <= last_block; i++) {
		if (!dev_read_block(dev, probe_blk, i))
			validate_block(expected_sector_offset, probe_blk,
				block_size);
		else
			warn("Failed reading block 0x%" PRIx64, i);
		expected_sector_offset += block_size;
	}
}

/* XXX Properly handle return errors. */
static void test_write_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + block_size];
	char *blk = align_mem(stack, block_order);

	printf("Writing blocks from 0x%" PRIx64 " to 0x%" PRIx64 "...",
		first_block, last_block);
	fflush(stdout);
	write_blocks(blk, dev, first_block, last_block);
	printf(" Done\n\n");
}

/* XXX Properly handle return errors. */
static void test_read_blocks(struct device *dev,
	uint64_t first_block, uint64_t last_block)
{
	const int block_order = dev_get_block_order(dev);
	const int block_size = dev_get_block_size(dev);
	char stack[align_head(block_order) + block_size];
	char *blk = align_mem(stack, block_order);

	printf("Reading blocks from 0x%" PRIx64 " to 0x%" PRIx64 "...",
		first_block, last_block);
	fflush(stdout);
	read_blocks(blk, dev, first_block, last_block);
	printf(" Done\n\n");
}

int main(int argc, char **argv)
{
	struct args args = {
		/* Defaults. */
		.debug		= false,
		.keep_file	= false,
		.reset_type	= RT_DEFAULT,
		.test_write	= true,
		.test_read	= true,
		.real_size_byte	= 1ULL << 31,
		.fake_size_byte	= 1ULL << 34,
		.wrap		= 31,
		.block_order	= 0,
		.first_block	= 0,
		.last_block	= -1ULL,
	};
	struct device *dev;
	uint64_t very_last_block;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "brew");

	dev = args.debug
		? create_block_device(args.filename, args.reset_type)
		: create_file_device(args.filename, args.real_size_byte,
			args.fake_size_byte, args.wrap, args.block_order,
			args.keep_file);
	assert(dev);

	very_last_block = dev_get_size_byte(dev) >> dev_get_block_order(dev);
	if (args.first_block > very_last_block)
		args.first_block = very_last_block;
	if (args.last_block > very_last_block)
		args.last_block = very_last_block;

	if (args.test_write)
		test_write_blocks(dev, args.first_block, args.last_block);
	if (args.test_write && args.test_read)
		assert(!dev_reset(dev));
	if (args.test_read)
		test_read_blocks(dev, args.first_block, args.last_block);

	free_device(dev);
	return 0;
}
