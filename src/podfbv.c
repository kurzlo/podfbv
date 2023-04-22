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
	for (i = 0; i < (_msg)->len; i++) \
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
		pthread_cond_t *cond; \
		__VA_ARGS__; }

#define thread_context_initializer(_running, _mutex, _cond, ...) { \
	.running = _running, .mutex = _mutex, .cond = _cond, \
	__VA_ARGS__ }

typedef struct _midi_message_t {
	unsigned cnt;
	const tic_t *tic;
	const unsigned char *buf;
	size_t len;
} midi_message_t;

#define midi_message_initializer(_cnt, _tic, _buf, _len) { \
	.cnt = _cnt, .tic = _tic, .buf = _buf, .len = _len }

typedef thread_context_define(message_t,
	int *fid;
	midi_message_t *msg) thread_context_message_t;

#define thread_context_message_initializer(_running, _mutex, _cond, _fid, _msg) \
	thread_context_initializer(_running, _mutex, _cond, \
		.fid = _fid, .msg = _msg)

enum _ctl_tic_t {
	TIC_BTN_A,
	TIC_BTN_B,
	TIC_BTN_C,
	TIC_BTN_D,
	TICS
};

typedef struct _controller_state_t {
	unsigned char bank, btn;
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
	pthread_cond_t *cond_fbv, *cond_pod;
	midi_message_t *msg_fbv2ctl, *msg_ctl2fbv, *msg_pod2ctl, *msg_ctl2pod;
	controller_state_t *state) thread_context_control_t;

#define thread_context_control_initializer(_running, _mutex, _cond_ctl, _cond_fbv, _cond_pod, _fbv2ctl, _ctl2fbv, _pod2ctl, _ctl2pod, _state) \
	thread_context_initializer(_running, _mutex, _cond_ctl, \
		.cond_fbv = _cond_fbv, .cond_pod = _cond_pod, \
		.msg_fbv2ctl = _fbv2ctl, .msg_ctl2fbv = _ctl2fbv, .msg_pod2ctl = _pod2ctl, .msg_ctl2pod = _ctl2pod, \
		.state = _state)

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
		cond_fbv = PTHREAD_COND_INITIALIZER,
		cond_pod = PTHREAD_COND_INITIALIZER;

	controller_state_t
		state = controller_state_initializer();
	midi_message_t
		msg_fbv2ctl = midi_message_initializer(0, 0, 0, 0),
		msg_ctl2fbv = midi_message_initializer(0, 0, 0, 0),
		msg_pod2ctl = midi_message_initializer(0, 0, 0, 0),
		msg_ctl2pod = midi_message_initializer(0, 0, 0, 0);

	thread_context_control_t
		ctx_control = thread_context_control_initializer(&ctl_running, &mutex, &cond_ctl, &cond_fbv, &cond_pod, &msg_fbv2ctl, &msg_ctl2fbv, &msg_pod2ctl, &msg_ctl2pod, &state);
	thread_context_message_t
		ctx_fbv2ctl = thread_context_message_initializer(&fbv2ctl_running, &mutex, &cond_ctl, &fid_fbv, &msg_fbv2ctl),
		ctx_ctl2fbv = thread_context_message_initializer(&ctl2fbv_running, &mutex, &cond_fbv, &fid_fbv, &msg_ctl2fbv),
		ctx_pod2ctl = thread_context_message_initializer(&pod2ctl_running, &mutex, &cond_ctl, &fid_pod, &msg_pod2ctl),
		ctx_ctl2pod = thread_context_message_initializer(&ctl2pod_running, &mutex, &cond_pod, &fid_pod, &msg_ctl2pod);
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
				if (pthread_cond_wait(&cond_ctl, &mutex))
				{
					error("Wait failed.\n");
					goto exit0;
				}
				for (i = 0, sleep = 1; i < THREADS; i++)
					sleep &= *running[i] != 0;
			} while (sleep);
		}
		if ((!ctl_running || !fbv2ctl_running || !ctl2fbv_running) && (fid_fbv >= 0))
		{
			close(fid_fbv);
			fid_fbv = -1;
			fbv2ctl_running = ctl2fbv_running = 0;
		}
		if ((!ctl_running || !pod2ctl_running || !ctl2pod_running) && (fid_pod >= 0))
		{
			close(fid_pod);
			fid_pod = -1;
			pod2ctl_running = ctl2pod_running = 0;
		}
		pthread_cond_broadcast(&cond_ctl);
		pthread_cond_broadcast(&cond_fbv);
		pthread_cond_broadcast(&cond_pod);
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

	pthread_cond_destroy(&cond_ctl);
	pthread_cond_destroy(&cond_fbv);
	pthread_cond_destroy(&cond_pod);
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

