#include "api.h"

#ifdef API_WIN
#	include <mmsystem.h>
#else
#	include <sys/types.h>
#	include <sys/stat.h>
#	include <sys/soundcard.h>
#	include <syslog.h>
#	include <fcntl.h>
#	include <errno.h>
#	include <signal.h>
#	include <stddef.h>
#	include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
#ifndef API_WIN
	_daemon = 0,
#endif
	loop = 0,
	ctl_running = 0;

mutex_t mutex;
cond_t cond_ctl;

#ifdef API_WIN

#	define error(_fmt, ...) do { \
		printf(_fmt, ##__VA_ARGS__); \
	} while (0)

#	define info(_fmt, ...) do { \
		printf(_fmt, ##__VA_ARGS__); \
	} while (0)

#	ifdef DEBUG
#		define debug(_fmt, ...) do { \
			printf("%s(%i): " _fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
		} while (0)
#	else
#		define debug(_fmt, ...) {}
#	endif

#	ifdef DEBUG
#		define debug_msg(_str, _msg) do { \
			unsigned i; \
			if (_str) \
				debug("%s:", _str); \
			for (i = 0; i < *(_msg)->len; i++) \
				printf(" 0x%02x", (_msg)->buf[i]); \
			puts(""); \
		} while (0)
#	else
#		define debug_msg(_str, _msg) {}
#	endif

#else

#	define error(_fmt, ...) do { \
		if (_daemon) \
			syslog(LOG_ERR, _fmt, ##__VA_ARGS__); \
		else \
			printf(_fmt, ##__VA_ARGS__); \
	} while (0)

#	define info(_fmt, ...) do { \
		if (_daemon) \
			syslog(LOG_NOTICE, _fmt, ##__VA_ARGS__); \
		else \
			printf(_fmt, ##__VA_ARGS__); \
	} while (0)

#	ifdef DEBUG
#		define debug(_fmt, ...) do { \
			if (!_daemon) \
				printf("%s(%i): " _fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
		} while (0)
#	else
#		define debug(_fmt, ...) {}
#	endif

#	ifdef DEBUG
#		define debug_msg(_str, _msg) do { \
			if (!_daemon) { \
				unsigned i; \
				if (_str) \
					debug("%s:", _str); \
				for (i = 0; i < *(_msg)->len; i++) \
					printf(" 0x%02x", (_msg)->buf[i]); \
				puts(""); \
			} \
		} while (0)
#	else
#		define debug_msg(_str, _msg) {}
#	endif

#endif

#define thread_context_type(_t) \
	struct _thread_context_##_t

#define thread_context_define(_t, ...) \
	thread_context_type(_t) { \
		unsigned *running; \
		mutex_t *mutex; \
		cond_t *cond_rst, *cond_ctl; \
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

#ifdef API_WIN
typedef struct _fid_t {
	HMIDIIN inp;
	HMIDIOUT out;
} fid_t;
#	define fid_initializer() { \
		.inp = INVALID_HANDLE_VALUE, .out = INVALID_HANDLE_VALUE }
#else
typedef int fid_t;
#	define fid_initializer() -1
#endif

typedef thread_context_define(message_t,
	cond_t *cond_dev;
	midi_message_t *msg;
	fid_t *fid) thread_context_message_t;

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
	cond_t *cond_fbv_inp, *cond_fbv_out, *cond_pod_inp, *cond_pod_out;
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
#ifndef API_WIN
	THREAD_FBV_INP,
#endif
	THREAD_FBV_OUT,
#ifndef API_WIN
	THREAD_POD_INP,
#endif
	THREAD_POD_OUT,
	THREADS
};

