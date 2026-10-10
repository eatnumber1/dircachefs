/*
 * crash_states - the crash states of the cache database's disk, for
 * guest/sqlite_durability.sh (plan step 12.14): reads a dm-log-writes log,
 * writes a chosen subset of its entries to a device, and reads, cuts and
 * forges SQLite WAL files and fingerprints the database a WAL recovers to.
 *
 * Written from the formats' descriptions, not from other tools' code: the
 * dm-log-writes log as the kernel's drivers/md/dm-log-writes.c lays it out
 * (Documentation/admin-guide/device-mapper/log-writes.rst), the WAL as
 * https://www.sqlite.org/fileformat2.html#walformat describes it.
 *
 * Subcommands (each exits 0 on success; on failure it says why on stderr
 * and exits 1):
 *
 *   crash_states log-info <log>
 *       "entries=<n> sectorsize=<n>": the log's entry count and the sector
 *       size its entries count in.
 *   crash_states log-list <log>
 *       One line per entry of the log, in log order: "<index> <sector>
 *       <sectors> <flags>[ <mark>]", <flags> the names of the entry's flags
 *       joined by "|" (FLUSH, FUA, DISCARD, MARK, METADATA; NONE for none),
 *       <mark> a MARK entry's name with every byte outside printable ASCII
 *       (and the backslash) written as \xHH.
 *   crash_states log-apply <log> <device> [<index>[:<from>-<to>]...]
 *       Writes the data of the entries named to <device>, in log order
 *       whatever the order of the arguments (a later write to a sector
 *       wins, as on the disk); with :<from>-<to>, only the sectors
 *       [from, to) of the entry's data, counted from its first (a write the
 *       disk tore: it promises no order or atomicity within one request);
 *       a DISCARD, a MARK or an empty FLUSH writes
 *       nothing (a discarded range keeps its old content, one of the two it
 *       may read back as; replay-log --no-discard does the same). Then
 *       fsync(2)s <device>.
 *   crash_states copy-sparse <src> <dst>
 *       Creates the file <dst> the size of <src> (a device or a file) with
 *       <src>'s content, writing only the 64 KiB blocks that are not all
 *       zeros: the rest stays a hole, which reads as zeros and on tmpfs
 *       takes no memory.
 *   crash_states wal-info <wal>
 *       "frames=<n> commits=<n> pagesize=<n> ckptseq=<n> salt=<hex>-<hex>
 *       beyond=<n> aftergap=<n>": the frames and commits SQLite's recovery
 *       accepts (below); all zero when the header is not valid (SQLite then
 *       ignores the file). beyond: the frames past the recovered ones that
 *       carry the header's salts (written in this WAL generation, but not
 *       recovered); aftergap: those of them that lie after a frame that
 *       does not (a later frame that landed while an earlier one did not).
 *   crash_states wal-commits <wal>
 *       One line per accepted commit: "<k> <end offset> <pages>", <k> from
 *       1, <end offset> the byte just past its commit frame, <pages> the
 *       page numbers its frames write, joined by ",".
 *   crash_states wal-truncate <wal> <k> <out>
 *       <out> is <wal> cut just after its k-th accepted commit (k = 0: the
 *       header only): the WAL a power loss leaves when commits after the
 *       k-th never reached the disk.
 *   crash_states wal-skip <wal> <j> <out>
 *       <out> holds commits 1..j-1 of <wal>, then commit j+1 with its
 *       frames' checksums recomputed so that SQLite accepts it, and nothing
 *       after: a WAL in which commit j was lost and commit j+1 survived,
 *       which the chained checksums make impossible for a real power loss
 *       (the oracle's known-bad input). Prints the pages commit j wrote and
 *       commit j+1 does not (which keep their content from before commit
 *       j), and fails if there are none (the result would be a prefix).
 *   crash_states db-fingerprint <db>
 *       Opens <db> read-write (a scratch copy: SQLite recovers its -wal
 *       beside it, as dcfs's next start would), requires PRAGMA
 *       integrity_check to say "ok", checkpoints every recovered frame into
 *       <db> (PRAGMA wal_checkpoint(TRUNCATE)) and closes it: the file's
 *       bytes are then the recovered database, page for page, and its
 *       md5sum the state's fingerprint.
 *
 * WAL recovery as SQLite does it: the 32-byte header is valid if its magic
 * is 0x377f0682 or 0x377f0683 (the low bit set: checksums over big-endian
 * words, else little-endian), its version 3007000, its page size a power of
 * two from 512 to 65536 and its checksum right. A frame (a 24-byte header,
 * then one page) is accepted if its page number is not 0, its salts are the
 * header's and its checksum is right, the checksum being chained: the sums
 * run on from the previous frame's (the header's for the first), over the
 * frame header's first 8 bytes and then the page. Recovery stops at the
 * first frame not accepted and keeps the frames up to the last commit frame
 * (a nonzero "database size after commit") before it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_errno(const char *what, const char *path)
{
	fprintf(stderr, "crash_states: %s %s: %s\n", what, path,
		strerror(errno));
	return 1;
}

static int fail_msg(const char *what)
{
	fprintf(stderr, "crash_states: %s\n", what);
	return 1;
}

/* Reads all of <path> into memory; NULL (errno set) on failure. */
static unsigned char *read_file(const char *path, size_t *len)
{
	struct stat st;
	unsigned char *buf;
	size_t got = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return NULL;
	}
	buf = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
	if (buf == NULL) {
		close(fd);
		return NULL;
	}
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			free(buf);
			close(fd);
			return NULL;
		}
		got += (size_t)n;
	}
	close(fd);
	*len = got;
	return buf;
}

