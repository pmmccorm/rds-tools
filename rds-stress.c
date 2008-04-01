#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <inttypes.h>
#include <syscall.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <ctype.h>
#include <fcntl.h>
#include <sched.h>
#include <getopt.h>
#include "net/rds.h"

#ifdef DYNAMIC_PF_RDS
#include "pfhack.h"
#endif

/*
 *
 * TODO
 *  - checksum the data some day.
 *  - use poll to wait instead of blocking recvmsg?  doesn't seem great.
 *  - measure us/call of nonblocking recvmsg
 *  - do something about receiver congestion
 *  - notice when parent tcp socket dies
 *  - should the parent be at a higher priority?
 *  - catch ctl-c
 *  - final stats summary page
 */

struct options {
	uint32_t	req_depth;
	uint32_t	req_size;
	uint32_t	ack_size;
	uint32_t	rdma_size;
	uint32_t	send_addr;
	uint32_t	receive_addr;
	uint16_t	starting_port;
	uint16_t	nr_tasks;
	uint32_t	run_time;
	uint8_t		summary_only;
	uint8_t		rtprio;
	uint8_t		tracing;
	uint8_t		verify;
	uint8_t		show_params;
	uint8_t		show_perfdata;
	uint8_t		use_cong_monitor;
	uint8_t		rdma_use_once;
	uint8_t		rdma_use_get_mr;
	unsigned int	rdma_alignment;
	unsigned int	connect_retries;

	/* At 1024 tasks, printing warnings about
	 * setsockopt(SNDBUF) allocation is rather
	 * slow. Cut that */
	uint8_t		suppress_warnings;
} __attribute__((packed));

static struct options	opt;

struct counter {
	uint64_t	nr;
	uint64_t	sum;
	uint64_t	min;
	uint64_t	max;
};

enum {
	S_REQ_TX_BYTES = 0,
	S_REQ_RX_BYTES,
	S_ACK_TX_BYTES,
	S_ACK_RX_BYTES,
	S_RDMA_WRITE_BYTES,
	S_RDMA_READ_BYTES,
	S_SENDMSG_USECS,
	S_RTT_USECS,
	S__LAST
};

#define NR_STATS S__LAST

/*
 * Parents share a mapped array of these with their children.  Each child
 * gets one.  It's used to communicate between the child and the parent
 * simply.
 */
struct child_control {
	pid_t pid;
	int ready;
	struct timeval start;
	struct counter cur[NR_STATS];
	struct counter last[NR_STATS];
} __attribute__((aligned (256))); /* arbitrary */

struct soak_control {
	pid_t		pid;
	uint64_t	per_sec;
	uint64_t	counter;
	uint64_t	last;
	struct timeval	start;
} __attribute__((aligned (256))); /* arbitrary */

void stop_soakers(struct soak_control *soak_arr);

/*
 * Requests tend to be larger and we try to keep a certain number of them
 * in flight at a time.  Acks are sent in response to requests and tend
 * to be smaller.
 */
#define OP_REQ		1
#define OP_ACK		2

#define RDMA_OP_READ	1
#define RDMA_OP_WRITE	2
#define RDMA_OP_TOGGLE(x) (3 - (x))	/* read becomes write and vice versa */

/*
 * Every message sent with sendmsg gets a header.  This lets the receiver
 * verify that it got what was sent.
 */
struct header {
	uint32_t	seq;
	uint32_t	from_addr;
	uint32_t	to_addr;
	uint16_t	from_port;
	uint16_t	to_port;
	uint16_t	index;
	uint8_t		op;

	/* RDMA related.
	 * rdma_op must be the first field, because we
	 * use offsetof(rdma_op) in fill_hdr and check_hdr
	 */
	uint8_t		rdma_op;
	uint64_t	rdma_addr;
	uint64_t	rdma_phyaddr;
	uint64_t	rdma_pattern;
	uint64_t	rdma_key;
	uint32_t	rdma_size;

	uint8_t		data[0];
} __attribute__((packed));

#define MIN_MSG_BYTES		(sizeof(struct header))
#define BASIC_HEADER_SIZE	(size_t)(&((struct header *) 0)->rdma_op)

#define die(fmt...) do {		\
	fprintf(stderr, fmt);		\
	exit(1);			\
} while (0)

#define die_errno(fmt, args...) do {				\
	fprintf(stderr, fmt ", errno: %d (%s)\n", ##args , errno,\
		strerror(errno));				\
	exit(1);						\
} while (0)

static int	mrs_allocated = 0;

#define trace(fmt...) do {		\
	if (opt.tracing)		\
		fprintf(stderr, fmt);	\
} while (0)

#define min(a,b) (a < b ? a : b)
#define max(a,b) (a > b ? a : b)

static unsigned long	sys_page_size;

/* This macro casts a pointer to uint64_t without producing
   warnings on either 32bit or 64bit platforms. At least
   with gcc, that is.
 */
#define ptr64(p)	((unsigned long) (p))

/* zero is undefined */
static inline uint64_t minz(uint64_t a, uint64_t b)
{
	if (a == 0)
		return b;
	if (b == 0)
		return a;
	return min(a, b);
}

static unsigned long long parse_ull(char *ptr, unsigned long long max)
{
	unsigned long long val;
	char *endptr;

	val = strtoull(ptr, &endptr, 0);
	switch (*endptr) {
	case 'k': case 'K':
		val <<= 10;
		endptr++;
		break;

	case 'm': case 'M':
		val <<= 20;
		endptr++;
		break;

	case 'g': case 'G':
		val <<= 30;
		endptr++;
		break;
	}

	if (*ptr && !*endptr && val <= max)
		return val;

	die("invalid number '%s'\n", ptr);
}

static uint32_t parse_addr(char *ptr)
{
	uint32_t addr;
        struct hostent *hent;

        hent = gethostbyname(ptr);
        if (hent && 
            hent->h_addrtype == AF_INET && hent->h_length == sizeof(addr)) {
		memcpy(&addr, hent->h_addr, sizeof(addr));
		return ntohl(addr);
	}

	die("invalid host name or dotted quad '%s'\n", ptr);
}

static void usage(void)
{
	printf(
	"Required parameters, no defaults:\n"
	" -p [port]         starting port number\n"
	" -r [addr]         receive on this host or dotted quad\n"
	" -s [addr]         send to this passive dotted quad\n"
	"\n"
	"Optional parameters, with defaults:\n"
	" -a [bytes, %u]    ack message length\n"
	" -q [bytes, 1024]  request message length\n"
	" -d [depth, 1]     request pipeline depth, nr outstanding\n"
	" -t [nr, 1]        number of child tasks\n"
	" -T [seconds, 0]   runtime of test, 0 means infinite\n"
	" -D [bytes]        RDMA size (RDSv3 only)\n"
	"\n"
	"Optional behavioural flags:\n"
	" -c                measure cpu use with per-cpu soak processes\n"
	" -V                trace execution\n"
	" -z                print a summary at end of test only\n"
	"\n"
	"Example:\n"
	"  recv$ rds-stress -r recv -p 4000\n"
	"  send$ rds-stress -r send -s recv -p 4000 -q 4096 -t 2 -d 2\n"
	"\n", (int) MIN_MSG_BYTES);

	exit(2);
}

static void set_rt_priority(void)
{
	struct sched_param	param;

	memset(&param, 0, sizeof(param));
	param.sched_priority = 1;

	if (sched_setscheduler(0, SCHED_RR, &param) < 0)
		die_errno("sched_setscheduler(SCHED_RR) failed");
}

/* This hack lets children notice when their parents die.
 * We could also use kill(0), but that results in false
 * positives when the parent is a zombie (and that happens
 * if you have a script parsing the output of rds-stress,
 * and the parent dies).
 */
static void check_parent(pid_t pid)
{
	if (pid != getppid())
		die("parent %u exited\n", pid);
}

/*
 * put a pattern in the message so the remote side can verify that it's
 * what was expected.
 */
static unsigned char *	msg_pattern;

static void init_msg_pattern(struct options *opts)
{
	unsigned int max_size = max(opts->req_size, opts->ack_size);
	unsigned int i, k = 11;

	msg_pattern = malloc(max_size);

	/* k = 41 * (k + 3) is a generator of Z(256). Adding
	 * (i >> 8) makes sure the pattern is shifted by 1 in
	 * every successive 256 byte block, so that we can detect
	 * swapped blocks. */
	for (i = 0; i < max_size; i++, k = 41 * (k + 3) + (i >> 8))
		msg_pattern[i] = k;
}

static void fill_hdr(void *message, uint32_t bytes, struct header *hdr)
{
	memcpy(message, hdr, sizeof(*hdr));
	if (opt.verify)
		memcpy(message + sizeof(*hdr), msg_pattern, bytes - sizeof(*hdr));
}

/* inet_ntoa uses a static buffer, so calling it twice in
 * a single printf as we do below will produce undefined
 * results. We copy the output two two static buffers,
 * and switch between them.
 */
