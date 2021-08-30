/*
 * pslash-systemd - systemd integration for psplash
 *
 * Copyright (c) 2020 Toradex
 *
 * Author: Stefan Agner <stefan.agner@toradex.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include "psplash.h"

#include <string.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>

#define RESULT_BUFFER_SIZE 2048
#define PSPLASH_UPDATE_USEC 1000000
// maximum wait multiplier of average delay to wait for next psplash-systemd
#define PSPLASH_MAX_WAIT_MULTIPLIER 12
#define RUN_PSPLASH_SYSTEMD "/run/psplash-systemd"
#define RUN_PSPLASH_SYSTEMD_FLAG "/run/psplash-systemd.running"
#define SYSROOT_DIR "/run/systemd/system"
#define PSPLASH_SYSTEMD_SERVICE "psplash-systemd.service"
#define BASIC_TARGET_WANTS "basic.target.wants"
const char *FIRST_TIME_BOOT_COMMAND =
    "MSG Setting up. This takes up to 15 minutes.";

typedef uint64_t usec_t;

static int pipe_fd;
static int progress_base;
static int progress_weight;
static int progress_weighted;
static int progress_runs;
static FILE *logfile;
static FILE *comm_file;

// debug function
#define print_log fprintf
// #define print_log do_print_log

// custom logging to the file + stdour/stderr
void do_print_log(FILE *stream, const char *fmt, ...) {
	time_t now;
	struct tm *tm;
	va_list args;
	char *str;

	va_start (args, fmt);
	if (vasprintf(&str, fmt, args) < 0)
		return;

	if (logfile) {
		now =time(NULL);
		tm = localtime (&now);
		va_start (args, fmt);
		fprintf(logfile, "%02d:%02d:%02d %s", tm->tm_hour, tm->tm_min, tm->tm_sec, str);
	}
	fprintf(stream, "%s", str);
	free(str);
}

int check_if_seed_is_loaded() {
	const char * const host = "/run/snapd.socket";
	const char * const message = "GET /v2/snaps/system/conf?keys=seed.loaded HTTP/1.0\r\n\r\n";
    static char output[1024];


	const int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
      return -1;
	}

	struct sockaddr_un serv_addr = {0};
	serv_addr.sun_family = AF_UNIX;
	strncpy(serv_addr.sun_path, host, sizeof(serv_addr.sun_path) - 1);
	int ret = connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
	if (ret == -1) {
		close(sockfd);
	    return -1;
	}

	const int total = strlen(message);
	int bytes, received;
	int sent = 0;
	do {
      bytes = write(sockfd, message + sent, total - sent);
      if (bytes < 0) {
        close(sockfd);
        return -1;
      }

      if (bytes == 0)
        break;
      sent += bytes;
	} while (sent < total);

    char response[RESULT_BUFFER_SIZE];
	memset(response, 0, RESULT_BUFFER_SIZE);

	received = 0;
	const ssize_t bytes_read = recv(sockfd, response, RESULT_BUFFER_SIZE, MSG_CMSG_CLOEXEC);
	if (bytes_read <= 0) {
      close(sockfd);
      return -1;
	}

	return strstr(response, "\"seed.loaded\":true") != NULL ? 1 : 0;
}

int check_if_screenly_client_is_installed() {
	const char * const host = "/run/snapd.socket";
	const char * const message = "GET /v2/snaps HTTP/1.0\r\n\r\n";

	const int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
	    print_log(stdout, "Error opening socket: ");
	    return -1;
	}

	struct sockaddr_un serv_addr = {0};
	serv_addr.sun_family = AF_UNIX;
	strncpy(serv_addr.sun_path, host, sizeof(serv_addr.sun_path) - 1);
	int ret = connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
	if (ret == -1) {
		close(sockfd);
	    print_log(stdout, "Error connecting: ");
	    return -1;
	}

	const int total = strlen(message);
	int bytes, received;
	int sent = 0;
	do {
	    bytes = write(sockfd, message + sent, total - sent);
	    if (bytes < 0) {
	        perror("Error writing message to socket");
	        close(sockfd);
	        return -1;
	    }

	    if (bytes == 0)
	        break;
	    sent += bytes;
	} while (sent < total);

	char *response = malloc(sizeof(char) * 50000);
	memset(response,0,sizeof(char) * 50000);
	received = 0;
	const ssize_t bytes_read= recv(sockfd, response, 50000, MSG_CMSG_CLOEXEC);
	if (bytes_read <= 0) {
	    print_log(stdout, "Error receiving an answer");
	    free(response);
	    close(sockfd);
	    return -1;
	}

	if(strstr(response, "screenly-client") != NULL) {
		free(response);
	    print_log(stdout, "Found screenly-client snap.\n");
	    return 1;
	} else {
		free(response);
		return -1;
	}
}

void display_first_time_boot_message(int pipe_fd) {
  print_log(stdout, "The first phase of the first boot detected\n");
  write(pipe_fd, FIRST_TIME_BOOT_COMMAND, strlen(FIRST_TIME_BOOT_COMMAND) + 1);
}

bool is_snapd_mode_install() {
  static const char* first_boot_marker = "snapd_recovery_mode=install";
  static const unsigned int buffer_size = 1024;

  FILE *cmdline_file = fopen("/proc/cmdline", "r");
  if (cmdline_file == NULL) {
    print_log(stdout, "Cannot open cmdline file\n");
    return 0;
  }

  char cmdline[buffer_size];
  char* result = fgets(cmdline, buffer_size, cmdline_file);
  fclose(cmdline_file);

  if (result == NULL) {
    print_log(stdout, "Cannot read cmdline file\n");
    return 0;
  }

  return strstr(cmdline, first_boot_marker) != NULL;
}


int get_progress()
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	static double current_progress = 0;
    static bool first_boot_messaged_displayed = 0;

    double progress = 0;
	sd_bus *bus = NULL;
	int r;
	char buffer[64];
	int len;

        /* Connect to the system bus */
	r = sd_bus_new(&bus);
	if (r < 0) {
		print_log(stderr, "Failed to get bus: %s\n", strerror(-r));
		goto finish;
	}

	r = sd_bus_set_address(bus, "unix:path=/run/systemd/private");
	if (r < 0) {
		print_log(stderr, "Failed to set bus address: %s\n", strerror(-r));
		goto finish;
	}

	r = sd_bus_start(bus);
	if (r < 0) {
		print_log(stderr, "Failed to connect to systemd private bus: %s\n", strerror(-r));
		goto finish;
        }

        /* Issue the method call and store the respons message in m */
	r = sd_bus_get_property_trivial(bus,
		"org.freedesktop.systemd1",           /* service to contact */
		"/org/freedesktop/systemd1",          /* object path */
		"org.freedesktop.systemd1.Manager",   /* interface name */
		"Progress",                           /* method name */
		&error,                               /* object to return error in */
		'd',                                  /* return message on success */
		&progress);                           /* value */
	if (r < 0) {
		print_log(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		goto finish;
	}

    if (!first_boot_messaged_displayed) {
      if (is_snapd_mode_install()) {
        display_first_time_boot_message(pipe_fd);
        first_boot_messaged_displayed = 1;
      } else {
        int res = check_if_seed_is_loaded();

        if (res == 0) {
          display_first_time_boot_message(pipe_fd);
          first_boot_messaged_displayed = 1;
        } else if (res == 1) {
          first_boot_messaged_displayed = 1;
        }
      }
    }
	/*
	 * Systemd's progress seems go backwards at times. Prevent that
	 * progress bar on psplash goes backward by just communicating the
	 * highest observed progress so far.
	 */
	if (current_progress < progress)
		current_progress = progress;

	progress_weighted = progress_base + current_progress * progress_weight;
	// print_log(stdout, "Progress: %f, weighted: %d\n", progress, progress_weighted);
	/* len = snprintf(buffer, 20, "PROGRESS %d", progress_weighted); */
	/* len = write(pipe_fd, buffer, len + 1); */
	int systemd_loaded = -1;
	if (progress == 1.0) {
		print_log(stdout, "Systemd reported progress of 1.0, quit monitoring loop\n");
		// send QUIT command only if overal proress is 100
		if (progress_weighted == 100) {
			print_log(stdout, "systemd is fully booted\n");
		}
	} else {
		goto finish;
	}

	if (check_if_screenly_client_is_installed() != -1) {
		print_log(stdout, "system is fully booted and screenly-client is installed\n");
		len = write(pipe_fd, "QUIT", 5);
		r = -1;
	}

finish:
	sd_bus_error_free(&error);
	sd_bus_unref(bus);
	return r;
}