static int write_file(const char *path, const unsigned char *buf, size_t len)
{
	size_t done = 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0)
		return fail_errno("open", path);
	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);

		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			close(fd);
			return fail_errno("write", path);
		}
		done += (size_t)n;
	}
	if (close(fd) != 0)
		return fail_errno("close", path);
	return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;

	errno = 0;
	*out = strtoull(s, &end, 10);
	return errno == 0 && end != s && *end == '\0' ? 0 : -1;
}

/* --- dm-log-writes ---------------------------------------------------- */

/*
 * The log: sector 0 holds the super (le64 magic, le64 version, le64 entry
 * count, le32 sector size); the entries follow from the second sector, each
 * a header sector (le64 sector, le64 sector count, le64 flags, le64 data
 * length; a MARK's name, data length bytes, follows the 32 header bytes in
 * the same sector) and then, unless it is a DISCARD, its sector count of
 * data sectors. Sectors here are the log's sector size, which the entries'
 * sector numbers on the data device count in too.
 */
#define LW_MAGIC 0x6a736677736872ULL
#define LW_VERSION 1
#define LW_FLUSH (1u << 0)
#define LW_FUA (1u << 1)
#define LW_DISCARD (1u << 2)
#define LW_MARK (1u << 3)
#define LW_METADATA (1u << 4)

struct lw_entry {
	uint64_t sector;
	uint64_t nr_sectors;
	uint64_t flags;
	uint64_t data_len;
	uint64_t offset; /* of the header sector in the log */
};

struct lw_log {
	int fd;
	uint64_t sectorsize;
	uint64_t nr_entries;
	struct lw_entry *entries;
};

static uint64_t le64(const unsigned char *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, (char *)buf + done, len - done,
				  (off_t)(off + done));

		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pwrite(fd, (const char *)buf + done, len - done,
				   (off_t)(off + done));

		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