static void *thread_function_input(void *const context, unsigned char *const buffer, const size_t size, const input_parser_t parser);

#ifdef __cplusplus
}
#endif

static void *fbvinp(void *const context)
{
	unsigned char buf[2 * FBV_INP_BUF_SIZE];
	void *ret;
	debug("FBV input started.\n");
	ret = thread_function_input(context, buf, sizeof(buf), &input_parser_fbv);
	debug("FBV input terminated.\n");
	return ret;
}

static void *podinp(void *const context)
{
	unsigned char buf[2 * POD_INP_BUF_SIZE];
	void *ret;
	debug("POD input started.\n");
	ret = thread_function_input(context, buf, sizeof(buf), &input_parser_pod);
	debug("POD input terminated.\n");
	return ret;
}

static void *thread_function_input(void *const context, unsigned char *const buffer, const size_t size, const input_parser_t parse)
{
	thread_context_message_t *const ctx = (thread_context_message_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	pthread_cond_t *const cond = ctx->cond;
	unsigned *const running = ctx->running;
	unsigned char
		*buf1 = buffer,
		*buf2 = buffer + size;
	tic_t
		tic[2] = { [0] = 0LL, [1] = 0LL },
		*tic1 = tic,
		*tic2 = tic + 1;
	pthread_mutex_lock(mutex);
	while (*running)
	{
		const int fid = *ctx->fid;
		midi_message_t *const msg = ctx->msg;
		unsigned char *ptr = buf1;
		ssize_t left = 1;
		pthread_mutex_unlock(mutex);
		do
		{
			ssize_t rcvd;
			if ((rcvd = read(fid, ptr, left)) < 0)
			{
				debug("Failed to read data.\n");
				goto exit0;
			}
			tic_get(tic1);
			ptr += rcvd;
			if ((left = parse(buf1, ptr - buf1)) < 0)
			{
				debug("Received unsupported message.\n");
				break;
			}
		} while (left);
		pthread_mutex_lock(mutex);
		if (!left)
		{
			if ((msg->len = ptr - (msg->buf = buf1)))
			{
				msg->cnt++;
				msg->tic = tic1;
				pthread_cond_signal(cond);
				swap_ptr(buf1, buf2);
				swap_ptr(tic1, tic2);
			}
		}
	}
	pthread_mutex_unlock(mutex);
	return 0;
exit0:
	pthread_mutex_lock(mutex);
	*running = 0;
	pthread_cond_broadcast(cond);
	pthread_mutex_unlock(mutex);
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

static void *thread_function_output(void *const context);

#ifdef __cplusplus
}
#endif

static void *fbvout(void *const context)
{
	void *ret;
	debug("FBV output started.\n");
	ret = thread_function_output(context);
	debug("FBV output terminated.\n");
	return ret;
}

static void *podout(void *const context)
{
	void *ret;
	debug("POD output started.\n");
	ret = thread_function_output(context);
	debug("POD output terminated.\n");
	return ret;
}

static void *thread_function_output(void *const context)
{
	thread_context_message_t *const ctx = (thread_context_message_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	unsigned *const running = ctx->running;
	pthread_mutex_lock(mutex);
	while (*running)
	{
		const int fid = *ctx->fid;
		pthread_cond_t *const cond = ctx->cond;
		midi_message_t *const msg = ctx->msg;
		const unsigned cnt = msg->cnt;
		do {
			if (pthread_cond_wait(cond, mutex))
			{
				debug("Wait failed.\n");
				goto exit1;
			}
		} while (*running && (cnt == msg->cnt));
		if (msg->len)
		{
			pthread_mutex_unlock(mutex);
#if 1
			if (write(fid, msg->buf, msg->len) < 0)
			{
				debug("Failed to write data.\n");
				goto exit0;
			}
#endif
			pthread_mutex_lock(mutex);
		}
	}
exit1:
	pthread_mutex_unlock(mutex);
exit0:
	return 0;
}

static void *control(void *const context)
{
	thread_context_control_t *const ctx = (thread_context_control_t *)context;
	pthread_mutex_t *const mutex = ctx->mutex;
	unsigned *const running = ctx->running;
	pthread_mutex_lock(mutex);
	while (*running)
	{
		pthread_cond_t
			*const cond_ctl = ctx->cond,
			*const cond_fbv = ctx->cond_fbv,
			*const cond_pod = ctx->cond_pod;
		const midi_message_t
			*const msg_fbv2ctl = ctx->msg_fbv2ctl,
			*const msg_pod2ctl = ctx->msg_pod2ctl;
		const unsigned
			cnt_fbv2ctl = msg_fbv2ctl->cnt,
			cnt_pod2ctl = msg_pod2ctl->cnt;
		midi_message_t
			*const msg_ctl2fbv = ctx->msg_ctl2fbv,
			*const msg_ctl2pod = ctx->msg_ctl2pod;
		unsigned
			upd_fbv2ctl = 0, upd_pod2ctl = 0;
		unsigned char
			buf_fbv[2 * FBV_OUT_BUF_SIZE],
			*buf1_fbv = buf_fbv,
			*buf2_fbv = buf_fbv + FBV_OUT_BUF_SIZE,
			buf_pod[2 * POD_OUT_BUF_SIZE],
			*buf1_pod = buf_pod,
			*buf2_pod = buf_pod + POD_OUT_BUF_SIZE;
		controller_state_t *const state = ctx->state;
		do {
			if (pthread_cond_wait(cond_ctl, mutex))
			{
				debug("Wait failed.\n");
				goto exit1;
			}
		} while (*running && !(upd_fbv2ctl = (cnt_fbv2ctl != msg_fbv2ctl->cnt)) && !(upd_pod2ctl = (cnt_pod2ctl != msg_pod2ctl->cnt)));
		if (!*running)
			break;
		if (upd_fbv2ctl && msg_fbv2ctl->len)
		{
			const midi_message_t *const inp = msg_fbv2ctl;
			midi_message_t *const out = msg_ctl2pod;
			unsigned char *ptr = buf1_pod;
			const unsigned char *buf = ptr;
debug_msg("FBV > CTL", inp);
			switch (inp->buf[0])
			{
				case 0xb0:
					if (inp->len == 3)
					{
						switch (inp->buf[1])
						{
							case 0x07: //channel volume
								memcpy(ptr, inp->buf, inp->len);
								ptr += inp->len;
								break;
							case 0x0b: //expression
								*ptr++ = 0xb0;
								*ptr++ = 0x04;
								*ptr++ = inp->buf[2];
								break;
							case 0x14: case 0x15: case 0x16: case 0x17: //btn codes
							{
								const unsigned btn = inp->buf[1] - 0x14;
								if (inp->buf[2]) //press
								{
									tic_t *const tic = &state->tic[btn + TIC_BTN_A];
									*tic = *inp->tic;
debug("Press %i\n", btn);
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
								else //release
								{
									if (btn == state->btn)
									{
										const tic_t
											*const tic1 = inp->tic,
											*const tic0 = &state->tic[btn + TIC_BTN_A],
											dtic = *tic1 - *tic0;
debug("Release %i (%lli)\n", btn, dtic);
									}
								}
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
			if ((out->len = (ptr - (out->buf = buf))))
			{
				out->cnt++;
debug_msg("CTL > POD", out);
				pthread_cond_signal(cond_pod);
				swap_ptr(buf1_pod, buf2_pod);
			}
		}
		if (upd_pod2ctl && msg_pod2ctl->len)
		{
			const midi_message_t *const inp = msg_pod2ctl;
			midi_message_t *const out = msg_ctl2fbv;
			unsigned char *ptr = buf1_fbv;
			const unsigned char *buf = ptr;
debug_msg("POD > CTL", inp);
			switch (inp->buf[0])
			{
				case 0xb0:
					if (inp->len == 3)
					{
						switch (inp->buf[1])
						{
							default:
								break;
						}
					}
					break;
				case 0xc0:
					if (inp->len == 2)
					{
						const unsigned idx = inp->buf[1] - 1;
						state->bank = idx / FBV_BTNS;
						state->btn = idx % FBV_BTNS;
					}
					break;
				default:
					break;
			}
			if ((out->len = (ptr - (out->buf = buf))))
			{
				out->cnt++;
debug_msg("CTL > FBV", out);
				pthread_cond_signal(cond_fbv);
				swap_ptr(buf1_fbv, buf2_fbv);
			}
		}
	}
exit1:
	pthread_mutex_unlock(mutex);
	return 0;
}
