#include <sys/types.h>
#include <sys/stat.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

enum _fbv_btn_t {
	FBV_BTN_A,
	FBV_BTN_B,
	FBV_BTN_C,
	FBV_BTN_D,
	FBV_BTNS
};
#define FBV_BTN_LONGPRESS 1000000LL/*us*/
#define FBV_PEDAL_THRESH 2

#define FBV_INP_BUF_SIZE 4
#define FBV_OUT_BUF_SIZE FBV_INP_BUF_SIZE
#define FBV_BUF_SIZE (FBV_INP_BUF_SIZE < FBV_OUT_BUF_SIZE ? FBV_OUT_BUF_SIZE : FBV_INP_BUF_SIZE)

#define POD_INP_BUF_SIZE 4
#define POD_OUT_BUF_SIZE POD_INP_BUF_SIZE
#define POD_BUF_SIZE (POD_INP_BUF_SIZE < POD_OUT_BUF_SIZE ? POD_OUT_BUF_SIZE : POD_INP_BUF_SIZE)


static unsigned
	_daemon = 0,
	ctl_running = 0;

pthread_mutex_t
	mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t
	cond_ctl = PTHREAD_COND_INITIALIZER;


#define error(_fmt, ...) do { \
	if (_daemon) \
		syslog(LOG_ERR, _fmt, ##__VA_ARGS__); \
	else \
		printf(_fmt, ##__VA_ARGS__); \
} while (0)

#define info(_fmt, ...) do { \
	if (_daemon) \
		syslog(LOG_NOTICE, _fmt, ##__VA_ARGS__); \
	else \
		printf(_fmt, ##__VA_ARGS__); \
} while (0)

#define debug(_fmt, ...) do { \
	if (!_daemon) \
		printf("%s(%i): " _fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

#define debug_msg(_str, _msg) do { \
	unsigned i; \
	if (_str) \
		debug("%s:", _str); \
	for (i = 0; i < *(_msg)->len; i++) \
		printf(" 0x%02x", (_msg)->buf[i]); \
	puts(""); \
} while (0)


typedef signed long long tic_t;

static inline void tic_get(tic_t *const tic)
{
	struct timeval now;
	gettimeofday(&now, 0);
	*tic = (tic_t)now.tv_sec * (tic_t)1000000LL + (tic_t)now.tv_usec;
}

#define thread_context_type(_t) \
	struct _thread_context_##_t

#define thread_context_define(_t, ...) \
	thread_context_type(_t) { \
		unsigned *running; \
		pthread_mutex_t *mutex; \
		pthread_cond_t *cond_rst, *cond_ctl; \
		__VA_ARGS__; }

#define thread_context_initializer(_running, _mutex, _cond_rst, _cond_ctl, ...) { \
	.running = _running, .mutex = _mutex, .cond_rst = _cond_rst, .cond_ctl = _cond_ctl, __VA_ARGS__ }

typedef struct _midi_message_t {
	tic_t *tic;
	unsigned char *buf;
	size_t *len;
	tic_t *const _tic;
	unsigned char *const _buf;
	const size_t _size;
	size_t *const _len;
} midi_message_t;

#define midi_message_initializer(__tic, __buf, __size, __len) { \
	.tic = __tic, .buf = __buf, .len = __len, \
	._tic = __tic, ._buf = __buf, ._size = __size, ._len = __len }

typedef thread_context_define(message_t,
	pthread_cond_t *cond_dev;
	midi_message_t *msg;
	int *fid) thread_context_message_t;

#define thread_context_message_initializer(_running, _mutex, _cond_rst, _cond_ctl, _cond_dev, _msg, _fid) \
	thread_context_initializer(_running, _mutex, _cond_rst, _cond_ctl, \
		.cond_dev = _cond_dev, .msg = _msg, .fid = _fid)

enum _ctl_tic_t {
	TIC_BTN_A,
	TIC_BTN_B,
	TIC_BTN_C,
	TIC_BTN_D,
	TICS
};