static char *inet_ntoa_32(uint32_t val)
{
	struct in_addr addr = { .s_addr = val };
	static char buffer[2][64];
	static unsigned int select = 0;

	select = 1 - select;
	strncpy(buffer[select], inet_ntoa(addr), 63);

	return buffer[select];
}

static int check_hdr(void *message, uint32_t bytes, const struct header *hdr)
{
	if (memcmp(message, hdr, BASIC_HEADER_SIZE)) {
		struct header *chk = (struct header *) message;

#define bleh(var, disp)					\
		disp(hdr->var),				\
		chk->var == hdr->var ? " =" : "!=",	\
		disp(chk->var)

		/* 
		 * This is printed as one GIANT printf() so that it serializes
		 * with stdout() and we don't get things stomping on each
		 * other
		 */
		printf( "An incoming message had a header which\n"
			"didn't contain the fields we expected:\n"
			"    member        expected eq             got\n"
			"       seq %15u %s %15u\n"
			" from_addr %15s %s %15s\n"
			" from_port %15u %s %15u\n"
			"   to_addr %15s %s %15s\n"
			"   to_port %15u %s %15u\n"
			"     index %15u %s %15u\n"
			"        op %15u %s %15u\n",
			bleh(seq, ntohl),
			bleh(from_addr, inet_ntoa_32),
			bleh(from_port, ntohs),
			bleh(to_addr, inet_ntoa_32),
			bleh(to_port, ntohs),
			bleh(index, ntohs),
			bleh(op, (uint8_t)));
#undef bleh

		return 1;
	}

	if (opt.verify
	 && memcmp(message + sizeof(*hdr), msg_pattern, bytes - sizeof(*hdr))) {
		unsigned char *p = message + sizeof(*hdr);
		unsigned int i, count = 0, total = bytes - sizeof(*hdr);
		int offset = -1;

		for (i = 0; i < total; ++i) {
			if (p[i] != msg_pattern[i]) {
				if (offset < 0)
					offset = i;
				count++;
			}
		}

		printf("An incoming message has a corrupted payload at offset %u; "
				"%u out of %u bytes corrupted\n",
				offset, count, total);
		return 1;
	}

	return 0;
}

void stat_inc(struct counter *ctr, uint64_t val)
{
	ctr->nr++;
	ctr->sum += val;
	ctr->min = minz(val, ctr->min);
	ctr->max = max(val, ctr->max);
}

int64_t tv_cmp(struct timeval *a, struct timeval *b)
{
	int64_t a_usecs = ((uint64_t)a->tv_sec * 1000000ULL) + a->tv_usec;
	int64_t b_usecs = ((uint64_t)b->tv_sec * 1000000ULL) + b->tv_usec;

	return a_usecs - b_usecs;
}

/* returns a - b in usecs */
uint64_t usec_sub(struct timeval *a, struct timeval *b)
{
	return ((uint64_t)(a->tv_sec - b->tv_sec) * 1000000ULL) + 
		a->tv_usec - b->tv_usec;
}

static int bound_socket(int domain, int type, int protocol,
			struct sockaddr_in *sin)
{
	int fd;
	int opt;

	fd = socket(domain, type, protocol);
	if (fd < 0)
		die_errno("socket(%d, %d, %d) failed", domain, type, protocol);

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
		die_errno("setsockopt(SO_REUSEADDR) failed");

	if (bind(fd, (struct sockaddr *)sin, sizeof(struct sockaddr_in)))
		die_errno("bind() failed");

	return fd;
}

static int rds_socket(struct options *opts, struct sockaddr_in *sin)
{
	int bytes;
	int fd;
	int val;
	socklen_t optlen;

	fd = bound_socket(PF_RDS, SOCK_SEQPACKET, 0, sin);

	bytes = opts->nr_tasks * opts->req_depth * 
		(opts->req_size + opts->ack_size) * 2;

	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)))
		die_errno("setsockopt(SNDBUF, %d) failed", bytes);
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)))
		die_errno("setsockopt(RCVBUF, %d) failed", bytes);

	optlen = sizeof(val);
	if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &optlen))
		die_errno("getsockopt(SNDBUF) failed");
	if (val / 2 < bytes && !opts->suppress_warnings)
		fprintf(stderr,
			"getsockopt(SNDBUF) returned %d, we wanted %d * 2\n",
			val, bytes);

	optlen = sizeof(val);
	if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, &optlen))
		die_errno("getsockopt(RCVBUF) failed");
	if (val / 2 < bytes && !opts->suppress_warnings)
		fprintf(stderr,
			"getsockopt(RCVBUF) returned %d, we need %d * 2\n",
			val, bytes);

	val = 1;
	if (opts->use_cong_monitor
	 && setsockopt(fd, SOL_RDS, RDS_CONG_MONITOR, &val, sizeof(val))) {
		if (errno != ENOPROTOOPT)
			die_errno("setsockopt(RDS_CONG_MONITOR) failed");
		printf("Kernel does not support congestion monitoring; disabled\n");
		opts->use_cong_monitor = 0;
	}

	fcntl(fd, F_SETFL, O_NONBLOCK);

	return fd;
}

static int check_rdma_support(struct options *opts)
{
	struct sockaddr_in sin;
	struct rds_free_mr_args args;
	int fd, okay = 0;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(opts->starting_port);
	sin.sin_addr.s_addr = htonl(opts->receive_addr);

	fd = bound_socket(AF_RDS, SOCK_SEQPACKET, 0, &sin);

	memset(&args, 0, sizeof(args));
	if (setsockopt(fd, SOL_RDS, RDS_FREE_MR, &args, sizeof(args)) >= 0) {
		okay = 1;
	} else if (errno == ENOPROTOOPT) {
		okay = 0;
	} else {
		die_errno("%s: RDS_FREE_MR failed with unexpected error",
				__FUNCTION__);
	}
	close(fd);

	return okay;
}

static uint64_t get_rdma_key(int fd, uint64_t addr, uint32_t size)
{
	uint64_t cookie = 0;
	struct rds_get_mr_args mr_args;

	mr_args.vec.addr = addr;
	mr_args.vec.bytes = size;
	mr_args.cookie_addr = ptr64(&cookie);
	mr_args.flags = RDS_RDMA_READWRITE; /* for now, always assume r/w */
	if (opt.rdma_use_once)
		mr_args.flags |= RDS_RDMA_USE_ONCE;

	if (setsockopt(fd, SOL_RDS, RDS_GET_MR, &mr_args, sizeof(mr_args)))
		die_errno("setsockopt(RDS_GET_MR) failed (%u allocated)", mrs_allocated);

	trace("RDS get_rdma_key() = %Lx\n",
				(unsigned long long) cookie);

	mrs_allocated++;
	return cookie;
}

static void free_rdma_key(int fd, uint64_t key)
{
	struct rds_free_mr_args mr_args;

	trace("RDS free_rdma_key(%Lx)\n", (unsigned long long) key);

	mr_args.cookie = key;
#if 1
	mr_args.flags = 0;
#else
	mr_args.flags = RDS_FREE_MR_ARGS_INVALIDATE;
#endif
	if (setsockopt(fd, SOL_RDS, RDS_FREE_MR, &mr_args, sizeof(mr_args)))
		die_errno("setsockopt(RDS_FREE_MR) failed");
	mrs_allocated--;
}


static void rds_fill_buffer(void *buf, size_t size, uint64_t pattern)
{
	uint64_t *pos, *end;

	pos = (uint64_t *) buf;
	end = (uint64_t *) (buf + size);
	while (pos < end)
		*pos++ = pattern;
}


static void  rds_dump_buffer(const void *buf, size_t size)
{
	const uint64_t *pos;
	unsigned int i, count;

	pos = (const uint64_t *) buf;

	count = size / sizeof(uint64_t);
	pos = (const uint64_t *) buf;

	printf("rds_dump_buffer(%p, %u)\n", buf, (int) size);
	for (i = 0; i < count; ++i) {
		if ((i % 4) == 0)
			printf("\n%08x:", i);
		printf(" %016Lx", (unsigned long long) *pos++);
	}
}

static void rds_compare_buffer(uint64_t *addr, int size, uint64_t pattern)
{
	int d, failed = 0;

	for (d = 0; d < size / sizeof(uint64_t); d++) {
		if (addr[d] == pattern)
			continue;

		failed = 1;
		trace("compare fail pattern offset %u: expected %Lx got %Lx\n",
				8 * d,
				(unsigned long long) pattern,
				(unsigned long long) addr[d]);

#if 0
		rds_dump_buffer(addr, size);
		die("compare pass\n");
#endif
	}

	if (!failed)
		trace("compare pass pattern %Lx addr %p\n",
			(unsigned long long) pattern, addr);
}

struct task {
	unsigned int		nr;
	unsigned int		pending;
	unsigned int		unacked;
	struct sockaddr_in	src_addr;	/* same for all tasks */
	struct sockaddr_in	dst_addr;
	unsigned char		congested;
	unsigned char		drain_rdmas;
	uint32_t		send_seq;
	uint32_t		recv_seq;
	uint16_t		send_index;
	uint16_t		recv_index;
	struct timeval *	send_time;
	struct header *		ack_header;

