#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"


#define FILEPATH_ROOT "/"
#define FILEPATH_NULL "/dev/null"
#define FILEPATH_FUSE "/dev/fuse"
#define DEFAULT_MOUNT_POINT "/mnt/kim"
#define MAX_PAGES 64u

#define PROTOCOL_VERSION_MAJOR 7
#define PROTOCOL_VERSION_MINOR 39

int fuse_fd = -1;
enum {
  LOG_QUIET   = 0,
  LOG_NORMAL  = 1,
  LOG_VERBOSE = 2
} log_level = LOG_NORMAL;

void cleanup(void) {
  if (fuse_fd != -1)
    if (close(fuse_fd) == -1)
      if (log_level >= LOG_NORMAL) perror("close");

  // TODO: flush and save fs
}

void signal_handler(int sig) {
  (void) sig;

  exit(EXIT_FAILURE);
}

int main(void) {
  long PAGE_SIZE = sysconf(_SC_PAGESIZE);
  if (PAGE_SIZE == -1) {
    if (log_level >= LOG_NORMAL) perror("sysconf _SC_PAGESIZE");
    exit(EXIT_FAILURE);
  }

  atexit(cleanup);
  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);

  // TODO: parse command line arguments
  bool daemonize = true;
  log_level = LOG_VERBOSE;

  // TODO: initialize fs from file
  // lock backing storage file to prevent access

  fuse_fd = open(FILEPATH_FUSE, O_RDWR);
  if (fuse_fd == -1) {
    if (log_level >= LOG_NORMAL) perror("open " FILEPATH_FUSE);
    exit(EXIT_FAILURE);
  }

  char options[128];
  snprintf(options, sizeof(options), "fd=%d,rootmode=040777,user_id=%u,group_id=%u,default_permissions,allow_other", fuse_fd, getuid(), getgid());
  if (mount("NAME", DEFAULT_MOUNT_POINT, "fuse.kim", 0, options) == -1) { // TODO: replace NAME with the name of the backing storage file
    if (log_level >= LOG_NORMAL) perror("mount");
    exit(EXIT_FAILURE);
  }

  if (daemonize) { // TODO: log to a temp file
    pid_t pid = fork();
    if (pid < 0) {
      if (log_level >= LOG_NORMAL) perror("fork");
      exit(EXIT_FAILURE);
    }
    if (pid > 0) {
      exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
      if (log_level >= LOG_NORMAL) perror("setsid");
      exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < 0) {
      if (log_level >= LOG_NORMAL) perror("fork");
      exit(EXIT_FAILURE);
    }
    if (pid > 0) {
      if (log_level >= LOG_VERBOSE) printf("daemonized successfully. daemon PID: %jd\n", (intmax_t) pid);
      exit(EXIT_SUCCESS);
    }

    if (chdir(FILEPATH_ROOT) == -1) {
      if (log_level >= LOG_NORMAL) perror("chdir " FILEPATH_ROOT);
      exit(EXIT_FAILURE);
    }

    int null_fd = open(FILEPATH_NULL, O_RDWR);
    if (null_fd == -1) {
      if (log_level >= LOG_NORMAL) perror("open " FILEPATH_NULL);
      exit(EXIT_FAILURE);
    }
    if    (dup2(null_fd, STDIN_FILENO)  == -1
        || dup2(null_fd, STDOUT_FILENO) == -1
        || dup2(null_fd, STDERR_FILENO) == -1) {
      if (log_level >= LOG_NORMAL) perror("dup2");
      exit(EXIT_FAILURE);
    }
    if (null_fd > 2) {
      if (close(null_fd) == -1) {
        if (log_level >= LOG_NORMAL) perror("close");
        exit(EXIT_FAILURE);
      }
    }
  }

  unsigned long buf_size = (MAX_PAGES + 1) * (unsigned long) PAGE_SIZE;
  char buf[buf_size];
  long count;
  bool running     = true;
  bool initialized = false;
  while (running) {
    count = read(fuse_fd, buf, buf_size);
    if (count == -1) {
      if (log_level >= LOG_NORMAL) perror("read");
      exit(EXIT_FAILURE);
    }

    struct fuse_in_header in_header = *(struct fuse_in_header*) buf;
    void* payload = &buf[sizeof(struct fuse_in_header)];

    if (log_level >= LOG_VERBOSE) printf("received %s (%u) from the kernel\n", fuse_opcode_enum_str[in_header.opcode], in_header.opcode);
    if (in_header.opcode == FUSE_INIT) {
      struct fuse_init_in init_in = *(struct fuse_init_in*) payload;
      if (log_level >= LOG_VERBOSE) printf("kernel's FUSE protocol version is %u.%u (supported is %u.%u)\n", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
      if (init_in.major < PROTOCOL_VERSION_MAJOR) {
        if (log_level >= LOG_NORMAL) fprintf(stderr, "error: kernel's FUSE protocol version is too old (%u.%u < %u.%u)", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
        exit(EXIT_FAILURE);
      } else if (init_in.major > PROTOCOL_VERSION_MAJOR) {
        struct fuse_out_header out_header = (struct fuse_out_header) {
          .len = sizeof(struct fuse_out_header) + sizeof(uint32_t),
          .error = 0,
          .unique = in_header.unique
        };
        uint32_t major = PROTOCOL_VERSION_MAJOR;
        struct iovec iov[] = {
          { &out_header, sizeof(struct fuse_out_header) },
          { &major,      sizeof(uint32_t)               }
        };

        if (writev(fuse_fd, iov, 2) == -1) {
          if (log_level >= LOG_NORMAL) perror("writev");
          exit(EXIT_FAILURE);
        }
      } else {
        struct fuse_out_header out_header = (struct fuse_out_header) {
          .len = sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out),
          .error = 0,
          .unique = in_header.unique
        };
        struct fuse_init_out init_out = (struct fuse_init_out) {
          .major = PROTOCOL_VERSION_MAJOR,
          .minor = PROTOCOL_VERSION_MINOR,
          .max_readahead = init_in.max_readahead,
          .flags = 0,
          .max_background = 1,
          .congestion_threshold = 1,
          .max_write = MAX_PAGES * (unsigned) PAGE_SIZE,
          .time_gran = 1000000000, // 1s |  TODO: gran is not set for kim fs
          .max_pages = MAX_PAGES,
          .map_alignment = 0,
          .flags2 = 0,
        };
        struct iovec iov[] = {
          { &out_header, sizeof(struct fuse_out_header) },
          { &init_out,   sizeof(struct fuse_init_out)   }
        };

        if (writev(fuse_fd, iov, 2) == -1) {
          if (log_level >= LOG_NORMAL) perror("writev");
          exit(EXIT_FAILURE);
        }

        initialized = true;
      }
      continue;
    } else if (!initialized) {
      struct fuse_out_header out_header = (struct fuse_out_header) {
        .len = sizeof(struct fuse_out_header),
        .error = -EUNATCH,
        .unique = in_header.unique
      };

      struct iovec iov[] = {
        { &out_header, sizeof(struct fuse_out_header) }
      };

      if (writev(fuse_fd, iov, 1) == -1) {
        if (log_level >= LOG_NORMAL) perror("writev");
        exit(EXIT_FAILURE);
      }
    }
    switch (in_header.opcode) {
      case FUSE_DESTROY:
        {
          struct fuse_out_header out_header = (struct fuse_out_header) {
            .len = sizeof(struct fuse_out_header),
            .error = 0,
            .unique = in_header.unique
          };

          struct iovec iov[] = {
            { &out_header, sizeof(struct fuse_out_header) }
          };

          if (writev(fuse_fd, iov, 1) == -1) {
            if (log_level >= LOG_NORMAL) perror("writev");
          }

          running = false;
          break;
        }
      default:
        {
          struct fuse_out_header out_header = (struct fuse_out_header) {
            .len = sizeof(struct fuse_out_header),
            .error = -EOPNOTSUPP,
            .unique = in_header.unique
          };

          struct iovec iov[] = {
            { &out_header, sizeof(struct fuse_out_header) }
          };

          if (writev(fuse_fd, iov, 1) == -1) {
            if (log_level >= LOG_NORMAL) perror("writev");
            exit(EXIT_FAILURE);
          }
        }
    }
  }

  return EXIT_SUCCESS;
}