typedef struct _controller_state_t {
	unsigned char bank, btn;
	unsigned char vol, expr;
	tic_t tic[TICS];
} controller_state_t;

#define controller_state_initializer() { \
	.bank = 0, .btn = FBV_BTNS, \
	.tic = { \
			[TIC_BTN_A] = 0LL, \
			[TIC_BTN_B] = 0LL, \
			[TIC_BTN_C] = 0LL, \
			[TIC_BTN_D] = 0LL, \
		}, \
	}
typedef thread_context_define(control_t,
	pthread_cond_t *cond_fbv_inp, *cond_fbv_out, *cond_pod_inp, *cond_pod_out;
	midi_message_t *msg_fbv2ctl, *msg_ctl2fbv, *msg_pod2ctl, *msg_ctl2pod;
	controller_state_t *state) thread_context_control_t;

#define thread_context_control_initializer(_running, _mutex, _cond_rst, _cond_ctl, _cond_fbv_inp, _cond_fbv_out, _cond_pod_inp, _cond_pod_out, _fbv2ctl, _ctl2fbv, _pod2ctl, _ctl2pod, _state) \
	thread_context_initializer(_running, _mutex, _cond_rst, _cond_ctl, \
		.cond_fbv_inp = _cond_fbv_inp, .cond_fbv_out = _cond_fbv_out, .cond_pod_inp = _cond_pod_inp, .cond_pod_out = _cond_pod_out, \
		.msg_fbv2ctl = _fbv2ctl, .msg_ctl2fbv = _ctl2fbv, .msg_pod2ctl = _pod2ctl, .msg_ctl2pod = _ctl2pod, \
		.state = _state)

#define swap_var(_i1, _i2) do { \
	(_i1) = (_i1) ^ (_i2); \
	(_i2) = (_i1) ^ (_i2); \
	(_i1) = (_i1) ^ (_i2); \
} while (0)

#define swap_ptr(_p1, _p2) do { \
	(_p1) = (typeof(_p1))((intptr_t)(_p1) ^ (intptr_t)(_p2)); \
	(_p2) = (typeof(_p2))((intptr_t)(_p1) ^ (intptr_t)(_p2)); \
	(_p1) = (typeof(_p1))((intptr_t)(_p1) ^ (intptr_t)(_p2)); \
} while (0)

enum _podfbv_threads_t
{
	THREAD_CONTROL,
	THREAD_FBV_INP,
	THREAD_FBV_OUT,
	THREAD_POD_INP,
	THREAD_POD_OUT,
	THREADS
};

#ifdef __cplusplus
extern "C" {
#endif

static void *control(void *const);
static void *fbvinp(void *const);
static void *fbvout(void *const);
static void *podinp(void *const);
static void *podout(void *const);

static const char *id2dev(const char *const id, char *const buf, const size_t size);
static void register_signals();
static void daemonize();

#ifdef __cplusplus
}
#endif

static void *(*const THREAD_FUNCTIONS[THREADS])(void *const) =
{
	[THREAD_CONTROL] = &control,
	[THREAD_FBV_INP] = &fbvinp,
	[THREAD_FBV_OUT] = &fbvout,
	[THREAD_POD_INP] = &podinp,
	[THREAD_POD_OUT] = &podout,
};