int psplash_handler(sd_event_source *s,
			uint64_t usec,
			void *userdata)
{
	sd_event *event = userdata;
	int r;

	progress_runs++;
	r = get_progress();
	if (r < 0)
		goto err;

	r = sd_event_source_set_time(s, usec + PSPLASH_UPDATE_USEC);
	if (r < 0)
		goto err;

	return 0;
err:
	sd_event_exit(event, EXIT_FAILURE);

	return r;
}

/**
 * we finished initrd portion of the boot
 * keep pipe file open, so main psplash process survives root pivot
 * in the mean time emulate "gentle" progress and keep watching for
 * signal from rootfs psplash-systemd
 * we wait max 10 seconds
 * it takes ~3+ seconds for systemd start post root pivot and start
 * next psplash-systemd instance, at which point progress is ~0.5
 */
void wait_for_next_monitor() {
	char buffer[20];
	int len;
	int i;
	int progress_increments;
	int average_delay;

	// this is estimate: but on average it stake 1.3 times to run through initrd section
	// as it takes to pivot root and next monitor to start
	average_delay = progress_runs / 1.3;
	// increment progress by:
	// average progress of next section by the time next monitor starts ~0.3 times
	// progress weight of next section is 100 - weight of this sercion (we have two sections)
	// divide that number by average delay for next monitor to start
	progress_increments = 0.3 * (100 - progress_weight) / average_delay;

	for (i=0 ; i < PSPLASH_MAX_WAIT_MULTIPLIER * average_delay; ++i) {
		// check if next section started by reading from comm_file file
		if (comm_file){
			rewind(comm_file); // always read from the begining
			if (fgets(buffer,sizeof(buffer), comm_file) !=NULL) {
				// file is empty, moment read succeeds, other side is running
				fclose(comm_file);
				comm_file = NULL;
				return;
			}
		}
                progress_weighted += progress_increments;
		// print_log(stdout, "Progress:           weighted: %d\n", progress_weighted);
		/* len = snprintf(buffer, 20, "PROGRESS %d", progress_weighted); */
		/* len = write(pipe_fd, buffer, len + 1); */
		sleep(1);
	}
}