/* Opens <path> and indexes every entry's header. */
static int lw_open(const char *path, struct lw_log *log)
{
	unsigned char super[28];
	unsigned char head[32];
	uint64_t off;

	memset(log, 0, sizeof(*log));
	log->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (log->fd < 0)
		return fail_errno("open", path);
	if (pread_full(log->fd, super, sizeof(super), 0) != 0)
		return fail_errno("read the super of", path);
	if (le64(super) != LW_MAGIC || le64(super + 8) != LW_VERSION) {
		fprintf(stderr, "crash_states: %s: not a dm-log-writes log "
			"(magic %#llx, version %llu)\n", path,
			(unsigned long long)le64(super),
			(unsigned long long)le64(super + 8));
		return 1;
	}
	log->nr_entries = le64(super + 16);
	log->sectorsize = le32(super + 24);
	if (log->sectorsize < 512 || (log->sectorsize & (log->sectorsize - 1)))
		return fail_msg("the log's sector size is not a power of two "
				"of at least 512");
	log->entries = calloc(log->nr_entries ? log->nr_entries : 1,
			      sizeof(*log->entries));
	if (log->entries == NULL)
		return fail_errno("allocate the entries of", path);
	off = log->sectorsize;
	for (uint64_t i = 0; i < log->nr_entries; i++) {
		struct lw_entry *e = &log->entries[i];

		if (pread_full(log->fd, head, sizeof(head), off) != 0)
			return fail_errno("read an entry of", path);
		e->sector = le64(head);
		e->nr_sectors = le64(head + 8);
		e->flags = le64(head + 16);
		e->data_len = le64(head + 24);
		e->offset = off;
		if (e->flags == 0 && e->nr_sectors == 0) {
			fprintf(stderr, "crash_states: %s: entry %llu of %llu "
				"is all zeros\n", path, (unsigned long long)i,
				(unsigned long long)log->nr_entries);
			return 1;
		}
		if (e->data_len > log->sectorsize - sizeof(head))
			return fail_msg("an entry's data does not fit its "
					"header sector");
		off += log->sectorsize;
		if (!(e->flags & LW_DISCARD))
			off += e->nr_sectors * log->sectorsize;
	}
	return 0;
}

static void lw_flags(uint64_t flags, char *buf, size_t len)
{
	static const struct {
		uint64_t bit;
		const char *name;
	} names[] = {
		{LW_FLUSH, "FLUSH"}, {LW_FUA, "FUA"}, {LW_DISCARD, "DISCARD"},
		{LW_MARK, "MARK"}, {LW_METADATA, "METADATA"},
	};
	size_t at = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(flags & names[i].bit))
			continue;
		at += (size_t)snprintf(buf + at, len - at, "%s%s",
				       at ? "|" : "", names[i].name);
		flags &= ~names[i].bit;
	}
	if (flags)
		at += (size_t)snprintf(buf + at, len - at, "%s%#llx",
				       at ? "|" : "", (unsigned long long)flags);
	if (at == 0)
		snprintf(buf, len, "NONE");
}

static int cmd_log_info(const char *path)
{
	struct lw_log log;

	if (lw_open(path, &log) != 0)
		return 1;
	printf("entries=%llu sectorsize=%llu\n",
	       (unsigned long long)log.nr_entries,
	       (unsigned long long)log.sectorsize);
	return 0;
}

static int cmd_log_list(const char *path)
{
	struct lw_log log;
	unsigned char *sec;
	char flags[96];

	if (lw_open(path, &log) != 0)
		return 1;
	sec = malloc(log.sectorsize);
	if (sec == NULL)
		return fail_errno("allocate a sector for", path);
	for (uint64_t i = 0; i < log.nr_entries; i++) {
		const struct lw_entry *e = &log.entries[i];

		lw_flags(e->flags, flags, sizeof(flags));
		printf("%llu %llu %llu %s", (unsigned long long)i,
		       (unsigned long long)e->sector,
		       (unsigned long long)e->nr_sectors, flags);
		if ((e->flags & LW_MARK) && e->data_len > 0) {
			if (pread_full(log.fd, sec, log.sectorsize,
				       e->offset) != 0)
				return fail_errno("read a mark of", path);
			putchar(' ');
			for (uint64_t b = 0; b < e->data_len; b++) {
				unsigned char c = sec[32 + b];

				if (c == '\0')
					break;
				if (c > ' ' && c < 0x7f && c != '\\')
					putchar(c);
				else
					printf("\\x%02x", c);
			}
		}
		putchar('\n');
	}
	free(sec);
	return ferror(stdout) ? fail_msg("writing stdout failed") : 0;
}

/* One entry of log-apply's list: the entry, and the sectors of its data to
 * write, [from, to) counted from its first. */
struct apply_item {
	uint64_t idx;
	uint64_t from;
	uint64_t to;
};