int main(int argc, char **argv)
{
	char
		fbv_str[128], pod_str[128];
	const char
		*fbv_id = "usb-Line_6_FBV_Express_Mk_II-00",
		*pod_id = "usb-Line_6_Line_6_Pocket_POD-00",
		*fbv_dev = 0,
		*pod_dev = 0;
	int
		fid_fbv = -1,
		fid_pod = -1;
	tic_t tic = 0;
	unsigned loop = 0, retry = 0;
	unsigned i;

	unsigned
		fbv2ctl_running = 0, ctl2fbv_running = 0,
		pod2ctl_running = 0, ctl2pod_running = 0;
	pthread_cond_t
		cond_rst = PTHREAD_COND_INITIALIZER,
		cond_fbv_inp = PTHREAD_COND_INITIALIZER,
		cond_fbv_out = PTHREAD_COND_INITIALIZER,
		cond_pod_inp = PTHREAD_COND_INITIALIZER,
		cond_pod_out = PTHREAD_COND_INITIALIZER;

	controller_state_t
		state = controller_state_initializer();
	tic_t
		tic_fbv2ctl[2] = { 0, 0 },
		tic_ctl2fbv[2] = { 0, 0 },
		tic_pod2ctl[2] = { 0, 0 },
		tic_ctl2pod[2] = { 0, 0 };
	unsigned char
		buf_fbv2ctl[2 * FBV_OUT_BUF_SIZE],
		buf_ctl2fbv[2 * FBV_INP_BUF_SIZE],
		buf_pod2ctl[2 * POD_OUT_BUF_SIZE],
		buf_ctl2pod[2 * POD_INP_BUF_SIZE];
	size_t
		len_fbv2ctl[2] = { 0, 0 },
		len_ctl2fbv[2] = { 0, 0 },
		len_pod2ctl[2] = { 0, 0 },
		len_ctl2pod[2] = { 0, 0 };
	midi_message_t
		msg_fbv2ctl = midi_message_initializer(tic_fbv2ctl, buf_fbv2ctl, FBV_OUT_BUF_SIZE, len_fbv2ctl),
		msg_ctl2fbv = midi_message_initializer(tic_ctl2fbv, buf_ctl2fbv, FBV_INP_BUF_SIZE, len_ctl2fbv),
		msg_pod2ctl = midi_message_initializer(tic_pod2ctl, buf_pod2ctl, POD_OUT_BUF_SIZE, len_pod2ctl),
		msg_ctl2pod = midi_message_initializer(tic_ctl2pod, buf_ctl2pod, POD_INP_BUF_SIZE, len_ctl2pod);

	thread_context_control_t
		ctx_control = thread_context_control_initializer(&ctl_running, &mutex, &cond_rst, &cond_ctl, &cond_fbv_inp, &cond_fbv_out, &cond_pod_inp, &cond_pod_out, &msg_fbv2ctl, &msg_ctl2fbv, &msg_pod2ctl, &msg_ctl2pod, &state);
	thread_context_message_t
		ctx_fbv2ctl = thread_context_message_initializer(&fbv2ctl_running, &mutex, &cond_rst, &cond_ctl, &cond_fbv_inp, &msg_fbv2ctl, &fid_fbv),
		ctx_ctl2fbv = thread_context_message_initializer(&ctl2fbv_running, &mutex, &cond_rst, &cond_ctl, &cond_fbv_out, &msg_ctl2fbv, &fid_fbv),
		ctx_pod2ctl = thread_context_message_initializer(&pod2ctl_running, &mutex, &cond_rst, &cond_ctl, &cond_pod_inp, &msg_pod2ctl, &fid_pod),
		ctx_ctl2pod = thread_context_message_initializer(&ctl2pod_running, &mutex, &cond_rst, &cond_ctl, &cond_pod_out, &msg_ctl2pod, &fid_pod);
	void *const context[THREADS] = {
		[THREAD_CONTROL] = (void *)&ctx_control,
		[THREAD_FBV_INP] = (void *)&ctx_fbv2ctl,
		[THREAD_FBV_OUT] = (void *)&ctx_ctl2fbv,
		[THREAD_POD_INP] = (void *)&ctx_pod2ctl,
		[THREAD_POD_OUT] = (void *)&ctx_ctl2pod,
	};
	unsigned *const running[THREADS] = {
		[THREAD_CONTROL] = (void *)&ctl_running,
		[THREAD_FBV_INP] = (void *)&fbv2ctl_running,
		[THREAD_FBV_OUT] = (void *)&ctl2fbv_running,
		[THREAD_POD_INP] = (void *)&pod2ctl_running,
		[THREAD_POD_OUT] = (void *)&ctl2pod_running,
	};

	pthread_t threads[THREADS];

	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--fbv_id") && (++i < argc))
			fbv_id = argv[i];
		else if (!strcmp(argv[i], "--pod_id") && (++i < argc))
			pod_id = argv[i];
		else if (!strcmp(argv[i], "--fbv_dev") && (++i < argc))
			fbv_id = fbv_dev = argv[i];
		else if (!strcmp(argv[i], "--pod_dev") && (++i < argc))
			pod_id = pod_dev = argv[i];
		else if (!strcmp(argv[i], "--loop"))
			loop = 1;
		else if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-d"))
			_daemon = loop = 1;
	}

	if (_daemon)
	{
		daemonize();
		info("Daemon started.\n");
	}
	else
		register_signals();

	for (;;)
	{

		if (fid_fbv < 0)
		{
			if ((fbv_dev != fbv_id) && !(fbv_dev = id2dev(fbv_id, fbv_str, sizeof(fbv_str))))
				goto cont0;
			if ((fid_fbv = open(fbv_dev, O_RDWR)) < 0)
			{
				debug("Failed to open FBV \"%s\".\n", fbv_dev ? fbv_dev : "<null>");
				goto cont0;
			}
			info("FBV device \"%s\" ready.\n", fbv_dev);
		}

		if (fid_pod < 0)
		{
			if ((pod_dev != pod_id) && !(pod_dev = id2dev(pod_id, pod_str, sizeof(pod_str))))
				goto cont0;
			if ((fid_pod = open(pod_dev, O_RDWR)) < 0)
			{
				debug("Failed to open POD \"%s\".\n", pod_dev ? pod_dev : "<null>");
				goto cont0;
			}
			for (i = 0; i < 5; i++)
			{
				ssize_t sent;
				char c;
				usleep(100000);
				if ((sent = write(fid_pod, &c, 0)) >= 0)
					break;
				close(fid_pod);
				if ((fid_pod = open(pod_dev, O_RDWR)) < 0)
				{
					error("Failed to open POD device \"%s\".\n", pod_dev);
					goto exit0;
				}
			}
			if (i >= 5)
			{
				error("Failed write initial channel to pod (%s).\n", strerror(errno));
				goto exit0;
			}
			info("POD device \"%s\" ready.\n", pod_dev);
		}

		retry = 0;
		tic_get(&tic);
		for (i = 0; i < TICS; i++)
			state.tic[i] = tic;
		for (i = 0; i < THREADS; i++)
		{
			if (!*running[i])
			{
				if (pthread_create(&threads[i], 0, THREAD_FUNCTIONS[i], context[i]))
				{
					error("Failed to create thread(s).\n");
					goto exit0;
				}
				*running[i] = 1;
			}
		}

		pthread_mutex_lock(&mutex);
		{
			unsigned sleep;
			do
			{
				if (pthread_cond_wait(&cond_rst, &mutex))
				{
					error("Wait failed.\n");
					goto exit0;
				}
				for (i = 0, sleep = 1; i < THREADS; i++)
					sleep &= *running[i] != 0;
			} while (sleep);
		}
		if (!ctl_running)
			fbv2ctl_running = ctl2fbv_running = pod2ctl_running = ctl2pod_running = 0;
		if ((!fbv2ctl_running || !ctl2fbv_running) && (fid_fbv >= 0))
		{
			close(fid_fbv);
			fid_fbv = -1;
			fbv2ctl_running = ctl2fbv_running = 0;
		}
		if ((!pod2ctl_running || !ctl2pod_running) && (fid_pod >= 0))
		{
			close(fid_pod);
			fid_pod = -1;
			pod2ctl_running = ctl2pod_running = 0;
		}
		pthread_cond_broadcast(&cond_ctl);
		pthread_cond_broadcast(&cond_fbv_inp);
		pthread_cond_broadcast(&cond_fbv_out);
		pthread_cond_broadcast(&cond_pod_inp);
		pthread_cond_broadcast(&cond_pod_out);
		pthread_mutex_unlock(&mutex);

		for (i = 0; i < THREADS; i++)
		{
			if (!*running[i])
				pthread_join(threads[i], 0);
		}

cont0:
		if (!loop || !ctl_running)
			break;
		usleep(retry++ < 100 ? 100000 : 1000000);
	}

	pthread_cond_destroy(&cond_rst);
	pthread_cond_destroy(&cond_ctl);
	pthread_cond_destroy(&cond_fbv_inp);
	pthread_cond_destroy(&cond_fbv_out);
	pthread_cond_destroy(&cond_pod_inp);
	pthread_cond_destroy(&cond_pod_out);
	pthread_mutex_destroy(&mutex);

	if (_daemon)
		info("Daemon terminated successfully.\n");
	return EXIT_SUCCESS;