int do_main()
{
	sd_event *event;
	sd_event_source *event_source = NULL;
	int r;
	sigset_t ss;
	usec_t time_now;
	char *rundir;

	print_log(stdout, "Entering main loop of systemd monitor.\n");
	/* Open pipe for psplash */
	rundir = getenv("PSPLASH_FIFO_DIR");

	if (!rundir)
		rundir = "/run";

	r = chdir(rundir);
	if (r < 0 ) {
		r = -errno;
		goto finish;
	}

	if ((pipe_fd = open (PSPLASH_FIFO,O_WRONLY|O_NONBLOCK)) == -1) {
		print_log(stderr, "Error unable to open fifo: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// update progress before we register for the event loop
	get_progress();

	r = sd_event_default(&event);
	if (r < 0)
		goto finish;

	if (sigemptyset(&ss) < 0 ||
	    sigaddset(&ss, SIGTERM) < 0 ||
	    sigaddset(&ss, SIGINT) < 0) {
		r = -errno;
		goto finish;
	}

	/* Block SIGTERM first, so that the event loop can handle it */
	if (sigprocmask(SIG_BLOCK, &ss, NULL) < 0) {
		r = -errno;
		print_log(stderr, "Failed to set SIG_BLOCK mask: %s\n", strerror(errno));
		goto finish;
	}

	/* Let's make use of the default handler and "floating" reference
	 * features of sd_event_add_signal() */
	r = sd_event_add_signal(event, NULL, SIGTERM, NULL, NULL);
	if (r < 0) {
		print_log(stdout, "sd_event add SIGTERM signal error: %s\n", strerror(errno));
		goto finish;
	}

	r = sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);
	if (r < 0) {
		print_log(stdout, "sd_event add SIGINT signal error: %s\n", strerror(errno));
		goto finish;
	}

	r = sd_event_now(event, CLOCK_MONOTONIC, &time_now);
	if (r < 0) {
		print_log(stdout, "sd_event_now error: %s\n", strerror(errno));
		goto finish;
	}

	r = sd_event_add_time(event, &event_source, CLOCK_MONOTONIC,
			      time_now, 0, psplash_handler, event);
	if (r < 0)
		goto finish;

	r = sd_event_source_set_enabled(event_source, SD_EVENT_ON);
	if (r < 0)
		goto finish;

	r = sd_event_loop(event);
finish:
	sd_event_source_disable_unref(event_source);
	event = sd_event_unref(event);
	if(progress_weighted != 100) {
		wait_for_next_monitor();
	}
	// it's safe to close the pipe now
        close(pipe_fd);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}


int copy_itself(const char *source, const char *target) {
	int sf;
	int tf;
	int r;
	unsigned char buffer[4096];

        sf = open(source, O_RDONLY);
	if (sf<0) {
		print_log(stderr, "open %s failed: %s\n", source, strerror(errno));
		return -errno;
	}

	tf = open(target,  O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0755);
	if (tf<0) {
		print_log(stderr, "open %s failed: %s\n", target, strerror(errno));
		return -errno;
	}

	while(1) {
		r = read(sf, buffer, 4096);
		if (r == -1) {
			print_log(stderr, "Error reading from %s : %s\n", source, strerror(errno));
			goto finish;
		}
		if (r == 0) {
			break;
		}
		r = write(tf, buffer, r);
		if (r == -1) {
			print_log(stderr, "Error writing to %s : %s\n", target, strerror(errno));
			goto finish;
		}
	}

finish:
	close(sf);
	close(tf);
	return r;
}

// sets up service file under /sysroot/run/systemd/system/
void setup_service() {
	FILE *new_service;
	int r;
	new_service = fopen(SYSROOT_DIR"/"PSPLASH_SYSTEMD_SERVICE, "a+");
	if (new_service) {
		print_log(stdout, "Empty service file created\n");
		fprintf(new_service, "[Unit]\n");
		fprintf(new_service, "Description=Start psplash-systemd progress communication helper\n");
		fprintf(new_service, "DefaultDependencies=no\n");
		fprintf(new_service, "RequiresMountsFor=/run\n\n");
		fprintf(new_service, "[Service]\n");
		fprintf(new_service, "ExecStart=/run/psplash-systemd\n");
		fprintf(new_service, "RemainAfterExit=yes\n");
		fprintf(new_service, "Environment=\"PSPLASH_BASE=35\"\n");
		fprintf(new_service, "Environment=\"PSPLASH_WEIGHT=65\"\n\n");
		fprintf(new_service, "[Install]\n");
		fprintf(new_service, "WantedBy=basic.target\n");
		fclose(new_service);
		// creare sym link to enable the new service
		r = mkdir(SYSROOT_DIR"/"BASIC_TARGET_WANTS, 0755);
		if (r == -1)
			print_log(stderr,"Failed to create %s dir to enable new servive: %s\n", BASIC_TARGET_WANTS, strerror(errno));
		r = symlink("../"PSPLASH_SYSTEMD_SERVICE, SYSROOT_DIR"/"BASIC_TARGET_WANTS"/"PSPLASH_SYSTEMD_SERVICE);
		if (r == -1)
			print_log(stderr,"Failed to enable psplash-systemd service: %s\n", strerror(errno));
	} else {
		print_log(stderr,"Failed to create new service file: %s\n", strerror(errno));
	}
}


/**
 * Two modes of operation are avaiabe
 * - default: monitor systemd boot progress
 *            no parameter is passed
 * - setup: prepare for root pivot
 *          'setup' parameter is passed
 *         - copies psplash-systemd executable to /run
 *         - created new systemd service file under /sysroot/run/systemd/system/
 *           and enables it
 *         This operation can be only called when system is ready for it
 *         e.g. /sysroot/run/systemd/system/ is created
 */
int main(int argc, char *argv[]) {
	int r;
	const char *env;
	int cleanup = 0;

	env = getenv("PSPLASH_LOG");
	if (env) {
		logfile = fopen(env, "a+");
	}
	if (argc == 1 ) {
		// if we run from RUN_PSPLASH_SYSTEMD we should signal we started
		// by writing to the RUN_PSPLASH_SYSTEMD_FLAG file
		comm_file = fopen(RUN_PSPLASH_SYSTEMD_FLAG, "a+");
		if(comm_file) {
			if (!strncmp(argv[0], RUN_PSPLASH_SYSTEMD, strlen(RUN_PSPLASH_SYSTEMD))) {
				// write/flush and close the file, to make sure
				// other process has something to read
				fprintf(comm_file, "Runnin...");
				fflush(comm_file);
				fclose(comm_file);
				comm_file = NULL;
				cleanup = 1;
			}
		}
	} else if (!strncmp(argv[1], "setup", strlen("setup"))) {
		// copy itself to the /run directory
		r = copy_itself(argv[0], RUN_PSPLASH_SYSTEMD);
		if (r <0)
			print_log(stderr, "Failed to copy itself to /run dir: %s\n", strerror(-r));
		// create new service unit
		setup_service();
		goto finish;
	}

  env = getenv("PSPLASH_BASE");
  if (env) {
    progress_base = atoi(env);
  }
  env = getenv("PSPLASH_WEIGHT");
  if (env) {
    progress_weight = atoi(env);
  }
  if (!progress_weight) {
    // if weight is 0 -> not defined -> default 100
    // if base is not defined, it's correct to be 0
    progress_weight = 100;
  }

  r = do_main();

finish:
  if (logfile)
    fclose(logfile);

  if (comm_file)
    fclose(comm_file);

  if (cleanup) {
    // try to delete "flag" file, ignore error
    remove(RUN_PSPLASH_SYSTEMD_FLAG);
  }
  return r;
}