static int cmp_item(const void *a, const void *b)
{
	const struct apply_item *x = a, *y = b;

	return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* Parses "<index>" or "<index>:<from>-<to>" against <log>. */
static int parse_item(const struct lw_log *log, const char *arg,
		      struct apply_item *it)
{
	char idx_s[32], from_s[32], to_s[32];
	const char *colon = strchr(arg, ':');
	const char *dash = colon ? strchr(colon, '-') : NULL;

	if (colon == NULL) {
		if (parse_u64(arg, &it->idx) != 0 || it->idx >= log->nr_entries)
			return -1;
		it->from = 0;
		it->to = log->entries[it->idx].nr_sectors;
		return 0;
	}
	if (dash == NULL || (size_t)(colon - arg) >= sizeof(idx_s) ||
	    (size_t)(dash - colon - 1) >= sizeof(from_s) ||
	    strlen(dash + 1) >= sizeof(to_s))
		return -1;
	memcpy(idx_s, arg, (size_t)(colon - arg));
	idx_s[colon - arg] = '\0';
	memcpy(from_s, colon + 1, (size_t)(dash - colon - 1));
	from_s[dash - colon - 1] = '\0';
	strcpy(to_s, dash + 1);
	if (parse_u64(idx_s, &it->idx) != 0 || parse_u64(from_s, &it->from) != 0 ||
	    parse_u64(to_s, &it->to) != 0 || it->idx >= log->nr_entries ||
	    it->from >= it->to || it->to > log->entries[it->idx].nr_sectors)
		return -1;
	return 0;
}

static int cmd_log_apply(const char *path, const char *dev, int n,
			 char **args)
{
	struct lw_log log;
	struct apply_item *items;
	unsigned char *buf = NULL;
	size_t buf_len = 0;
	int out;

	if (lw_open(path, &log) != 0)
		return 1;
	items = calloc(n > 0 ? (size_t)n : 1, sizeof(*items));
	if (items == NULL)
		return fail_errno("allocate the entries for", path);
	for (int i = 0; i < n; i++) {
		if (parse_item(&log, args[i], &items[i]) != 0) {
			fprintf(stderr, "crash_states: no entry or sectors %s in "
				"%s (%llu entries)\n", args[i], path,
				(unsigned long long)log.nr_entries);
			return 1;
		}
	}
	/* Log order: a later write to a sector must land last. */
	qsort(items, (size_t)n, sizeof(*items), cmp_item);
	out = open(dev, O_WRONLY | O_CLOEXEC);
	if (out < 0)
		return fail_errno("open", dev);
	for (int i = 0; i < n; i++) {
		const struct lw_entry *e = &log.entries[items[i].idx];
		size_t len = (size_t)((items[i].to - items[i].from) *
				      log.sectorsize);

		if ((e->flags & (LW_MARK | LW_DISCARD)) || len == 0)
			continue;
		if (len > buf_len) {
			free(buf);
			buf = malloc(len);
			if (buf == NULL)
				return fail_errno("allocate a write of", path);
			buf_len = len;
		}
		if (pread_full(log.fd, buf, len,
			       e->offset +
				       (1 + items[i].from) * log.sectorsize) != 0)
			return fail_errno("read an entry's data from", path);
		if (pwrite_full(out, buf, len,
				(e->sector + items[i].from) * log.sectorsize) != 0)
			return fail_errno("write", dev);
	}
	if (fsync(out) != 0)
		return fail_errno("fsync", dev);
	if (close(out) != 0)
		return fail_errno("close", dev);
	free(buf);
	free(items);
	return 0;
}

static int cmd_copy_sparse(const char *src, const char *dst)
{
	enum { kBlock = 64 * 1024 };
	static unsigned char buf[kBlock];
	static const unsigned char zeros[kBlock];
	uint64_t size, off;
	int in, out;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return fail_errno("open", src);
	off = (uint64_t)lseek(in, 0, SEEK_END);
	if (off == (uint64_t)-1)
		return fail_errno("find the size of", src);
	size = off;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0)
		return fail_errno("open", dst);
	if (ftruncate(out, (off_t)size) != 0)
		return fail_errno("size", dst);
	for (off = 0; off < size; off += kBlock) {
		size_t len = size - off < kBlock ? (size_t)(size - off) : kBlock;

		if (pread_full(in, buf, len, off) != 0)
			return fail_errno("read", src);
		if (memcmp(buf, zeros, len) == 0)
			continue;
		if (pwrite_full(out, buf, len, off) != 0)
			return fail_errno("write", dst);
	}
	if (close(out) != 0)
		return fail_errno("close", dst);
	close(in);
	return 0;
}