exit0:
	if (fid_fbv >= 0)
		close(fid_fbv);
	if (fid_pod >= 0)
		close(fid_pod);
	if (_daemon)
		error("Daemon terminated with error(s).\n");
	return EXIT_FAILURE;
}

static const char *id2dev(const char *const id, char *const buf, const size_t size)
{
	const char
		*ctrl = "controlC",
		*path = "/dev/snd",
		*beg;
	int devno;
	char *ptr = buf;
	size_t ptr_size = size;
	ssize_t result;
	if ((result = snprintf(ptr, ptr_size, "%s/by-id/%s", path, id)) < 0)
	{
		debug("Insufficient buffer size.\n");
		goto exit0;
	}
	result -= strlen(id);
	beg = ptr;
	ptr += result;
	ptr_size -= result;
	if ((result = readlink(beg, ptr, ptr_size)) < 0)
	{
		debug("Device n/a or failed to read link.\n");
		goto exit0;
	}
	beg = ptr;
	ptr += result;
	ptr_size -= result;
	*ptr = 0;
	if (!(ptr = strstr(beg, ctrl)))
	{
		debug("Failed to find control device.\n");
		goto exit0;
	}
	devno = atoi(ptr + strlen(ctrl));
	sprintf(ptr, "midiC%iD0", devno);
	return buf;
exit0:
	return 0;
}