	/* RDMA related stuff */
	uint64_t **		local_buf;
	uint64_t **		rdma_buf;
	uint64_t *		rdma_req_key;
	uint8_t *		rdma_inflight;
	uint32_t		buffid;
	uint8_t			rdma_next_op;
};

static void alloc_rdma_buffers(struct task *t, struct options *opts)
{
	unsigned int i, j;
	size_t len;
	caddr_t	base;

	/* We use mmap here rather than malloc, because it is always
	 * page aligned. */
	len = 2 * opts->nr_tasks * opts->req_depth * opts->rdma_size + sys_page_size;
	base = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
	if (base == MAP_FAILED)
		die_errno("alloc_rdma_buffers: mmap failed");
	memset(base, 0x2f, len);
	base += opts->rdma_alignment;

	for (i = 0; i < opts->nr_tasks; ++i, ++t) {
		for (j = 0; j < opts->req_depth; ++j) {
			t->rdma_buf[j] = (uint64_t *) base;
			base += opts->rdma_size;

			t->local_buf[j] = (uint64_t *) base;
			base += opts->rdma_size;

			t->rdma_req_key[j] = 0;
			t->rdma_inflight[j] = 0;
		}
	}
}

static void rdma_build_req(int fd, struct header *hdr, struct task *t,
		unsigned int rdma_size, unsigned int req_depth)
{
	uint64_t *rdma_addr, *rdma_key_p;

	rdma_addr = t->rdma_buf[t->send_index];

	rdma_key_p = &t->rdma_req_key[t->send_index];
	if (opt.rdma_use_get_mr && *rdma_key_p == 0)
		*rdma_key_p = get_rdma_key(fd, ptr64(rdma_addr), rdma_size);

	/* We alternate between RDMA READ and WRITEs */
	hdr->rdma_op = t->rdma_next_op;
	t->rdma_next_op = RDMA_OP_TOGGLE(t->rdma_next_op);

	/* FIXME! rdma_size and req_depth need htonl */
	hdr->rdma_pattern = (((uint64_t) t->send_seq) << 32) | getpid();
	hdr->rdma_addr = ptr64(rdma_addr);
	hdr->rdma_phyaddr = 0;
	hdr->rdma_size = rdma_size;
	hdr->rdma_key = *rdma_key_p;

	if (RDMA_OP_READ == hdr->rdma_op) {
		if (opt.verify)
			rds_fill_buffer(rdma_addr, rdma_size, hdr->rdma_pattern);
		trace("Requesting RDMA read for pattern %Lx "
				"local addr to rdma read %p\n",
				(unsigned long long) hdr->rdma_pattern,
				rdma_addr);
	} else {
		if (opt.verify)
			rds_fill_buffer(rdma_addr, rdma_size, 0);
		trace("Requesting RDMA write for pattern %Lx "
				"local addr to rdma write %p\n",
				(unsigned long long) hdr->rdma_pattern,
				rdma_addr);
	}
}

static void rdma_validate(const struct header *in_hdr, struct options *opts)
{
	unsigned long	rdma_size;

	rdma_size = in_hdr->rdma_size;      /* ntohl? */
	if (rdma_size != opts->rdma_size)
		die("Unexpected RDMA size %lu in request\n", rdma_size);

	if (in_hdr->rdma_op != RDMA_OP_READ && in_hdr->rdma_op != RDMA_OP_WRITE)
		die("Unexpected RDMA op %u in request\n", in_hdr->rdma_op);


	trace("RDS received request to issue rdma %s len %lu rva %Lx key %Lx pattern %Lx\n",
		in_hdr->rdma_op == RDMA_OP_WRITE? "write to" : "read from",
		rdma_size,
		(unsigned long long) in_hdr->rdma_addr,
		(unsigned long long) in_hdr->rdma_key,
		(unsigned long long) in_hdr->rdma_pattern);
}

static void rdma_build_ack(struct header *hdr, const struct header *in_hdr)
{
	hdr->rdma_op = in_hdr->rdma_op;
	hdr->rdma_size = in_hdr->rdma_size;
	hdr->rdma_key = in_hdr->rdma_key;
	hdr->rdma_phyaddr = in_hdr->rdma_phyaddr; /* remote's address to rdma to / from */
	hdr->rdma_addr = in_hdr->rdma_addr; /* remote's address to rdma to / from */
	hdr->rdma_pattern = in_hdr->rdma_pattern;
}

static inline unsigned int rdma_user_token(struct task *t, unsigned int qindex)
{
	return t->nr * opt.req_depth + qindex;
}

static void rdma_mark_completed(struct task *tasks, unsigned int token, int status)
{
	struct task *t;
	unsigned int i;

	trace("RDS rdma completion for token %x\n", token);

	t = &tasks[token / opt.req_depth];
	i = token % opt.req_depth;

	if (status) {
		const char *errmsg;

		switch (status) {
		case RDS_RDMA_REMOTE_ERROR:
			errmsg = "remote error"; break;
		case RDS_RDMA_CANCELED:
			errmsg = "operation was cancelled"; break;
		case RDS_RDMA_DROPPED:
			errmsg = "operation was dropped"; break;
		case RDS_RDMA_OTHER_ERROR:
			errmsg = "other error"; break;
		default:
			errmsg = "unknown error"; break;
		}

		printf("%s:%u: RDMA op %u failed: %s\n",
				inet_ntoa(t->dst_addr.sin_addr),
				ntohs(t->dst_addr.sin_port),
				i, errmsg);
	}

	t->rdma_inflight[i] = 0;
	t->drain_rdmas = 0;
}

#define MSG_MAXIOVLEN 2

/*
 * Add a control message to the outgoing message
 */
static void rdma_put_cmsg(struct msghdr *msg, int type,
			const void *ptr, size_t size)
{
	static char ctlbuf[1024];
	struct cmsghdr *cmsg;

	msg->msg_control = ctlbuf;
	msg->msg_controllen = CMSG_SPACE(size);

	cmsg = CMSG_FIRSTHDR(msg);
	cmsg->cmsg_level = SOL_RDS;
	cmsg->cmsg_type = type;
	cmsg->cmsg_len = CMSG_LEN(size);
	memcpy(CMSG_DATA(cmsg), ptr, size);
}

/*
 * This sets up all the fields for an RDMA transfer.
 * The request is passed as a control message along with
 * the ACK packet.
 */
static void rdma_build_cmsg_xfer(struct msghdr *msg, const struct header *hdr,
		unsigned int user_token, void *local_buf)
{
	static struct rds_iovec iov;
	struct rds_rdma_args args;
	unsigned int rdma_size;

	rdma_size = hdr->rdma_size;	/* ntohl? */

	trace("RDS issuing rdma for token %x key %Lx len %u local_buf %p\n",
			user_token,
			(unsigned long long) hdr->rdma_key,
			rdma_size, local_buf);

	/* rdma args */
	memset(&args, 0, sizeof(args));

	/* Set up the iovec pointing to the RDMA buffer */
	args.local_vec_addr = (uint64_t) &iov;
	args.nr_local = 1;
	iov.addr = ptr64(local_buf);
	iov.bytes = rdma_size;

	/* The remote could either give us a physical address, or
	 * an index into a zero-based FMR. Either way, we just copy it.
	 */
	args.remote_vec.addr = hdr->rdma_phyaddr;
	args.remote_vec.bytes = rdma_size;
	args.cookie = hdr->rdma_key;

	/* read or write */
	switch (hdr->rdma_op) {
	case RDMA_OP_WRITE:
		args.flags = RDS_RDMA_READWRITE;

		if (opt.verify)
			rds_fill_buffer(local_buf, rdma_size, hdr->rdma_pattern);
		break;

	case RDMA_OP_READ:
		args.flags = 0;
		break;
	}

	/* Always fence off subsequent SENDs */
	args.flags |= RDS_RDMA_FENCE;

	args.flags |= RDS_RDMA_NOTIFY_ME;
	args.user_token = user_token;

	rdma_put_cmsg(msg, RDS_CMSG_RDMA_ARGS, &args, sizeof(args));
}

static void rdma_build_cmsg_dest(struct msghdr *msg, rds_rdma_cookie_t rdma_dest)
{
	rdma_put_cmsg(msg, RDS_CMSG_RDMA_DEST, &rdma_dest, sizeof(rdma_dest));
}

static void rdma_build_cmsg_map(struct msghdr *msg, uint64_t addr, uint32_t size)
{
	static rds_rdma_cookie_t cookie;
	struct rds_get_mr_args args;

	args.vec.addr = addr;
	args.vec.bytes = size;
	args.cookie_addr = ptr64(&cookie);
	args.flags = RDS_RDMA_READWRITE; /* for now, always assume r/w */
	if (opt.rdma_use_once)
		args.flags |= RDS_RDMA_USE_ONCE;

	rdma_put_cmsg(msg, RDS_CMSG_RDMA_MAP, &args, sizeof(args));
}