/* --- the WAL ---------------------------------------------------------- */

#define WAL_HEADER 32
#define WAL_FRAME_HEADER 24

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/* Runs SQLite's WAL checksum over <len> bytes (a multiple of 8) from the
 * sums in s[0], s[1]. */
static void wal_sum(int big_endian, const unsigned char *p, size_t len,
		    uint32_t s[2])
{
	for (size_t i = 0; i + 8 <= len; i += 8) {
		uint32_t x0 = big_endian ? be32(p + i) : le32(p + i);
		uint32_t x1 = big_endian ? be32(p + i + 4) : le32(p + i + 4);

		s[0] += x0 + s[1];
		s[1] += x1 + s[0];
	}
}

struct wal_commit {
	size_t first_frame; /* the commit's first frame, from 0 */
	size_t end_frame;   /* one past its commit frame */
};

struct wal {
	unsigned char *buf;
	size_t len;
	int valid;
	int big_endian;
	uint32_t pagesize;
	uint32_t ckptseq;
	uint32_t salt[2];
	size_t frames; /* accepted, up to the last commit frame */
	size_t beyond; /* frames after those with the header's salts */
	size_t aftergap; /* ... of them after one without */
	size_t ncommits;
	struct wal_commit *commits;
};

static size_t frame_size(const struct wal *w)
{
	return WAL_FRAME_HEADER + w->pagesize;
}

static const unsigned char *frame_at(const struct wal *w, size_t i)
{
	return w->buf + WAL_HEADER + i * frame_size(w);
}

/* Reads <path> and finds the frames and commits SQLite would recover. */
static int wal_read(const char *path, struct wal *w)
{
	uint32_t s[2] = {0, 0};
	uint32_t magic;
	size_t cap = 16;
	size_t first = 0;

	memset(w, 0, sizeof(*w));
	w->buf = read_file(path, &w->len);
	if (w->buf == NULL)
		return fail_errno("read", path);
	w->commits = malloc(cap * sizeof(*w->commits));
	if (w->commits == NULL)
		return fail_errno("allocate the commits of", path);
	if (w->len < WAL_HEADER)
		return 0;
	magic = be32(w->buf);
	if (magic != 0x377f0682 && magic != 0x377f0683)
		return 0;
	w->big_endian = magic & 1;
	w->pagesize = be32(w->buf + 8);
	if (be32(w->buf + 4) != 3007000 || w->pagesize < 512 ||
	    w->pagesize > 65536 || (w->pagesize & (w->pagesize - 1)))
		return 0;
	wal_sum(w->big_endian, w->buf, 24, s);
	if (s[0] != be32(w->buf + 24) || s[1] != be32(w->buf + 28))
		return 0;
	w->valid = 1;
	w->ckptseq = be32(w->buf + 12);
	w->salt[0] = be32(w->buf + 16);
	w->salt[1] = be32(w->buf + 20);
	for (size_t i = 0;
	     WAL_HEADER + (i + 1) * frame_size(w) <= w->len; i++) {
		const unsigned char *f = frame_at(w, i);

		if (be32(f) == 0 || be32(f + 8) != w->salt[0] ||
		    be32(f + 12) != w->salt[1])
			break;
		wal_sum(w->big_endian, f, 8, s);
		wal_sum(w->big_endian, f + WAL_FRAME_HEADER, w->pagesize, s);
		if (s[0] != be32(f + 16) || s[1] != be32(f + 20))
			break;
		if (be32(f + 4) != 0) {
			if (w->ncommits == cap) {
				struct wal_commit *grown;

				cap *= 2;
				grown = realloc(w->commits,
						cap * sizeof(*w->commits));
				if (grown == NULL)
					return fail_errno("allocate the "
							  "commits of", path);
				w->commits = grown;
			}
			w->commits[w->ncommits].first_frame = first;
			w->commits[w->ncommits].end_frame = i + 1;
			w->ncommits++;
			w->frames = i + 1;
			first = i + 1;
		}
	}
	/* What lies past the recovered frames: this generation's frames that
	 * recovery did not reach. */
	for (size_t i = w->frames, gap = 0;
	     WAL_HEADER + (i + 1) * frame_size(w) <= w->len; i++) {
		const unsigned char *f = frame_at(w, i);

		if (be32(f) != 0 && be32(f + 8) == w->salt[0] &&
		    be32(f + 12) == w->salt[1]) {
			w->beyond++;
			if (gap)
				w->aftergap++;
		} else {
			gap = 1;
		}
	}
	return 0;
}