#ifdef __cplusplus
extern "C" {
#endif

static void *control(void *const);
#ifdef API_WIN
static void CALLBACK fbvinp(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
#else
static void *fbvinp(void *const);
#endif
static void *fbvout(void *const);
#ifdef API_WIN
static void CALLBACK podinp(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
#else
static void *podinp(void *const);
#endif
static void *podout(void *const);

#ifdef API_WIN
static int get_inp_num(const char *const name);
static int get_out_num(const char *const name);
#else
static const char *id2dev(const char *const id, char *const buf, const size_t size);
static void register_signals();
static void daemonize();
#endif

#ifdef __cplusplus
}
#endif

static void *(*const THREAD_FUNCTIONS[THREADS])(void *const) =
{
	[THREAD_CONTROL] = &control,
#ifndef API_WIN
	[THREAD_FBV_INP] = &fbvinp,
#endif
	[THREAD_FBV_OUT] = &fbvout,
#ifndef API_WIN
	[THREAD_POD_INP] = &podinp,
#endif
	[THREAD_POD_OUT] = &podout,
};

int main(int argc, char **argv)
{
#ifdef API_WIN
	const char
		*fbv_id = "FBV Express Mk II",
		*pod_id = "Line 6 Pocket POD";
#else
	char
		fbv_str[128], pod_str[128];
	const char
		*fbv_id = "usb-Line_6_FBV_Express_Mk_II-00",
		*pod_id = "usb-Line_6_Line_6_Pocket_POD-00",
		*fbv_dev = 0,
		*pod_dev = 0;
#endif
	fid_t
		fid_fbv = fid_initializer(),
		fid_pod = fid_initializer();
	tic_t tic = 0;
	unsigned retry = 0;
	unsigned i;

	unsigned
		fbv2ctl_running = 0, ctl2fbv_running = 0,
		pod2ctl_running = 0, ctl2pod_running = 0;
	cond_t
		cond_rst,
		cond_fbv_inp,
		cond_fbv_out,
		cond_pod_inp,
		cond_pod_out;

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
#ifndef API_WIN
		[THREAD_FBV_INP] = (void *)&ctx_fbv2ctl,
#endif
		[THREAD_FBV_OUT] = (void *)&ctx_ctl2fbv,
#ifndef API_WIN
		[THREAD_POD_INP] = (void *)&ctx_pod2ctl,
#endif
		[THREAD_POD_OUT] = (void *)&ctx_ctl2pod,
	};
	unsigned *const running[THREADS] = {
		[THREAD_CONTROL] = (void *)&ctl_running,
#ifndef API_WIN
		[THREAD_FBV_INP] = (void *)&fbv2ctl_running,
#endif
		[THREAD_FBV_OUT] = (void *)&ctl2fbv_running,
#ifndef API_WIN
		[THREAD_POD_INP] = (void *)&pod2ctl_running,
#endif
		[THREAD_POD_OUT] = (void *)&ctl2pod_running,
	};

	thread_t threads[THREADS];

	mutex_init(&mutex);
	cond_init(&cond_rst);
	cond_init(&cond_ctl);
	cond_init(&cond_fbv_inp);
	cond_init(&cond_fbv_out);
	cond_init(&cond_pod_inp);
	cond_init(&cond_pod_out);

	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--fbv_id") && (++i < argc))
			fbv_id = argv[i];
		else if (!strcmp(argv[i], "--pod_id") && (++i < argc))
			pod_id = argv[i];
#ifndef API_WIN
		else if (!strcmp(argv[i], "--fbv_dev") && (++i < argc))
			fbv_id = fbv_dev = argv[i];
		else if (!strcmp(argv[i], "--pod_dev") && (++i < argc))
			pod_id = pod_dev = argv[i];
#endif
		else if (!strcmp(argv[i], "--loop"))
			loop = 1;
#ifndef API_WIN
		else if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-d"))
			_daemon = loop = 1;
#endif
	}

#ifndef API_WIN
	if (_daemon)
	{
		daemonize();
		info("Daemon started.\n");
	}
	else
		register_signals();
#endif

	for (;;)
	{
#ifdef API_WIN
		if (fid_fbv.inp == INVALID_HANDLE_VALUE)
		{
			int num;
			if ((num = get_inp_num(fbv_id)) < 0)
				goto cont0;
			if ((midiInOpen(&fid_fbv.inp, num, (DWORD_PTR)&fbvinp, (DWORD_PTR)&ctx_fbv2ctl, CALLBACK_FUNCTION) != MMSYSERR_NOERROR))
			{
				debug("Failed to open FBV input \"%s\".\n", fbv_id);
				goto cont0;
			}
			midiInStart(fid_fbv.inp);
			fbv2ctl_running = 1;
		}
		if (fid_fbv.out == INVALID_HANDLE_VALUE)
		{
			int num;
			if ((num = get_out_num(fbv_id)) < 0)
				goto cont0;
			if ((midiOutOpen(&fid_fbv.out, num, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR))
			{
				debug("Failed to open FBV output \"%s\".\n", fbv_id);
				goto cont0;
			}
		}
		if (fid_pod.inp == INVALID_HANDLE_VALUE)
		{
			int num;
			if ((num = get_inp_num(pod_id)) < 0)
				goto cont0;
			if ((midiInOpen(&fid_pod.inp, num, (DWORD_PTR)&podinp, (DWORD_PTR)&ctx_pod2ctl, CALLBACK_FUNCTION) != MMSYSERR_NOERROR))
			{
				debug("Failed to open FBV input \"%s\".\n", pod_id);
				goto cont0;
			}
			midiInStart(fid_pod.inp);
			pod2ctl_running = 1;
		}
		if (fid_pod.out == INVALID_HANDLE_VALUE)
		{
			int num;
			if ((num = get_out_num(pod_id)) < 0)
				goto cont0;
			if ((midiOutOpen(&fid_pod.out, num, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR))
			{
				debug("Failed to open FBV output \"%s\".\n", pod_id);
				goto cont0;
			}
		}
#else
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
				sleep_ms(100);
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
#endif

		retry = 0;
		tic_get(&tic);
		for (i = 0; i < TICS; i++)
			state.tic[i] = tic;
		for (i = 0; i < THREADS; i++)
		{
			if (!*running[i])
			{
debug("Create %i\n", i);
				if (thread_create(&threads[i], THREAD_FUNCTIONS[i], context[i]))
				{
					error("Failed to create thread(s).\n");
					goto exit0;
				}
				*running[i] = 1;
			}
		}

		mutex_lock(&mutex);
		{
			unsigned sleep;
			do
			{
				if (cond_wait(&cond_rst, &mutex))
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
		if ((!fbv2ctl_running || !ctl2fbv_running) &&
#ifdef API_WIN
			(fid_fbv.inp != INVALID_HANDLE_VALUE) && (fid_fbv.out != INVALID_HANDLE_VALUE)
#else
			(fid_fbv >= 0)
#endif
			)
		{
debug("Reset FBV\n");
#ifdef API_WIN
			midiOutReset(fid_fbv.out);
			midiOutClose(fid_fbv.out);
			midiInStop(fid_fbv.inp);
			midiInClose(fid_fbv.inp);
			fid_fbv.inp = INVALID_HANDLE_VALUE;
			fid_fbv.out = INVALID_HANDLE_VALUE;
#else
			close(fid_fbv);
			fid_fbv = -1;
#endif
			fbv2ctl_running = ctl2fbv_running = 0;
		}
		if ((!pod2ctl_running || !ctl2pod_running) &&
#ifdef API_WIN
			(fid_pod.inp != INVALID_HANDLE_VALUE) && (fid_pod.out != INVALID_HANDLE_VALUE)
#else
			(fid_pod >= 0)
#endif
			)
		{
debug("Reset POD\n");
#ifdef API_WIN
			midiOutReset(fid_pod.out);
			midiOutClose(fid_pod.out);
			midiInStop(fid_pod.inp);
			midiInClose(fid_pod.inp);
			fid_pod.inp = INVALID_HANDLE_VALUE;
			fid_pod.out = INVALID_HANDLE_VALUE;
#else
			close(fid_pod);
			fid_pod = -1;
#endif
			pod2ctl_running = ctl2pod_running = 0;
		}
		cond_broadcast(&cond_ctl);
		cond_broadcast(&cond_fbv_inp);
		cond_broadcast(&cond_fbv_out);
		cond_broadcast(&cond_pod_inp);
		cond_broadcast(&cond_pod_out);
		mutex_unlock(&mutex);

		for (i = 0; i < THREADS; i++)
		{
			if (!*running[i])
			{
debug("Join %i\n", i);
				thread_join(&threads[i]);
			}
		}

cont0:
		if (!loop)
			break;
		sleep_ms(retry++ < 100 ? 100 : 1000);
	}

	cond_destroy(&cond_rst);
	cond_destroy(&cond_ctl);
	cond_destroy(&cond_fbv_inp);
	cond_destroy(&cond_fbv_out);
	cond_destroy(&cond_pod_inp);
	cond_destroy(&cond_pod_out);
	mutex_destroy(&mutex);

#ifdef API_WIN
	if (fid_fbv.out != INVALID_HANDLE_VALUE)
	{
		midiOutReset(fid_fbv.out);
		midiOutClose(fid_fbv.out);
	}
	if (fid_fbv.inp != INVALID_HANDLE_VALUE)
	{
		midiInStop(fid_fbv.inp);
		midiInClose(fid_fbv.inp);
	}
	if (fid_pod.out != INVALID_HANDLE_VALUE)
	{
		midiOutReset(fid_pod.out);
		midiOutClose(fid_pod.out);
	}
	if (fid_pod.inp != INVALID_HANDLE_VALUE)
	{
		midiInStop(fid_pod.inp);
		midiInClose(fid_pod.inp);
	}
#else
	if (fid_fbv >= 0)
		close(fid_fbv);
	if (fid_pod >= 0)
		close(fid_pod);
	if (_daemon)
		info("Daemon terminated successfully.\n");
#endif
	return EXIT_SUCCESS;

exit0:
#ifdef API_WIN
	if (fid_fbv.out != INVALID_HANDLE_VALUE)
	{
		midiOutReset(fid_fbv.out);
		midiOutClose(fid_fbv.out);
	}
	if (fid_fbv.inp != INVALID_HANDLE_VALUE)
	{
		midiInStop(fid_fbv.inp);
		midiInClose(fid_fbv.inp);
	}
	if (fid_pod.out != INVALID_HANDLE_VALUE)
	{
		midiOutReset(fid_pod.out);
		midiOutClose(fid_pod.out);
	}
	if (fid_pod.inp != INVALID_HANDLE_VALUE)
	{
		midiInStop(fid_pod.inp);
		midiInClose(fid_pod.inp);
	}
#else
	if (fid_fbv >= 0)
		close(fid_fbv);
	if (fid_pod >= 0)
		close(fid_pod);
	if (_daemon)
		error("Daemon terminated with error(s).\n");
#endif
	return EXIT_FAILURE;
}

#ifdef API_WIN

static int get_inp_num(const char *const name)
{
	int num = -1;
	const int N = midiInGetNumDevs();
	int i;
	for (i = 0; i < N; i++)
	{
		MIDIINCAPS caps;
		midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS));
		if (!strncmp(caps.szPname, name, strlen(name)))
		{
			num = i;
			break;
		}
	}
	return num;
}

static int get_out_num(const char *const name)
{
	int num = -1;
	const int N = midiOutGetNumDevs();
	int i;
	for (i = 0; i < N; i++)
	{
		MIDIOUTCAPS caps;
		midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS));
		if (!strncmp(caps.szPname, name, strlen(name)))
		{
			num = i;
			break;
		}
	}
	return num;
}