static void rdma_process_ack(int fd, struct header *hdr,
		struct child_control *ctl)
{
	trace("RDS rcvd rdma %s ACK for request key %Lx len %u local addr %Lx\n",
		  RDMA_OP_WRITE == hdr->rdma_op ? "write" : "read",
		  (unsigned long long) hdr->rdma_key,
		  hdr->rdma_size,	/* XXX ntohl? */
		  (unsigned long long) hdr->rdma_addr);

	/* Need to free the MR unless allocated with use_once */
	if (!opt.rdma_use_once)
		free_rdma_key(fd, hdr->rdma_key);

	/* if acking an rdma write request - then remote node wrote local host buffer
	 * (data in) so count this as rdma data coming in (rdma_read) - else remote node read
	 * local host buffer so count this as rdma write (data out)
	 */
	switch (hdr->rdma_op) {
	case RDMA_OP_WRITE:
		/* remote node wrote local buffer check pattern
		 * sent via immediate data in rdma buffer
		 */
		stat_inc(&ctl->cur[S_RDMA_READ_BYTES],  hdr->rdma_size);

		if (opt.verify) {
			/* This funny looking cast avoids compile warnings
			 * on 32bit platforms. */
			rds_compare_buffer((void *)(unsigned long) hdr->rdma_addr,
				hdr->rdma_size, 	/* XXX ntohl? */
				hdr->rdma_pattern);
		}
		break;

	case RDMA_OP_READ:
		stat_inc(&ctl->cur[S_RDMA_WRITE_BYTES],  hdr->rdma_size);
		break;
	}
}

static void build_header(struct task *t, struct header *hdr,
		unsigned int op, unsigned int qindex)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->op = op;
	hdr->seq = htonl(t->send_seq);
	hdr->from_addr = t->src_addr.sin_addr.s_addr;
	hdr->from_port = t->src_addr.sin_port;
	hdr->to_addr = t->dst_addr.sin_addr.s_addr;
	hdr->to_port = t->dst_addr.sin_port;
	hdr->index = htons(qindex);
}

static int send_packet(int fd, struct task *t,
		struct header *hdr, unsigned int size)
{
	unsigned char buf[size], *rdma_flight_recorder = NULL;
	struct msghdr msg;
	struct iovec iov;
	ssize_t ret;

	/* Make sure we always have the current sequence number.
	 * When we send ACK packets, the seq that gets filled in is
	 * stale. */
	hdr->seq = ntohl(t->send_seq);
	fill_hdr(buf, size, hdr);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name  = (struct sockaddr *) &t->dst_addr;
	msg.msg_namelen = sizeof(t->dst_addr);

	msg.msg_iovlen = 1;
	msg.msg_iov = &iov;
	iov.iov_base = buf;
	iov.iov_len = size;

	/* If this is a REQ packet in which we pass the MR to the
	 * peer, extract the RDMA cookie and pass it on in the control
	 * message for now. */
	if (hdr->op == OP_REQ && hdr->rdma_op != 0) {
		if (hdr->rdma_key != 0) {
			/* We used GET_MR to obtain a key */
			rdma_build_cmsg_dest(&msg, hdr->rdma_key);
			hdr->rdma_key = 0;
		} else {
			/* Use the RDMA_MAP cmsg to have sendmsg do the
			 * mapping on the fly. */
			rdma_build_cmsg_map(&msg, hdr->rdma_addr,
					hdr->rdma_size);
		}
	}

	/* If this is an ACK packet with RDMA, build the cmsg
	 * header that goes with it. */
	if (hdr->op == OP_ACK && hdr->rdma_op != 0) {
		unsigned int qindex = ntohs(hdr->index);

		if (t->rdma_inflight[qindex] != 0) {
			/* It is unlikely but (provably) possible for
			 * new requests to arrive before the RDMA notification.
			 * That's because RDMA notifications are triggered
			 * by the RDS ACK processing, which happens after new
			 * messages were queued on the socket.
			 *
			 * We return one of the more obscure error messages,
			 * which we recognize and handle in the top loop. */
			trace("Drain RDMA 0x%x\n", rdma_user_token(t, qindex));
			errno = EBADSLT;
			return -1;
		}
		rdma_build_cmsg_xfer(&msg, hdr,
				rdma_user_token(t, qindex),
				t->local_buf[qindex]);
		rdma_flight_recorder = &t->rdma_inflight[qindex];
	}

	ret = sendmsg(fd, &msg, 0);
	if (ret < 0) {
		if (errno != EAGAIN && errno != ENOBUFS)
			die_errno("sendto() failed");
		return ret;
	}
	if (ret != size)
		die("sendto() truncated - %zd", ret);

	if (rdma_flight_recorder)
		*rdma_flight_recorder = 1;
	t->send_seq++;
	return ret;
}

static int send_one(int fd, struct task *t,
		struct options *opts,
		struct child_control *ctl)
{
	struct timeval start;
	struct timeval stop;
	struct header hdr;
	int ret;

	build_header(t, &hdr, OP_REQ, t->send_index);
	if (opts->rdma_size && t->send_seq > 10)
		rdma_build_req(fd, &hdr, t,
				opts->rdma_size,
				opts->req_depth);


	gettimeofday(&start, NULL);
	ret = send_packet(fd, t, &hdr, opts->req_size);
	gettimeofday(&stop, NULL);

	if (ret < 0)
		return ret;

	t->send_time[t->send_index] = start;
	t->rdma_req_key[t->send_index] = 0; /* we consumed this key */
	stat_inc(&ctl->cur[S_REQ_TX_BYTES], ret);
	stat_inc(&ctl->cur[S_SENDMSG_USECS],
		 usec_sub(&stop, &start));

	t->send_index = (t->send_index + 1) % opts->req_depth;
	t->pending++;
	return ret;
}

static int send_ack(int fd, struct task *t, unsigned int qindex,
		struct options *opts,
		struct child_control *ctl)
{
	struct header *hdr = &t->ack_header[qindex];
	ssize_t ret;

	/* send an ack in response to the req we just got */
	ret = send_packet(fd, t, hdr, opts->ack_size);
	if (ret < 0)
		return ret;
	if (ret != opts->ack_size)
		die_errno("sendto() returned %zd", ret);

	stat_inc(&ctl->cur[S_ACK_TX_BYTES], ret);

	/* need separate rdma stats cells for send/recv */
	switch (hdr->rdma_op) {
	case RDMA_OP_WRITE:
		stat_inc(&ctl->cur[S_RDMA_WRITE_BYTES], opts->rdma_size);
		break;

	case RDMA_OP_READ:
		stat_inc(&ctl->cur[S_RDMA_READ_BYTES], opts->rdma_size);
		break;
	}

	return ret;
}

static int send_anything(int fd, struct task *t,
			struct options *opts,
			struct child_control *ctl,
			int can_send)
{
	int ret;

	while (t->unacked || t->pending < opts->req_depth) {
		if (t->unacked) {
			uint16_t qindex;

			qindex = (t->recv_index - t->unacked + opts->req_depth) % opts->req_depth;
			if (!can_send)
				goto eagain;
			ret = send_ack(fd, t, qindex, opts, ctl);
			if (ret < 0)
				return -1;
			t->unacked -= 1;
		}
		if (t->pending < opts->req_depth) {
			if (!can_send)
				goto eagain;
			ret = send_one(fd, t, opts, ctl);
			if (ret < 0)
				return -1;
		}
	}

	return 0;

eagain:
	errno = EAGAIN;
	return -1;
}

static int recv_message(int fd,
		void *buffer, size_t size,
		rds_rdma_cookie_t *cookie,
		struct sockaddr_in *sin,
		struct timeval *tstamp,
		struct task *tasks)
{
	struct cmsghdr *cmsg;
	char cmsgbuf[256];
	struct msghdr msg;
	struct iovec iov;
	ssize_t ret;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (struct sockaddr *) sin;
	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	iov.iov_base = buffer;
	iov.iov_len = size;

	ret = recvmsg(fd, &msg, MSG_DONTWAIT);
	gettimeofday(tstamp, NULL);

	if (ret < 0)
		return ret;
	if (ret && ret < sizeof(struct header))
		die("recvmsg() returned short data: %zd", ret);
	if (msg.msg_namelen < sizeof(struct sockaddr_in))
		die("socklen = %d < sizeof(sin) (%zu)\n",
		    msg.msg_namelen, sizeof(struct sockaddr_in));

	/* See if the message comes with a RDMA destination */
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		struct rds_rdma_notify notify;

		if (cmsg->cmsg_level != SOL_RDS)
			continue;
		switch (cmsg->cmsg_type) {
		case RDS_CMSG_CONG_UPDATE:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(uint64_t)))
				die("RDS_CMSG_CONG_UPDATE data too small");
			else {
				unsigned int i, port;
				uint64_t mask;

				memcpy(&mask, CMSG_DATA(cmsg), sizeof(mask));
				for (i = 0; i < opt.nr_tasks; ++i) {
					port = ntohs(tasks[i].dst_addr.sin_port);
					if (mask & RDS_CONG_MONITOR_MASK(port))
						tasks[i].congested = 0;
				}
			}
			break;
		case RDS_CMSG_RDMA_DEST:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(*cookie)))
				die("RDS_CMSG_RDMA_DEST data too small");
			memcpy(cookie, CMSG_DATA(cmsg), sizeof(*cookie));
			break;

		case RDS_CMSG_RDMA_STATUS:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(notify)))
				die("RDS_CMSG_RDMA_DEST data too small");
			memcpy(&notify, CMSG_DATA(cmsg), sizeof(notify));
			rdma_mark_completed(tasks, notify.user_token, notify.status);
			break;
		}
	}
	return ret;
}