#ifdef __cplusplus
extern "C" {
#endif

static void sig_handler(int signum);

#ifdef __cplusplus
}
#endif
static void register_signals()
{
	if (_daemon)
	{
		signal(SIGCHLD, SIG_IGN);
		signal(SIGHUP, SIG_IGN);
	}
	signal(SIGINT, &sig_handler);
}

static void sig_handler(int signum)
{
	switch (signum)
	{
		case SIGINT:
			pthread_mutex_lock(&mutex);
			ctl_running = 0;
			pthread_cond_broadcast(&cond_ctl);
			pthread_mutex_unlock(&mutex);
		default:
			break;
	}
	signal(signum, SIG_DFL);
}

static void daemonize()
{
	int i;
	pid_t pid;
	if ((pid = fork()) < 0)
		exit(EXIT_FAILURE);
	if (pid > 0)
		exit(EXIT_SUCCESS);
	if (setsid() < 0)
		exit(EXIT_FAILURE);
	register_signals();
	if ((pid = fork()) < 0)
		exit(EXIT_FAILURE);
	if (pid > 0)
		exit(EXIT_SUCCESS);
	umask(0);
	chdir("/");
	for (i = sysconf(_SC_OPEN_MAX); i >= 0; i--)
		close(i);
	openlog("podfbv", LOG_PID, LOG_DAEMON);
}

typedef ssize_t (* input_parser_t)(const unsigned char *const buf, const size_t len);

#ifdef __cplusplus
extern "C" {
#endif

static ssize_t input_parser_fbv(const unsigned char *const buf, const size_t len);
static ssize_t input_parser_pod(const unsigned char *const buf, const size_t len);

static void *thread_function_input(void *const context, const input_parser_t parser, const char *const func);

#ifdef __cplusplus
}
#endif