#else

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
			mutex_lock(&mutex);
			loop = ctl_running = 0;
			cond_broadcast(&cond_ctl);
			mutex_unlock(&mutex);
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

#endif /*API_WIN*/


#ifdef __cplusplus
extern "C" {
#endif

#ifdef API_WIN
static void CALLBACK callback_input(HMIDIIN handle, const UINT message_type, const DWORD_PTR context, const DWORD_PTR param1, const DWORD_PTR param2, const char *const func);
#else
static void *thread_function_input(void *const context, const char *const func);
#endif

#ifdef __cplusplus
}
#endif

#ifdef API_WIN
static void CALLBACK fbvinp(HMIDIIN handle, UINT message_type, DWORD_PTR context, DWORD_PTR param1, DWORD_PTR param2)
{
	callback_input(handle, message_type, context, param1, param2, __FUNCTION__);
}
static void CALLBACK podinp(HMIDIIN handle, UINT message_type, DWORD_PTR context, DWORD_PTR param1, DWORD_PTR param2)
{
	callback_input(handle, message_type, context, param1, param2, __FUNCTION__);
}
#else
static void *fbvinp(void *const context)
{
	void *ret;
	ret = thread_function_input(context, __FUNCTION__);
	return ret;
}
static void *podinp(void *const context)
{
	void *ret;
	ret = thread_function_input(context, __FUNCTION__);
	return ret;
}
#endif