static int recv_one(int fd, struct task *tasks,
			struct options *opts,
		struct child_control *ctl)
{
	char buf[max(opts->req_size, opts->ack_size)];
	rds_rdma_cookie_t rdma_dest = 0;
	struct sockaddr_in sin;
	struct header hdr, *in_hdr;
	struct timeval tstamp;
	struct task *t;
	uint16_t expect_index;
	int task_index;
	ssize_t ret;

	ret = recv_message(fd, buf, sizeof(buf), &rdma_dest, &sin, &tstamp, tasks);
	if (ret < 0)
		return ret;

	/* If we received only RDMA completions or cong updates,
	 * ret will be 0 */
	if (ret == 0)
		return 0;

	/* check the incoming sequence number */
	task_index = ntohs(sin.sin_port) - opts->starting_port - 1;
	if (task_index >= opts->nr_tasks)
		die("received bad task index %u\n", task_index);
	t = &tasks[task_index];

	/* make sure the incoming message's size matches is op */
	in_hdr = (void *)buf;
	switch(in_hdr->op) {
	case OP_REQ:
		stat_inc(&ctl->cur[S_REQ_RX_BYTES], ret);
		if (ret != opts->req_size)
			die("req size %zd, not %u\n", ret,
			    opts->req_size);
		expect_index = t->recv_index;
		break;
	case OP_ACK:
		stat_inc(&ctl->cur[S_ACK_RX_BYTES], ret);
		if (ret != opts->ack_size)
			die("ack size %zd, not %u\n", ret,
			    opts->ack_size);

		/* This ACK should be for the oldest outstanding REQ */
		expect_index = (t->send_index - t->pending + opts->req_depth) % opts->req_depth;
		break;
	default:
		die("unknown op %u\n", in_hdr->op);
	}

	/*
	 * Verify that the incoming header indicates that this
	 * is the next in-order message to us.  We can't predict
	 * op.
	 */
	hdr.op = in_hdr->op;
	hdr.seq = htonl(t->recv_seq);
	hdr.from_addr = sin.sin_addr.s_addr;
	hdr.from_port = sin.sin_port;
	hdr.to_addr = t->src_addr.sin_addr.s_addr;
	hdr.to_port = t->src_addr.sin_port;
	hdr.index = htons(expect_index);

	if (check_hdr(buf, ret, &hdr))
		die("header from %s:%u to id %u bogus\n",
		    inet_ntoa(sin.sin_addr), htons(sin.sin_port),
		    ntohs(t->src_addr.sin_port));

	if (hdr.op == OP_ACK) {
		stat_inc(&ctl->cur[S_RTT_USECS],
			 usec_sub(&tstamp, &t->send_time[expect_index]));
		t->pending -= 1;

		if (in_hdr->rdma_key)
			rdma_process_ack(fd, in_hdr, ctl);
	} else {
		struct header *ack_hdr;

		/* Build the ACK header right away */
		ack_hdr = &t->ack_header[t->recv_index];
		build_header(t, ack_hdr, OP_ACK, t->recv_index);

		/* The RDMA is performed at the time the ACK
		 * message is sent. We need to mirror all
		 * RDMA related header fields in our response
		 * anyway, so that's a good place for send_ack
		 * to pick them up from.
		 */
		if (rdma_dest)
			in_hdr->rdma_key = rdma_dest;
		if (in_hdr->rdma_key) {
			rdma_validate(in_hdr, opts);
			rdma_build_ack(ack_hdr, in_hdr);
		}

		t->unacked += 1;
		t->recv_index = (t->recv_index + 1) % opts->req_depth;
	}
	t->recv_seq++;

	return ret;
}

static void run_child(pid_t parent_pid, struct child_control *ctl,
		      struct options *opts, uint16_t id)
{
	struct sockaddr_in sin;
	struct pollfd pfd;
	int fd;
	uint16_t i;
	ssize_t ret;
	struct task tasks[opts->nr_tasks];
	struct timeval start;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(opts->starting_port + 1 + id);
	sin.sin_addr.s_addr = htonl(opts->receive_addr);

	memset(tasks, 0, sizeof(tasks));
	for (i = 0; i < opts->nr_tasks; i++) {
		tasks[i].nr = i;
		tasks[i].src_addr = sin;
		tasks[i].dst_addr.sin_family = AF_INET;
		tasks[i].dst_addr.sin_addr.s_addr = htonl(opts->send_addr);
		tasks[i].dst_addr.sin_port = htons(opts->starting_port + 1 + i);
		tasks[i].send_time = alloca(opts->req_depth * sizeof(struct timeval));
		tasks[i].rdma_req_key = alloca(opts->req_depth * sizeof(uint64_t));
		tasks[i].rdma_inflight = alloca(opts->req_depth * sizeof(uint8_t));
		tasks[i].rdma_buf = alloca(opts->req_depth * sizeof(uint64_t *));
		tasks[i].local_buf = alloca(opts->req_depth * sizeof(uint64_t *));
		tasks[i].ack_header = alloca(opts->req_depth * sizeof(struct header));
		tasks[i].rdma_next_op = (i & 1)? RDMA_OP_READ : RDMA_OP_WRITE;
	}

	if (opts->rdma_size)
		alloc_rdma_buffers(tasks, opts);

	fd = rds_socket(opts, &sin);

	ctl->ready = 1;

	while (ctl->start.tv_sec == 0) {
		check_parent(parent_pid);
		sleep(1);
	}

	/* sleep until we're supposed to start */
	gettimeofday(&start, NULL);
	if (tv_cmp(&start, &ctl->start) < 0)
		usleep(usec_sub(&ctl->start, &start));

	sin.sin_family = AF_INET;

	pfd.fd = fd;
	pfd.events = POLLIN | POLLOUT;
	while (1) {
		struct task *t;
		int can_send;

		check_parent(parent_pid);

		ret = poll(&pfd, 1, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_errno("poll failed");
		}

		pfd.events = POLLIN;

		if (pfd.revents & POLLIN) {
			while (recv_one(fd, tasks, opts, ctl) >= 0)
				;
		}

		/* keep the pipeline full */
		can_send = !!(pfd.revents & POLLOUT);
		for (i = 0, t = tasks; i < opts->nr_tasks; i++, t++) {
			if (opt.use_cong_monitor && t->congested)
				continue;
			if (t->drain_rdmas)
				continue;
			if (send_anything(fd, t, opts, ctl, can_send) < 0) {
				pfd.events |= POLLOUT;

				/* If the send queue is full, we will see EAGAIN.
				 * If a particular destination is congested, the
				 * kernel will return ENOBUFS. In the former case,
				 * there's no point in trying other destinations;
				 * in the latter case we certainly want to try
				 * sending to other tasks.
				 *
				 * It would be nice if we could map the congestion
				 * map into user space :-)
				 */
				if (errno == ENOBUFS)
					t->congested = 1;
				else if (errno == EBADSLT)
					t->drain_rdmas = 1;
				else
					break;
			}
		}
	}
}

static struct child_control *start_children(struct options *opts)
{
	struct child_control *ctl;
	pid_t parent = getpid();
	pid_t pid;
	size_t len;
	uint32_t i;

	len = opts->nr_tasks * sizeof(*ctl);
	ctl = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED,
		   0, 0);
	if (ctl == MAP_FAILED)
		die("mmap of %u child control structs failed", opts->nr_tasks);

	memset(ctl, 0, len);

	init_msg_pattern(opts);

	for (i = 0; i < opts->nr_tasks; i++) {
		pid = fork();
		if (pid == -1)
			die_errno("forking child nr %u failed", i);
		if (pid == 0) {
			opts->suppress_warnings = (i > 0);
			run_child(parent, ctl + i, opts, i);
			exit(0);
		}
		ctl[i].pid = pid;
	}

	for (i = 0; i < opts->nr_tasks; i++) {
		if (ctl[i].ready)
			continue;
		pid = waitpid(-1, NULL, WNOHANG);
		if (pid)
			die("child %u (pid %u) exited\n", i, pid);
		sleep(1);
		i--; /* try this child again */
	}

	return ctl;
}

static double avg(struct counter *ctr)
{
	if (ctr->nr)
		return (double)ctr->sum / (double)ctr->nr;
	else
		return 0.0;
}

static double throughput(struct counter *disp)
{
	return disp[S_REQ_TX_BYTES].sum + disp[S_REQ_RX_BYTES].sum +
		disp[S_ACK_TX_BYTES].sum + disp[S_ACK_RX_BYTES].sum;
}