static int cmd_wal_info(const char *path)
{
	struct wal w;

	if (wal_read(path, &w) != 0)
		return 1;
	printf("frames=%zu commits=%zu pagesize=%u ckptseq=%u "
	       "salt=%08x-%08x beyond=%zu aftergap=%zu\n", w.frames,
	       w.ncommits, w.pagesize, w.ckptseq, w.salt[0], w.salt[1],
	       w.beyond, w.aftergap);
	return 0;
}

static int cmd_wal_commits(const char *path)
{
	struct wal w;

	if (wal_read(path, &w) != 0)
		return 1;
	for (size_t k = 0; k < w.ncommits; k++) {
		const struct wal_commit *c = &w.commits[k];

		printf("%zu %zu ", k + 1,
		       WAL_HEADER + c->end_frame * frame_size(&w));
		for (size_t i = c->first_frame; i < c->end_frame; i++)
			printf("%s%u", i > c->first_frame ? "," : "",
			       be32(frame_at(&w, i)));
		putchar('\n');
	}
	return 0;
}

static int cmd_wal_truncate(const char *path, const char *k_str,
			    const char *out)
{
	struct wal w;
	uint64_t k;

	if (wal_read(path, &w) != 0)
		return 1;
	if (!w.valid)
		return fail_msg("the WAL's header is not valid");
	if (parse_u64(k_str, &k) != 0 || k > w.ncommits) {
		fprintf(stderr, "crash_states: %s has %zu commits, not %s\n",
			path, w.ncommits, k_str);
		return 1;
	}
	return write_file(out, w.buf,
			  WAL_HEADER + (k == 0 ? 0 : w.commits[k - 1].end_frame) *
					       frame_size(&w));
}

static int page_in(const struct wal *w, const struct wal_commit *c,
		   uint32_t pgno)
{
	for (size_t i = c->first_frame; i < c->end_frame; i++) {
		if (be32(frame_at(w, i)) == pgno)
			return 1;
	}
	return 0;
}

static int cmd_wal_skip(const char *path, const char *j_str, const char *out)
{
	struct wal w;
	uint64_t j;
	const struct wal_commit *lost, *kept;
	unsigned char *forged;
	size_t at;
	uint32_t s[2] = {0, 0};
	int lost_pages = 0;

	if (wal_read(path, &w) != 0)
		return 1;
	if (!w.valid)
		return fail_msg("the WAL's header is not valid");
	if (parse_u64(j_str, &j) != 0 || j < 1 || j + 1 > w.ncommits) {
		fprintf(stderr, "crash_states: %s has %zu commits: no commit "
			"%s with one after it\n", path, w.ncommits, j_str);
		return 1;
	}
	lost = &w.commits[j - 1];
	kept = &w.commits[j];
	printf("lost pages:");
	for (size_t i = lost->first_frame; i < lost->end_frame; i++) {
		uint32_t pgno = be32(frame_at(&w, i));

		if (!page_in(&w, kept, pgno)) {
			printf(" %u", pgno);
			lost_pages++;
		}
	}
	putchar('\n');
	if (lost_pages == 0)
		return fail_msg("commit j+1 rewrites every page commit j "
				"wrote: skipping j leaves a prefix");
	forged = malloc(WAL_HEADER + (lost->first_frame + kept->end_frame -
				      kept->first_frame) * frame_size(&w));
	if (forged == NULL)
		return fail_errno("allocate the forged copy of", path);
	/* The header, and commits 1..j-1 as they are. */
	at = WAL_HEADER + lost->first_frame * frame_size(&w);
	memcpy(forged, w.buf, at);
	/* The chain's sums at the end of commit j-1. */
	s[0] = be32(w.buf + 24);
	s[1] = be32(w.buf + 28);
	if (lost->first_frame > 0) {
		const unsigned char *last = frame_at(&w, lost->first_frame - 1);

		s[0] = be32(last + 16);
		s[1] = be32(last + 20);
	}
	/* Commit j+1, its sums run on from commit j-1's. */
	for (size_t i = kept->first_frame; i < kept->end_frame; i++) {
		unsigned char *f = forged + at;

		memcpy(f, frame_at(&w, i), frame_size(&w));
		wal_sum(w.big_endian, f, 8, s);
		wal_sum(w.big_endian, f + WAL_FRAME_HEADER, w.pagesize, s);
		put_be32(f + 16, s[0]);
		put_be32(f + 20, s[1]);
		at += frame_size(&w);
	}
	return write_file(out, forged, at);
}