#ifndef API_WIN

#ifdef __cplusplus
extern "C" {
#endif

static ssize_t parse_input(const unsigned char *const buf, const size_t len);

#ifdef __cplusplus
}
#endif

#endif

#ifdef API_WIN
static void CALLBACK callback_input(HMIDIIN handle, const UINT message_type, const DWORD_PTR context, const DWORD_PTR param1, const DWORD_PTR param2, const char *const func)
#else
static void *thread_function_input(void *const context, const char *const func)
#endif
{
	thread_context_message_t *const ctx = (thread_context_message_t *)context;
	mutex_t *const mutex = ctx->mutex;
	cond_t
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
#ifndef API_WIN
	debug("%s started.\n", func);
#endif
	mutex_lock(mutex);
#ifdef API_WIN
//debug("Callback %i\n", message_type);
	switch (message_type)
	{
		case MIM_OPEN:
#endif
	*(msg->len = len1) = 0;
	cond_broadcast(cond_inp2ctl);
	debug("%s ready.\n", func);
#ifdef API_WIN
		case MIM_DATA:
		case MIM_LONGDATA:
		case MIM_MOREDATA:
			break;
		default:
			goto exit1;
	}
#endif
	for (;;)
	{
		unsigned char *ptr = buf1;
#ifndef API_WIN
		const fid_t *const fid = ctx->fid;
		ssize_t left = 1;
#endif
#ifdef API_WIN
		if (*len1)
			goto exit1;
		switch (message_type)
		{
			case MIM_DATA:
			{
				intptr_t val = (intptr_t)param1;
				switch (val & 0xff)
				{
					case 0xb0:
						*ptr++ = val & 0xff;
						val >>= 8;
						*ptr++ = val & 0xff;
						val >>= 8;
						*ptr++ = val & 0xff;
						break;
					case 0xc0:
						*ptr++ = val & 0xff;
						val >>= 8;
						*ptr++ = val & 0xff;
					default:
						break;
				}
			}
			default:
				break;
		}
#else
		while (*running && *len1)
		{
			if (cond_wait(cond_ctl2inp, mutex))
			{
				debug("Wait failed.\n");
				goto exit1;
			}
		}
		if (!*running)
			goto exit1;
		mutex_unlock(mutex);
		do
		{
			ssize_t rcvd;
			if ((rcvd = read(*fid, ptr, left)) < 0)
			{
				debug("Failed to read data.\n");
				goto exit0;
			}
			if (left == 1)
				tic_get(tic1);
			ptr += rcvd;
			if ((left = parse_input(buf1, ptr - buf1)) < 0)
			{
				debug("Received unsupported message.\n");
				left = 1;
			}
		} while (left);
		mutex_lock(mutex);
		if (!*running)
			goto exit1;
#endif
		if ((*(msg->len = len1) = (ptr - (msg->buf = buf1))))
		{
			msg->tic = tic1;
//debug_msg(func, msg);
			cond_signal(cond_inp2ctl);
			swap_ptr(tic1, tic2);
			swap_ptr(buf1, buf2);
			swap_ptr(len1, len2);
		}
#ifdef API_WIN
		break;
#endif
	}
#ifndef API_WIN
	goto exit1;
exit0:
	mutex_lock(mutex);
#endif
exit1:
#ifdef API_WIN
	switch (message_type)
	{
		case MIM_CLOSE:
#endif
	*running = 0;
	cond_broadcast(cond_rst);
#ifdef API_WIN
		default:
			break;
	}
#endif
	mutex_unlock(mutex);
#ifndef API_WIN
	debug("%s exit.\n", func);
	return 0;
#endif
}