static double throughput_rdma(struct counter *disp)
{
	return disp[S_RDMA_WRITE_BYTES].sum + disp[S_RDMA_READ_BYTES].sum;
}

void stat_snapshot(struct counter *disp, struct child_control *ctl,
		   uint16_t nr_tasks)
{
	struct counter tmp[NR_STATS];
	uint16_t i;
	uint16_t s;

	memset(disp, 0, sizeof(tmp));

	for (i = 0; i < nr_tasks; i++) {
		memcpy(tmp, ctl[i].cur, sizeof(tmp));

		for (s = 0; s < NR_STATS; s++) {
			disp[s].nr += tmp[s].nr - ctl[i].last[s].nr;
			disp[s].sum += tmp[s].sum - ctl[i].last[s].sum;
			disp[s].min = minz(tmp[s].min, ctl[i].last[s].min);
			disp[s].max = max(tmp[s].max, ctl[i].last[s].max);
		}

		memcpy(ctl[i].last, tmp, sizeof(tmp));
	}
}

void stat_accumulate(struct counter *accum, const struct counter *cur)
{
	uint16_t s;

	for (s = 0; s < NR_STATS; ++s, ++cur, ++accum) {
		accum->nr += cur->nr;
		accum->sum += cur->sum;
		accum->min = minz(accum->min, cur->min);
		accum->max = max(accum->max, cur->max);
	}
}

void stat_total(struct counter *disp, struct child_control *ctl,
		uint16_t nr_tasks)
{
	uint16_t i;
	uint16_t s;

	memset(disp, 0, sizeof(struct counter) * NR_STATS);

	for (i = 0; i < nr_tasks; i++) {
		for (s = 0; s < NR_STATS; s++) {
			disp[s].nr += ctl[i].cur[s].nr;
			disp[s].sum += ctl[i].cur[s].sum;
			disp[s].min = minz(disp[s].min, ctl[i].cur[s].min);
			disp[s].max = max(disp[s].max, ctl[i].cur[s].max);
		}
	}
}

static double cpu_use(struct soak_control *soak_arr)
{
	struct soak_control *soak;
	uint64_t capacity = 0;
	uint64_t soaked = 0;
	uint64_t this;

	if (soak_arr == NULL)
		return -1.0;

	for (soak = soak_arr; soak && soak->per_sec; soak++) {
		capacity += soak->per_sec;
		this = soak->counter;
		soaked += min(soak->per_sec, this - soak->last);
		soak->last = this;
	}

	return (double)(capacity - soaked) * 100 / (double)capacity;
}

static void
get_stats(int initialize)
{
#define NTIMES 8
	struct sys_stats {
		/* Where we spent out time */
		unsigned long long	times[NTIMES];
		unsigned long long	other;

		/* Interrupt count */
		unsigned long long	intr;
	};
	static struct sys_stats prev, current;
	static int disable = 0;
	char buffer[2048];
	FILE *fp;

	if (disable)
		return;
	if ((fp = fopen("/proc/stat", "r")) == NULL) {
		fprintf(stderr, "Cannot open /proc/stat (%s) - "
				"not printing cpu stats\n",
				strerror(errno));
		disable = 1;
		return;
	}

	memset(&current, 0, sizeof(current));
	while (fgets(buffer, sizeof(buffer), fp)) {
		if (!strncmp(buffer, "cpu ", 4)) {
			char	*s = buffer + 4;
			int	j;

			for (j = 0; 1; ++j) {
				unsigned long long v;

				while (*s == ' ')
					++s;
				if (!isdigit(*s))
					break;
				v = strtoull(s, &s, 10);
				if (j < NTIMES)
					current.times[j] = v;
				else
					current.other += v;
			}
		} else
		if (!strncmp(buffer, "intr ", 5)) {
			sscanf(buffer + 5, "%Lu", &current.intr);
		}
	}
	fclose(fp);

	if (initialize) {
		printf(",user:percent,system:percent,idle:percent"
		       ",irq:percent,intr:count");
	} else {
		struct sys_stats sys;
		unsigned long sum = 0;
		double scale;
		int j;

		sum = sys.other = current.other - prev.other;
		for (j = 0; j < NTIMES; ++j) {
			sys.times[j] = current.times[j] - prev.times[j];
			sum += current.times[j];
		}
		sys.intr = current.intr - prev.intr;

		scale = sum? 100.0 / sum : 0;

		/* Magic procfs offsets
		 *  0	user
		 *  1	nice
		 *  2	system
		 *  3	idle
		 *  4	iowait
		 *  5	irq
		 *  6	softirq
		 */
		printf(",%f,%f,%f,%f,%Lu",
			(sys.times[0] + sys.times[1]) * scale,
			sys.times[2] * scale,
			(sys.times[3] + sys.times[4]) * scale,
			(sys.times[5] + sys.times[6]) * scale,
			sys.intr);
	}
	prev = current;
}

static void
get_perfdata(int initialize)
{
	static struct timeval last_ts, now;
	static struct rds_info_counter *prev, *ctr;
	static unsigned char *curr = NULL;
	static socklen_t buflen = 0;
	static int sock_fd = -1;
	int i, count, item_size;

	if (sock_fd < 0) {
		sock_fd = socket(PF_RDS, SOCK_SEQPACKET, 0);
		if (sock_fd < 0)
			die_errno("Unable to create socket");
	}

	/* We should only loop once on the first call; after that the
	 * buffer requirements for RDS counters should not change. */
	while ((item_size = getsockopt(sock_fd, SOL_RDS, RDS_INFO_COUNTERS, curr, &buflen)) < 0) {
		if (errno != ENOSPC)
			die_errno("getsockopt(RDS_INFO_COUNTERS) failed");
		curr = realloc(curr, buflen);
		if (!curr)
			die_errno("Cannot allocate buffer for stats counters");
	}

	if (item_size > sizeof(*ctr))
		die("Bad counter item size in RDS_INFO_COUNTERS (got %d, max %ld)\n",
				item_size, sizeof(*ctr));
	count = buflen / item_size;

	if (prev == NULL) {
		/* First call - allocate buffer */
		prev = calloc(count, sizeof(*ctr));
		ctr = calloc(count, sizeof(*ctr));
	}

	for (i = 0; i < count; ++i)
		memcpy(ctr + i, curr + i * item_size, item_size);

	gettimeofday(&now, NULL);

	if (initialize) {
		for (i = 0; i < count; ++i) {
			printf(",%s", ctr[i].name);
			if (strstr((char *) ctr[i].name, "_bytes"))
				printf(":bytes");
			else
				printf(":count");
		}
	} else {
		double scale;

		scale = 1e6 / usec_sub(&now, &last_ts);
		for (i = 0; i < count; ++i) {
			printf(",%f",
				(ctr[i].value - prev[i].value) * scale);
		}
	}

	memcpy(prev, ctr, count * sizeof(*ctr));
	last_ts = now;

	get_stats(initialize);
}

static int reap_one_child(int wflags)
{
	pid_t pid;
	int status;

	pid = waitpid(-1, &status, wflags);
	if (pid < 0)
		die("waitpid returned %u", pid);
	if (pid == 0)
		return 0;

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) == 0)
			return 1;
		die("child pid %u exited with status %d\n",
				pid, WEXITSTATUS(status));
	}
	if (WIFSIGNALED(status)) {
		if (WTERMSIG(status) == SIGTERM)
			return 1;
		die("child pid %u exited with signal %d\n",
				pid, WTERMSIG(status));
	}
	die("child pid %u wait status %d\n", pid, status);
}

static void release_children_and_wait(struct options *opts,
				      struct child_control *ctl,
				      struct soak_control *soak_arr)
{
	struct counter disp[NR_STATS];
	struct counter summary[NR_STATS];
	struct timeval start, end, now, first_ts, last_ts;
	double cpu_total = 0;
	uint16_t i, cpu_samples = 0;
	uint16_t nr_running;

	gettimeofday(&start, NULL);
	start.tv_sec += 2;
	for (i = 0; i < opts->nr_tasks; i++)
		ctl[i].start = start;

	/* Allow for a 4 second delay: 2 seconds for the children
	 * to come up, and 2 more of burn-in time
	 */
	printf("Starting up"); fflush(stdout);
	for (i = 0; i < 4; ++i) {
		sleep(1);
		stat_snapshot(disp, ctl, opts->nr_tasks);
		cpu_use(soak_arr);
		printf(".");
		fflush(stdout);
	}
	printf("\n");

	gettimeofday(&first_ts, NULL);
	if (opts->run_time) {
		end = first_ts;
		end.tv_sec += opts->run_time;
	} else {
		timerclear(&end);
	}

	nr_running = opts->nr_tasks;
	memset(summary, 0, sizeof(summary));

	if (opts->rtprio)
		set_rt_priority();

