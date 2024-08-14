
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ev.h>

#include "log.h"

#include "pingu_adm.h"
#include "pingu_conf.h"
#include "pingu_host.h"
#include "pingu_iface.h"
#include "pingu_netlink.h"

#ifndef DEFAULT_CONFIG
#define DEFAULT_CONFIG "/etc/pingu/pingu.conf"
#endif

#ifndef DEFAULT_PIDFILE
#define DEFAULT_PIDFILE "/var/run/pingu/pingu.pid"
#endif

/* libev 3 compat */
#if EV_VERSION_MAJOR < 4
#define EVBREAK_ALL EVUNLOOP_ALL
#define ev_run ev_loop
#define ev_break ev_unloop
#endif

int pingu_verbose = 0, pid_file_fd = 0, pingu_daemonize = 0;
char *pid_file = DEFAULT_PIDFILE;

static void print_version(const char *program)
{
	printf("%s " PINGU_VERSION "\n", program);
}

int usage(const char *program)
{
	print_version(program);
	fprintf(stderr, "usage: %s [-dhiv] [-a ADMSOCKET] [-c CONFIG] [-p PIDFILE]\n"
		"\n"
		"options:\n"
		" -a  Use administration socket ADMSOCKET (default is "
			DEFAULT_ADM_SOCKET ")\n"
		" -c  Read configuration from CONFIG (default is "
			DEFAULT_CONFIG ")\n"
		" -d  Fork to background (damonize)\n"
		" -h  Show this help\n"
		" -p  Use PIDFILE as pidfile (default is "
			DEFAULT_PIDFILE ")\n"
		" -V  Print version and exit\n"
		" -v  Run in verbose mode. Will log debug messages\n"
		"\n",
		program);
	return 1;
}

static void remove_pid_file(void)
{
	if (pid_file_fd != 0) {
		close(pid_file_fd);
		pid_file_fd = 0;
		remove(pid_file);
	}
}

static int daemonize(void)
{
	char tmp[16];
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		exit(0);

	if (setsid() < 0)
		return -1;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		exit(0);

	if (chdir("/") < 0)
		return -1;

	pid_file_fd = open(pid_file, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (pid_file_fd < 0) {
		log_error("Unable to open %s: %s.", pid_file, strerror(errno));
		return -1;
	}

	if (flock(pid_file_fd, LOCK_EX | LOCK_NB) < 0) {
		log_error("Unable to lock pid file (already running?).");
		close(pid_file_fd);
		pid_file_fd = 0;
		return -1;
	}

	ftruncate(pid_file_fd, 0);
	write(pid_file_fd, tmp, sprintf(tmp, "%d\n", getpid()));
	atexit(remove_pid_file);

	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);

	umask(0);

	return 0;
}

static void sigint_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
	ev_break(loop, EVBREAK_ALL);
}

static pid_t get_running_pid(void) {
	size_t n;
	int fd;
	char buf[32] = "/proc/";
	fd = open(pid_file, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, &buf[6], sizeof(buf)-7);
	close(fd);
	if (n < sizeof(buf)-6)
		buf[5+n] = '\0'; /* chomp newline */
	if (access(buf, R_OK) == 0)
		return atoi(&buf[6]);
	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	const char *config_file = DEFAULT_CONFIG;
	const char *adm_socket = DEFAULT_ADM_SOCKET;
	int verbose = 0;
	static struct ev_loop *loop;
	static struct ev_signal signal_watcher;
	pid_t pid;

	while ((c = getopt(argc, argv, "a:c:dhp:Vv")) != -1) {
		switch (c) {
		case 'a':
			adm_socket = optarg;
			break;
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			pingu_daemonize++;
			break;
		case 'h':
			return usage(basename(argv[0]));
		case 'p':
			pid_file = optarg;
			break;
		case 'V':
			print_version(basename(argv[0]));
			return 0;
		case 'v':
			verbose++;
			break;
		}
	}

	argc -= optind;
	argv += optind;

	log_init("pingu", verbose);

	pid = get_running_pid();
	if (pid)
		errx(1, "appears to be running already (pid %i)", pid);

	loop = ev_default_loop(0);

	if (pingu_conf_parse(config_file) < 0)
		return 1;

	if (pingu_iface_init(loop) < 0)
		return 1;

	if (pingu_host_init(loop) < 0)
		return 1;

	if (pingu_adm_init(loop, adm_socket) < 0)
		return 1;

	if (pingu_daemonize) {
		if (daemonize() == -1)
			return 1;
	}

	kernel_init(loop);
	ev_signal_init(&signal_watcher, sigint_cb, SIGINT);
	ev_signal_start(loop, &signal_watcher);

	ev_run(loop, 0);
	log_info("Shutting down");
	pingu_iface_cleanup(loop);
	pingu_host_cleanup();
	kernel_close();
	ev_loop_destroy(loop);
	return 0;
}