#ifndef API_WIN

static ssize_t parse_input(const unsigned char *const buf, const size_t len)
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

#endif

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
	mutex_t *const mutex = ctx->mutex;
	cond_t
		*const cond_rst = ctx->cond_rst,
		*const cond_out2ctl = ctx->cond_ctl,
		*const cond_ctl2out = ctx->cond_dev;
	unsigned *const running = ctx->running;
	midi_message_t *const msg = ctx->msg;
	debug("%s started.\n", func);
	mutex_lock(mutex);
	for (;;)
	{
		if (!*running)
			goto exit1;
		if (msg->len)
			break;
		if (cond_wait(cond_ctl2out, mutex))
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
			const fid_t *const fid = ctx->fid;
#ifdef API_WIN
			union { unsigned long word; unsigned char data[4]; } message;
			unsigned i;
			for (i = 0; (i < *msg->len) && (i < sizeof(message.data)/sizeof(*message.data)); i++)
				message.data[i] = msg->buf[i];
//debug("0x%08x\n", (unsigned)message.word);
#endif
//debug_msg(func, msg);
			mutex_unlock(mutex);
#if 1
#	ifdef API_WIN
			if (midiOutShortMsg(fid->out, message.word) != MMSYSERR_NOERROR)
#	else
			if (write(*fid, msg->buf, *msg->len) < 0)
#	endif
			{
				debug("Failed to write data.\n");
				goto exit0;
			}
#else
debug_msg("Not writing ", msg);
#endif
			mutex_lock(mutex);
			*msg->len = 0;
			cond_signal(cond_out2ctl);
		}
		if (cond_wait(cond_ctl2out, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	} while (*running);
	goto exit1;
exit0:
	mutex_lock(mutex);
exit1:
	*running = 0;
	cond_broadcast(cond_rst);
	mutex_unlock(mutex);
	debug("%s exit.\n", func);
	return 0;
}