	/* Prime the perf data counters and display the CSV header line
	 * You can filter the CSV data from the rds-stress output by
	 * grepping for the "::" marker.
	 */
	if (opt.show_perfdata) {
		printf("::");
		printf("nr_tasks:count"
		       ",req_size:bytes"
		       ",ack_size:bytes"
		       ",rdma_size:bytes");

		printf(",req_sent:count"
		       ",thruput:kB/s"
		       ",thruput_rdma:kB/s"
		       ",tx_delay:microseconds"
		       ",rtt:microseconds"
		       ",cpu:percent");
		get_perfdata(1);
		printf("\n");
	} else {
		printf("%4s %6s %10s %10s %7s %8s %5s\n",
			"tsks", "tx/s", "tx+rx K/s", "rw+rr K/s",
			"tx us/c", "rtt us", "cpu %");
	}

	last_ts = first_ts;
	while (nr_running) {
		double cpu;

		sleep(1);

		/* XXX big bug, need to mark some ctl elements dead */
		stat_snapshot(disp, ctl, nr_running);
		gettimeofday(&now, NULL);
		cpu = cpu_use(soak_arr);

		if (!opts->summary_only) {
			double scale;

			/* Every loop takes a little more than one second;
			 * and system load can actually introduce latencies.
			 * So try to measure the actual time elapsed as precise
			 * as possible, and scale all values by its inverse.
			 */
			scale = 1e6 / usec_sub(&now, &last_ts);

			if (!opt.show_perfdata) {
				printf("%4u %6"PRIu64" %10.2f %10.2f %7.2f %8.2f %5.2f\n",
					nr_running,
					disp[S_REQ_TX_BYTES].nr,
					scale * throughput(disp) / 1024.0,
					scale * throughput_rdma(disp) / 1024.0,
					scale * avg(&disp[S_SENDMSG_USECS]),
					scale * avg(&disp[S_RTT_USECS]),
					scale * cpu);
			} else {
				printf("::");
				printf("%u,%u,%u,%u,",
				       opts->nr_tasks, opts->req_size,
				       opts->ack_size, opts->rdma_size);

				printf("%Lu,%f,%f,%f,%f,%f",
					(unsigned long long) disp[S_REQ_TX_BYTES].nr,
					scale * throughput(disp) / 1024.0,
					scale * throughput_rdma(disp) / 1024.0,
					scale * avg(&disp[S_SENDMSG_USECS]),
					scale * avg(&disp[S_RTT_USECS]),
					cpu >= 0? scale * cpu : 0);

				/* Print RDS perf counters etc */
				get_perfdata(0);
				printf("\n");
			}
		}

		stat_accumulate(summary, disp);
		cpu_total += cpu;
		cpu_samples++;
		last_ts = now;

		if (timerisset(&end)) {
			if (timercmp(&now, &end, >=)) {
				for (i = 0; i < opts->nr_tasks; i++)
					kill(ctl[i].pid, SIGTERM);
				stop_soakers(soak_arr);
				break;
			}
		}

		/* see if any children have finished or died.
		 * This is a bit touchy - we should really be
		 * able to tell an exited soaker from an exiting
		 * RDS child. */
		if (reap_one_child(WNOHANG))
			nr_running--;
	}

	while (nr_running && reap_one_child(0))
		nr_running--;

	stat_total(disp, ctl, opts->nr_tasks);
	if (!opts->summary_only)
		printf("---------------------------------------------\n");
	{
		double scale;

		scale = 1e6 / usec_sub(&last_ts, &first_ts);

		printf("%4u %6lu %10.2f %10.2f %7.2f %8.2f %5.2f  (average)\n",
			opts->nr_tasks,
			(long) (scale * summary[S_REQ_TX_BYTES].nr),
			scale * throughput(summary) / 1024.0,
			scale * throughput_rdma(disp) / 1024.0,
			avg(&summary[S_SENDMSG_USECS]),
			avg(&summary[S_RTT_USECS]),
			soak_arr? scale * cpu_total : -1.0);
	}
}

static void peer_connect(int fd, const struct sockaddr_in *sin)
{
	int retries = 0;

	printf("connecting to %s:%u",
			inet_ntoa(sin->sin_addr),
			ntohs(sin->sin_port));
	fflush(stdout);

	while (connect(fd, (struct sockaddr *) sin, sizeof(*sin))) {
		if (retries == 0)
			printf(": %s", strerror(errno));

		switch (errno) {
		case ECONNREFUSED:
		case EHOSTUNREACH:
		case ENETUNREACH:
			if (retries >= opt.connect_retries)
				break;
			if (retries++ == 0)
				printf(" - retrying");
			printf(".");
			fflush(stdout);
			sleep(1);
			continue;
		}

		printf("\n");
		die("connect(%s) failed", inet_ntoa(sin->sin_addr));
	}
	printf("\n");
}

static void peer_send(int fd, const void *ptr, size_t size)
{
	ssize_t ret;

	while (size) {
		ret = write(fd, ptr, size);
		if (ret < 0)
			die_errno("Cannot send to peer");
		size -= ret;
		ptr += ret;
	}
}

static void peer_recv(int fd, void *ptr, size_t size)
{
	ssize_t ret;

	while (size) {
		ret = read(fd, ptr, size);
		if (ret < 0)
			die_errno("Cannot recv from peer");
		size -= ret;
		ptr += ret;
	}
}

static int active_parent(struct options *opts, struct soak_control *soak_arr)
{
	struct child_control *ctl;
	struct sockaddr_in sin;
	int fd;
	uint8_t ok;

	if (opts->show_params) {
		unsigned int k;

		printf("Options:\n"
		       "  %-10s %-7u\n"
		       "  %-10s %-7u\n"
		       "  %-10s %-7u\n"
		       "  %-10s %-7u\n",
		       "Tasks", opts->nr_tasks,
		       "Req size", opts->req_size,
		       "ACK size", opts->ack_size,
		       "RDMA size", opts->rdma_size);

		k = 0;
		printf("  %-10s", "RDMA opts");
		if (opts->rdma_use_once) {
			printf(" use_once"); ++k;
		}
		if (opts->rdma_use_get_mr) {
			printf(" use_get_mr"); ++k;
		}
		if (opts->rdma_alignment) {
			printf(" align=%u", opts->rdma_alignment); ++k;
		}
		if (!k)
			printf(" (defaults)");
		printf("\n");
		printf("\n");
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(opts->starting_port);
	sin.sin_addr.s_addr = htonl(opts->receive_addr);

	fd = bound_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sin);

	sin.sin_family = AF_INET;
	sin.sin_port = htons(opts->starting_port);
	sin.sin_addr.s_addr = htonl(opts->send_addr);

	peer_connect(fd, &sin);

	/* "negotiation" is overstating things a bit :-)
	 * We just tell the peer what options to use.
	 */
	peer_send(fd, opts, sizeof(struct options));

	printf("negotiated options, tasks will start in 2 seconds\n");
	ctl = start_children(opts);

	/* Tell the peer to start up. This is necessary when testing
	 * with a large number of tasks, because otherwise the peer
	 * may start sending before we have all our tasks running.
	 */
	peer_send(fd, &ok, sizeof(ok));
	peer_recv(fd, &ok, sizeof(ok));

	/* Close socket now, to prevent long TIME_WAIT delays */
	close(fd);

	release_children_and_wait(opts, ctl, soak_arr);

	return 0;
}

static int passive_parent(uint32_t addr, uint16_t port,
			  struct soak_control *soak_arr)
{
	struct options remote, *opts = &remote;
	struct child_control *ctl;
	struct sockaddr_in sin;
	socklen_t socklen;
	int lfd, fd;
	uint8_t ok;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(addr);

	lfd = bound_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sin);

	if (listen(lfd, 255))
		die_errno("listen() failed");

	socklen = sizeof(sin);

	fd = accept(lfd, (struct sockaddr *)&sin, &socklen);
	if (fd < 0)
		die_errno("accept() failed");

	/* Do not accept any further connections - we don't handle them
	 * anyway. */
	close(lfd);

	printf("accepted connection from %s:%u\n", inet_ntoa(sin.sin_addr),
		ntohs(sin.sin_port));

	peer_recv(fd, opts, sizeof(struct options));

	/*
	 * The sender gave us their send and receive addresses, we need
	 * to swap them.
	 */
	opts->send_addr = opts->receive_addr;
	opts->receive_addr = addr;
	opt = *opts;

	ctl = start_children(opts);

	/* Wait for "GO" from the initiating peer */
	peer_recv(fd, &ok, sizeof(ok));
	peer_send(fd, &ok, sizeof(ok));

	printf("negotiated options, tasks will start in 2 seconds\n");
	release_children_and_wait(opts, ctl, soak_arr);

	return 0;
}

/*
 * The soaker *constantly* spins calling getpid().  It tries to execute a
 * second's worth of calls before checking that it's parent is still alive.  It
 * uses gettimeofday() to figure out the per-second rate of the series it just
 * executed.  It always tries to work from the highest rate it ever saw.  
 */
static void run_soaker(pid_t parent_pid, struct soak_control *soak)
{
	uint64_t i;
	uint64_t per_sec;
	struct timeval start;
	struct timeval stop;
	uint64_t usecs;

	nice(20);

	soak->per_sec = 1000;

	while (1) {
		gettimeofday(&start, NULL);
		for (i = 0; i < soak->per_sec; i++) {
			syscall(SYS_getpid);
			soak->counter++;
		}
		gettimeofday(&stop, NULL);

		usecs = usec_sub(&stop, &start);
		per_sec = (double)soak->per_sec * 1000000.0 / (double)usecs;

		if (per_sec > soak->per_sec)
			soak->per_sec = per_sec;

		check_parent(parent_pid);
	}
}