/* --- the database ----------------------------------------------------- */

static int cmd_db_fingerprint(const char *path)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc, ok = 0, rows = 0;

	rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "crash_states: open %s: %s\n", path,
			db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
		sqlite3_close(db);
		return 1;
	}
	rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, NULL);
	while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(stmt, 0);

		rows++;
		ok = rows == 1 && text && strcmp((const char *)text, "ok") == 0;
		if (!ok)
			fprintf(stderr, "crash_states: integrity_check of %s: "
				"%s\n", path, text ? (const char *)text : "NULL");
		rc = SQLITE_OK;
	}
	if (rc != SQLITE_DONE || !ok) {
		if (rc != SQLITE_DONE)
			fprintf(stderr, "crash_states: integrity_check of %s: "
				"%s\n", path, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return 1;
	}
	sqlite3_finalize(stmt);
	rc = sqlite3_prepare_v2(db, "PRAGMA wal_checkpoint(TRUNCATE)", -1,
				&stmt, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW || sqlite3_column_int(stmt, 0) != 0) {
		fprintf(stderr, "crash_states: checkpoint of %s: %s\n", path,
			rc == SQLITE_ROW ? "busy" : sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return 1;
	}
	sqlite3_finalize(stmt);
	if (sqlite3_close(db) != SQLITE_OK)
		return fail_msg("closing the database failed");
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 3 && strcmp(argv[1], "log-info") == 0)
		return cmd_log_info(argv[2]);
	if (argc == 3 && strcmp(argv[1], "log-list") == 0)
		return cmd_log_list(argv[2]);
	if (argc >= 4 && strcmp(argv[1], "log-apply") == 0)
		return cmd_log_apply(argv[2], argv[3], argc - 4, argv + 4);
	if (argc == 4 && strcmp(argv[1], "copy-sparse") == 0)
		return cmd_copy_sparse(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "wal-info") == 0)
		return cmd_wal_info(argv[2]);
	if (argc == 3 && strcmp(argv[1], "wal-commits") == 0)
		return cmd_wal_commits(argv[2]);
	if (argc == 5 && strcmp(argv[1], "wal-truncate") == 0)
		return cmd_wal_truncate(argv[2], argv[3], argv[4]);
	if (argc == 5 && strcmp(argv[1], "wal-skip") == 0)
		return cmd_wal_skip(argv[2], argv[3], argv[4]);
	if (argc == 3 && strcmp(argv[1], "db-fingerprint") == 0)
		return cmd_db_fingerprint(argv[2]);
	fprintf(stderr,
		"usage: crash_states log-info <log>\n"
		"       crash_states log-list <log>\n"
		"       crash_states log-apply <log> <device> "
		"[<index>[:<from>-<to>]...]\n"
		"       crash_states copy-sparse <src> <dst>\n"
		"       crash_states wal-info <wal>\n"
		"       crash_states wal-commits <wal>\n"
		"       crash_states wal-truncate <wal> <k> <out>\n"
		"       crash_states wal-skip <wal> <j> <out>\n"
		"       crash_states db-fingerprint <db>\n");
	return 2;
}