static void *control(void *const context)
{
	thread_context_control_t *const ctx = (thread_context_control_t *)context;
	mutex_t *const mutex = ctx->mutex;
	cond_t
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
	mutex_lock(mutex);
	*(msg_ctl2fbv->len = len1_fbv) = 0;
	cond_broadcast(cond_fbv_out);
	*(msg_ctl2pod->len = len1_pod) = 0;
	cond_broadcast(cond_pod_out);
	//wait input threads
	for (;;)
	{
		if (!*running)
			goto exit1;
		if (msg_fbv2ctl->len && msg_pod2ctl->len)
			break;
		if (cond_wait(cond_ctl, mutex))
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
				cond_signal(cond_pod_out);
				swap_ptr(buf1_pod, buf2_pod);
				swap_ptr(len1_pod, len2_pod);
			}
			*msg_fbv2ctl->len = 0;
			cond_signal(cond_fbv_inp);
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
				cond_signal(cond_fbv_out);
				swap_ptr(buf1_fbv, buf2_fbv);
				swap_ptr(len1_fbv, len2_fbv);
			}
			*msg_pod2ctl->len = 0;
			cond_signal(cond_pod_inp);
		}
		if (cond_wait(cond_ctl, mutex))
		{
			debug("Wait failed.\n");
			goto exit1;
		}
	} while (*running);
exit1:
	*running = 0;
	cond_signal(cond_rst);
	mutex_unlock(mutex);
	debug("%s exit.\n", __FUNCTION__);
	return 0;
}