struct soak_control *start_soakers(void)
{
	struct soak_control *soak_arr;
	pid_t parent = getpid();
	pid_t pid;
	size_t len;
	long nr_soak = sysconf(_SC_NPROCESSORS_ONLN);
	long i;

	/* an extra terminating entry which will be all 0s */
	len = (nr_soak + 1) * sizeof(struct soak_control);
	soak_arr = mmap(NULL, len, PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_SHARED, 0, 0);
	if (soak_arr == MAP_FAILED)
		die("mmap of %ld soak control structs failed", nr_soak);

	memset(soak_arr, 0, len);

	printf("started %ld cycle soaking processes\n", nr_soak);

	for (i = 0; i < nr_soak; i++) {
		pid = fork();
		if (pid == -1)
			die_errno("forking soaker nr %lu failed", i);
		if (pid == 0) {
			run_soaker(parent, soak_arr + i);
			exit(0);
		}
		soak_arr[i].pid = pid;
	}

	return soak_arr;
}

void stop_soakers(struct soak_control *soak_arr)
{
	unsigned int i, nr_soak = sysconf(_SC_NPROCESSORS_ONLN);

	if (!soak_arr)
		return;
	for (i = 0; i < nr_soak; ++i) {
		kill(soak_arr[i].pid, SIGTERM);
		waitpid(soak_arr[i].pid, NULL, 0);
	}
}

void check_size(uint32_t size, uint32_t unspec, uint32_t max, char *desc,
		char *opt)
{
	if (size == ~0)
		die("specify %s with %s\n", desc, opt);
	if (size < max)
		die("%s must be at least %u bytes\n", desc, max);
}

enum {
	OPT_RDMA_USE_ONCE = 0x100,
	OPT_RDMA_USE_GET_MR,
	OPT_RDMA_USE_NOTIFY,
	OPT_RDMA_ALIGNMENT,
	OPT_SHOW_PARAMS,
	OPT_CONNECT_RETRIES,
	OPT_USE_CONG_MONITOR,
	OPT_PERFDATA,
};

static struct option long_options[] = {
{ "req-bytes",		required_argument,	NULL,	'q'	},
{ "ack-bytes",		required_argument,	NULL,	'a'	},
{ "rdma-bytes",		required_argument,	NULL,	'D'	},
{ "tasks",		required_argument,	NULL,	't'	},
{ "depth",		required_argument,	NULL,	'd'	},
{ "recv-addr",		required_argument,	NULL,	'r'	},
{ "send-addr",		required_argument,	NULL,	's'	},
{ "port",		required_argument,	NULL,	'p'	},
{ "time",		required_argument,	NULL,	'T'	},
{ "report-cpu",		no_argument,		NULL,	'c'	},
{ "report-summary",	no_argument,		NULL,	'z'	},
{ "rtprio",		no_argument,		NULL,	'R'	},
{ "verify",		no_argument,		NULL,	'v'	},
{ "trace",		no_argument,		NULL,	'V'	},

{ "rdma-use-once",	required_argument,	NULL,	OPT_RDMA_USE_ONCE },
{ "rdma-use-get-mr",	required_argument,	NULL,	OPT_RDMA_USE_GET_MR },
{ "rdma-use-notify",	required_argument,	NULL,	OPT_RDMA_USE_NOTIFY },
{ "rdma-alignment",	required_argument,	NULL,	OPT_RDMA_ALIGNMENT },
{ "show-params",	no_argument,		NULL,	OPT_SHOW_PARAMS },
{ "show-perfdata",	no_argument,		NULL,	OPT_PERFDATA },
{ "connect-retries",	required_argument,	NULL,	OPT_CONNECT_RETRIES },
{ "use-cong-monitor",	required_argument,	NULL,	OPT_USE_CONG_MONITOR },

{ NULL }
};

int main(int argc, char **argv)
{
	struct options opts;
	struct soak_control *soak_arr = NULL;

#ifdef DYNAMIC_PF_RDS
	/* Discover PF_RDS/SOL_RDS once, and be done with it */
	(void) discover_pf_rds();
	(void) discover_sol_rds();
#endif

#ifdef _SC_PAGESIZE
	sys_page_size = sysconf(_SC_PAGESIZE);
#else
	sys_page_size = 4096;
#endif

	/* We really want to see output when we redirect
	 * stdout to a pipe. */
	setlinebuf(stdout);

	memset(&opts, 0xff, sizeof(opts));

	if (argc == 1)
		usage();

	opts.ack_size = MIN_MSG_BYTES;
	opts.req_size = 1024;
	opts.run_time = 0;
	opts.summary_only = 0;
	opts.tracing = 0;
	opts.verify = 0;
	opts.rdma_size = 0;
	opts.use_cong_monitor = 1;
	opts.rdma_use_once = 1;
	opts.rdma_use_get_mr = 0;
	opts.rdma_alignment = 0;
	opts.show_params = 0;
	opts.connect_retries = 0;
	opts.show_perfdata = 0;

        while(1) {
		int c, index;

		c = getopt_long(argc, argv, "+a:cD:d:hp:q:Rr:s:t:T:vVz",
				long_options, &index);
                if (c == -1)
                        break;

                switch(c) {
                        case 'a':
				opts.ack_size = parse_ull(optarg, (uint32_t)~0);
                                break;
                        case 'c':
				soak_arr = start_soakers();
                                break;
			case 'D':
				opts.rdma_size = parse_ull(optarg, (uint32_t)~0);
				break;
                        case 'd':
				opts.req_depth = parse_ull(optarg,(uint32_t)~0);
                                break;
                        case 'p':
				opts.starting_port = parse_ull(optarg,
							       (uint16_t)~0);
                                break;
                        case 'q':
				opts.req_size = parse_ull(optarg, (uint32_t)~0);
                                break;
			case 'R':
				opts.rtprio = 1;
				break;
                        case 'r':
				opts.receive_addr = parse_addr(optarg);
                                break;
                        case 's':
				opts.send_addr = parse_addr(optarg);
                                break;
                        case 't':
				opts.nr_tasks = parse_ull(optarg,
							  (uint16_t)~0);
                                break;
			case 'T':
				opts.run_time = parse_ull(optarg, (uint32_t)~0);
				break;
			case 'z':
				opts.summary_only = 1;
				break;
			case 'v':
				opts.verify = 1;
				break;
			case 'V':
				opts.tracing = 1;
				break;
			case OPT_USE_CONG_MONITOR:
				opts.use_cong_monitor = parse_ull(optarg, 1);
				break;
			case OPT_RDMA_USE_ONCE:
				opts.rdma_use_once = parse_ull(optarg, 1);
				break;
			case OPT_RDMA_USE_GET_MR:
				opts.rdma_use_get_mr = parse_ull(optarg, 1);
				break;
			case OPT_RDMA_USE_NOTIFY:
				(void) parse_ull(optarg, 1);
				break;
			case OPT_RDMA_ALIGNMENT:
				opts.rdma_alignment = parse_ull(optarg, sys_page_size);
				break;
			case OPT_SHOW_PARAMS:
				opts.show_params = 1;
				break;
			case OPT_CONNECT_RETRIES:
				opts.connect_retries = parse_ull(optarg, (uint32_t)~0);
				break;
			case OPT_PERFDATA:
				opts.show_perfdata = 1;
				break;
                        case 'h':
                        case '?':
                        default:
				usage();
				break;
                }
        }

	if (opts.starting_port == (uint16_t)~0)
		die("specify starting port with -p\n");
	if (opts.receive_addr == ~0)
		die("specify receiving addr with -r\n");

	/* the passive parent will read options off the wire */
	if (opts.send_addr == ~0)
		return passive_parent(opts.receive_addr, opts.starting_port,
				      soak_arr);

	/* the active parent verifies and sends its options */
	check_size(opts.ack_size, ~0, MIN_MSG_BYTES, "ack size", "-a");
	check_size(opts.req_size, ~0, MIN_MSG_BYTES, "req size", "-q");

	/* defaults */
	if (opts.req_depth == ~0)
		opts.req_depth = 1;
	if (opts.nr_tasks == (uint16_t)~0)
		opts.nr_tasks = 1;

	if (opts.rdma_size && !check_rdma_support(&opts))
		die("RDMA not supported by this kernel\n");

	/* We require RDMA to be multiples of the page size for now.
	 * this is just to simplify debugging, but eventually we
	 * need to support rdma sizes from 1 to 1meg byte
	 */
	if (opts.rdma_size)
		opts.rdma_size = (opts.rdma_size + 4095) & ~4095;

	opt = opts;
	return active_parent(&opts, soak_arr);
}

/*
 * This are completely stupid.  options.c should be removed.
 */
void print_usage(int durr) { }
void print_version() { }