static void *fbvinp(void *const context)
{
	void *ret;
	ret = thread_function_input(context, &input_parser_fbv, __FUNCTION__);
	return ret;
}

static void *podinp(void *const context)
{
	void *ret;
	ret = thread_function_input(context, &input_parser_pod, __FUNCTION__);
	return ret;
}

static void *thread_function_input(void *const context, const input_parser_t parse, const char *const func)
{
	thread_context_message_t *const ctx = (thread_context_message_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	pthread_cond_t
		*const cond_rst = ctx->cond_rst,
		*const cond_inp2ctl = ctx->cond_ctl,
		*const cond_ctl2inp = ctx->cond_dev;
	unsigned *const running = ctx->running;
	midi_message_t *const msg = ctx->msg;
	tic_t
		*tic1 = msg->_tic,
		*tic2 = msg->_tic + 1;
	unsigned char
		*buf1 = msg->_buf,
		*buf2 = msg->_buf + msg->_size;
	size_t
		*len1 = msg->_len,
		*len2 = msg->_len + 1;
	debug("%s started.\n", func);
	pthread_mutex_lock(mutex);
	*(msg->len = len1) = 0;
	pthread_cond_broadcast(cond_inp2ctl);
	debug("%s ready.\n", func);
	for (;;)
	{
		const int fid = *ctx->fid;
		unsigned char *ptr = buf1;
		ssize_t left = 1;
		while (*running && *len1)
		{
			if (pthread_cond_wait(cond_ctl2inp, mutex))
			{
				debug("Wait failed.\n");
				goto exit1;
			}
		}
		if (!*running)
			goto exit1;
		pthread_mutex_unlock(mutex);
		do
		{
			ssize_t rcvd;
			if ((rcvd = read(fid, ptr, left)) < 0)
			{
				debug("Failed to read data.\n");
				goto exit0;
			}
			if (left == 1)
				tic_get(tic1);
			ptr += rcvd;
			if ((left = parse(buf1, ptr - buf1)) < 0)
			{
				debug("Received unsupported message.\n");
				left = 1;
			}
		} while (left);
		pthread_mutex_lock(mutex);
		if (!*running)
			goto exit1;
		if ((*(msg->len = len1) = (ptr - (msg->buf = buf1))))
		{
			msg->tic = tic1;
//debug_msg(func, msg);
			pthread_cond_signal(cond_inp2ctl);
			swap_ptr(tic1, tic2);
			swap_ptr(buf1, buf2);
			swap_ptr(len1, len2);
		}
	}
	goto exit1;
exit0:
	pthread_mutex_lock(mutex);
exit1:
	*running = 0;
	pthread_cond_broadcast(cond_rst);
	pthread_mutex_unlock(mutex);
	debug("%s exit.\n", func);
	return 0;
}

#ifdef __cplusplus
extern "C" {
#endif

static ssize_t input_parser_default(const unsigned char *const buf, const size_t len);

#ifdef __cplusplus
}
#endif

static ssize_t input_parser_fbv(const unsigned char *const buf, const size_t len)
{
	return input_parser_default(buf, len);
}

static ssize_t input_parser_pod(const unsigned char *const buf, const size_t len)
{
	return input_parser_default(buf, len);
}

static ssize_t input_parser_default(const unsigned char *const buf, const size_t len)
{
	ssize_t left = -1;
	if (len)
	{
		size_t msglen = 0;
		switch (*buf)
		{
			case 0xb0: /* Control change */
				msglen = 3;
				break;
			case 0xc0: /* Program change */
				msglen = 2;
			default:
				break;
		}
		if (msglen && (len <= msglen))
			left = msglen - len;
	}
	return left;
}

#ifdef __cplusplus
extern "C" {
#endif

static void *thread_function_output(void *const context, const char *const func);

#ifdef __cplusplus
}
#endif

static void *fbvout(void *const context)
{
	void *ret;
	ret = thread_function_output(context, __FUNCTION__);
	return ret;
}

static void *podout(void *const context)
{
	void *ret;
	ret = thread_function_output(context, __FUNCTION__);
	return ret;
}

static void *thread_function_output(void *const context, const char *const func)
{
	thread_context_message_t *const ctx = (thread_context_message_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	pthread_cond_t
		*const cond_rst = ctx->cond_rst,
		*const cond_out2ctl = ctx->cond_ctl,
		*const cond_ctl2out = ctx->cond_dev;
	unsigned *const running = ctx->running;
	midi_message_t *const msg = ctx->msg;
	debug("%s started.\n", func);
	pthread_mutex_lock(mutex);
	for (;;)
	{
		if (!*running)
			goto exit1;
		if (msg->len)
			break;
		if (pthread_cond_wait(cond_ctl2out, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	}
	debug("%s ready.\n", func);
	do
	{
		if (*msg->len)
		{
//debug_msg(func, msg);
			pthread_mutex_unlock(mutex);
#if 1
			if (write(*ctx->fid, msg->buf, *msg->len) < 0)
			{
				debug("Failed to write data.\n");
				goto exit0;
			}
#endif
			pthread_mutex_lock(mutex);
			*msg->len = 0;
			pthread_cond_signal(cond_out2ctl);
		}
		if (pthread_cond_wait(cond_ctl2out, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	} while (*running);
	goto exit1;
exit0:
	pthread_mutex_lock(mutex);
exit1:
	*running = 0;
	pthread_cond_broadcast(cond_rst);
	pthread_mutex_unlock(mutex);
	debug("%s exit.\n", func);
	return 0;
}

static void *control(void *const context)
{
	thread_context_control_t *const ctx = (thread_context_control_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	pthread_cond_t
		*const cond_rst = ctx->cond_rst,
		*const cond_ctl = ctx->cond_ctl,
		*const cond_fbv_inp = ctx->cond_fbv_inp,
		*const cond_fbv_out = ctx->cond_fbv_out,
		*const cond_pod_inp = ctx->cond_pod_inp,
		*const cond_pod_out = ctx->cond_pod_out;
	unsigned *const running = ctx->running;
	midi_message_t
		*const msg_fbv2ctl = ctx->msg_fbv2ctl,
		*const msg_pod2ctl = ctx->msg_pod2ctl,
		*const msg_ctl2fbv = ctx->msg_ctl2fbv,
		*const msg_ctl2pod = ctx->msg_ctl2pod;
	size_t
		*len1_fbv = msg_ctl2fbv->_len,
		*len2_fbv = msg_ctl2fbv->_len + 1,
		*len1_pod = msg_ctl2pod->_len,
		*len2_pod = msg_ctl2pod->_len + 1;
	debug("%s started.\n", __FUNCTION__);
	//notify output threads
	pthread_mutex_lock(mutex);
	*(msg_ctl2fbv->len = len1_fbv) = 0;
	pthread_cond_broadcast(cond_fbv_out);
	*(msg_ctl2pod->len = len1_pod) = 0;
	pthread_cond_broadcast(cond_pod_out);
	//wait input threads
	for (;;)
	{
		if (!*running)
			goto exit1;
		if (msg_fbv2ctl->len && msg_pod2ctl->len)
			break;
		if (pthread_cond_wait(cond_ctl, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	}
	debug("%s ready.\n", __FUNCTION__);
	do
	{
		unsigned char
			*buf1_fbv = msg_ctl2fbv->_buf,
			*buf2_fbv = msg_ctl2fbv->_buf + msg_ctl2fbv->_size,
			*buf1_pod = msg_ctl2pod->_buf,
			*buf2_pod = msg_ctl2pod->_buf + msg_ctl2pod->_size;
		controller_state_t *const state = ctx->state;
		if (*msg_fbv2ctl->len && !*len1_pod)
		{
			const midi_message_t *const inp = msg_fbv2ctl;
			midi_message_t *const out = msg_ctl2pod;
			unsigned char *ptr = buf1_pod;
debug_msg("FBV > CTL", inp);
			switch (inp->buf[0])
			{
				case 0xb0:
					if (*inp->len == 3)
					{
						switch (inp->buf[1])
						{
							case 0x07: //channel volume
							{
								const char
									val = inp->buf[2],
									diff = val < state->vol ? state->vol - val : val - state->vol;
								if (diff >= FBV_PEDAL_THRESH)
								{
									*ptr++ = inp->buf[0];
									*ptr++ = inp->buf[1];
									*ptr++ = (state->vol = val);
								}
								break;
							}
							case 0x0b: //expression
							{
								const char
									val = inp->buf[2],
									diff = val < state->expr ? state->expr - val : val - state->expr;
								if (diff >= FBV_PEDAL_THRESH)
								{
									*ptr++ = 0xb0;
									*ptr++ = 0x04;
									*ptr++ = (state->expr = val);
								}
								break;
							}
							case 0x14: case 0x15: case 0x16: case 0x17: //btn codes
							{
								const unsigned btn = inp->buf[1] - 0x14;
								if (inp->buf[2]) //press
								{
									tic_t *const tic = &state->tic[btn + TIC_BTN_A];
									*tic = *inp->tic;
//debug("Press %i\n", btn);
									if (btn != state->btn)
									{
										//btn change
										*ptr++ = 0xc0;
										*ptr++ = (state->btn = btn) + state->bank * FBV_BTNS + 1;
									}
									else
									{
										//tap
										*ptr++ = 0xb0;
										*ptr++ = 0x40;
										*ptr++ = 0x7f;
									}
								}
#if 0
								else //release
								{
									if (btn == state->btn)
									{
										const tic_t
											*const tic1 = inp->tic,
											*const tic0 = &state->tic[btn + TIC_BTN_A],
											dtic = *tic1 - *tic0;
//debug("Release %i (%lli)\n", btn, dtic);
									}
								}
#endif
								break;
							}
							case 0x66: //foot switch
								*ptr++ = 0xb0;
								*ptr++ = 0x2b;
								*ptr++ = inp->buf[2] ? 0x40 : 0x00;
							default:
								break;
						}
					}
				default:
					break;
			}
			if ((*(out->len = len1_pod) = (ptr - (out->buf = buf1_pod))))
			{
debug_msg("POD < CTL", out);
				pthread_cond_signal(cond_pod_out);
				swap_ptr(buf1_pod, buf2_pod);
				swap_ptr(len1_pod, len2_pod);
			}
			*msg_fbv2ctl->len = 0;
			pthread_cond_signal(cond_fbv_inp);
		}
		if (*msg_pod2ctl->len && !*len1_fbv)
		{
			const midi_message_t *const inp = msg_pod2ctl;
			midi_message_t *const out = msg_ctl2fbv;
			unsigned char *ptr = buf1_fbv;
debug_msg("POD > CTL", inp);
			switch (inp->buf[0])
			{
				case 0xb0:
					if (*inp->len == 3)
					{
						switch (inp->buf[1])
						{
							default:
								break;
						}
					}
					break;
				case 0xc0:
					if (*inp->len == 2)
					{
						const unsigned idx = inp->buf[1] - 1;
						state->bank = idx / FBV_BTNS;
						state->btn = idx % FBV_BTNS;
					}
				default:
					break;
			}
			if ((*(out->len = len1_fbv) = (ptr - (out->buf = buf1_fbv))))
			{
debug_msg("FBV < CTL", out);
				pthread_cond_signal(cond_fbv_out);
				swap_ptr(buf1_fbv, buf2_fbv);
				swap_ptr(len1_fbv, len2_fbv);
			}
			*msg_pod2ctl->len = 0;
			pthread_cond_signal(cond_pod_inp);
		}
		if (pthread_cond_wait(cond_ctl, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	} while (*running);
exit1:
	*running = 0;
	pthread_cond_signal(cond_rst);
	pthread_mutex_unlock(mutex);
	debug("%s exit.\n", __FUNCTION__);
	return 0;
}
