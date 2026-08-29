#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "fs.h"


#define KIM_VERSION_MAJOR 0
#define KIM_VERSION_MINOR 0
#define KIM_VERSION_PATCH 0

#define FILEPATH_ROOT "/"
#define FILEPATH_NULL "/dev/null"
#define FILEPATH_FUSE "/dev/fuse"
#define MAX_PAGES 64u

#define PROTOCOL_VERSION_MAJOR 7
#define PROTOCOL_VERSION_MINOR 39

enum {
  LOG_QUIET   = 0,
  LOG_NORMAL  = 1,
  LOG_VERBOSE = 2
} log_level = LOG_NORMAL;
int fd = -1;
int fuse_fd = -1;
struct kim_fs_runtime runtime = { .initialized = false };

void cleanup(void) {
  if (runtime.initialized)
    if (kim_fs_flush_all(&runtime) == -1)
      if (log_level >= LOG_NORMAL) perror("kim: kim_fs_flush_all");

  if (fd != -1)
    if (close(fd) == -1)
      if (log_level >= LOG_NORMAL) perror("kim: close");

  if (fuse_fd != -1)
    if (close(fuse_fd) == -1)
      if (log_level >= LOG_NORMAL) perror("kim: close");
}

void signal_handler(int sig) {
  (void) sig;

  exit(EXIT_FAILURE);
}

void usage(int argc, char** argv) {
  (void) argc;

  printf(
      "usage: %s [<options>...] <command> [<args>...]\n"
      "       %s [<options>...] <filepath> <mountpoint>\n"
      "options:\n"
      "  --version -V  display version\n"
      "  --help        display this help and exit\n"
      "  --verbose -v  verbose mode\n"
      "  --quiet   -q  quiet mode\n"
      "  --[no-]daemon daemonize the server\n"
      "commands:\n"
      "  new           [<options>...] <filepath> <blocks> [block_size=%u]\n"
      "  mount         [<options>...] <filepath> <mountpoint>\n"
      "\n",
      argv[0], argv[0], 512);
}

int main(int argc, char** argv) {
  long PAGE_SIZE = sysconf(_SC_PAGESIZE);
  if (PAGE_SIZE == -1) {
    if (log_level >= LOG_NORMAL) perror("kim: sysconf _SC_PAGESIZE");
    exit(EXIT_FAILURE);
  }

  atexit(cleanup);
  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);

  // TODO: parse command line arguments
  log_level = LOG_VERBOSE;
  bool display_version = false;
  bool daemonize = false;
  char* filepath = "./img";
  char* mountpoint = "/mnt/kim";

  if (argc == 0) {
    errno = EIO;
    perror("kim");
    exit(EXIT_FAILURE);
  } else if (argc == 1) {
    fprintf(stderr, "kim: no input provided\n");
    usage(argc, argv);
    exit(EXIT_FAILURE);
  } else {
    unsigned i = 1;
    while (argv[i][0] == '-') {
      if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
        display_version = true;
      } else if (strcmp(argv[i], "--help")) {
        usage(argc, argv);
        exit(EXIT_SUCCESS);
      } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
        log_level = LOG_VERBOSE;
      } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
        log_level = LOG_QUIET;
      } else if (strcmp(argv[i], "--daemon")) {
        daemonize = true;
      } else if (strcmp(argv[i], "--no-daemon")) {
        daemonize = false;
      }

      i += 1;
    }
  }

  if (display_version) printf("kim %u.%u.%u\n", KIM_VERSION_MAJOR, KIM_VERSION_MINOR, KIM_VERSION_PATCH);

  fd = open(filepath, O_RDWR);
  if (fd == -1) {
    if (log_level >= LOG_NORMAL) {
      unsigned long size = (strlen(filepath) + 6) * sizeof(char);
      char* buf = malloc(size);
      snprintf(buf, size, "kim: open %s", filepath);
      perror(buf);
      free(buf);
    }
    exit(EXIT_FAILURE);
  }
  if (kim_open_fs(&runtime, fd) == -1) {
    if (log_level >= LOG_NORMAL) {
      perror("kim: kim_open_fs");
    }
    exit(EXIT_FAILURE);
  }

  fuse_fd = open(FILEPATH_FUSE, O_RDWR);
  if (fuse_fd == -1) {
    if (log_level >= LOG_NORMAL) perror("kim: open " FILEPATH_FUSE);
    exit(EXIT_FAILURE);
  }

  char options[128];
  snprintf(options, sizeof(options), "fd=%d,rootmode=040777,user_id=%ju,group_id=%ju,default_permissions,allow_other", fuse_fd, (uintmax_t) getuid(), (uintmax_t) getgid());
  if (mount(filepath, mountpoint, "fuse.kim", 0, options) == -1) { // TODO: reduce filepath
    if (log_level >= LOG_NORMAL) perror("kim: mount");
    exit(EXIT_FAILURE);
  }

  if (daemonize) { // TODO: log to a temp file
    pid_t pid = fork();
    if (pid < (pid_t) 0) {
      if (log_level >= LOG_NORMAL) perror("kim: fork");
      exit(EXIT_FAILURE);
    }
    if (pid > (pid_t) 0) {
      exit(EXIT_SUCCESS);
    }

    if (setsid() == (pid_t) -1) {
      if (log_level >= LOG_NORMAL) perror("kim: setsid");
      exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < (pid_t) 0) {
      if (log_level >= LOG_NORMAL) perror("kim: fork");
      exit(EXIT_FAILURE);
    }
    if (pid > (pid_t) 0) {
      if (log_level >= LOG_VERBOSE) printf("kim: daemonized successfully. daemon PID: %jd\n", (intmax_t) pid);
      exit(EXIT_SUCCESS);
    }

    if (chdir(FILEPATH_ROOT) == -1) {
      if (log_level >= LOG_NORMAL) perror("kim: chdir " FILEPATH_ROOT);
      exit(EXIT_FAILURE);
    }

    int null_fd = open(FILEPATH_NULL, O_RDWR);
    if (null_fd == -1) {
      if (log_level >= LOG_NORMAL) perror("kim: open " FILEPATH_NULL);
      exit(EXIT_FAILURE);
    }
    if    (dup2(null_fd, STDIN_FILENO)  == -1
        || dup2(null_fd, STDOUT_FILENO) == -1
        || dup2(null_fd, STDERR_FILENO) == -1) {
      if (log_level >= LOG_NORMAL) perror("kim: dup2");
      exit(EXIT_FAILURE);
    }
    if (null_fd > 2) {
      if (close(null_fd) == -1) {
        if (log_level >= LOG_NORMAL) perror("kim: close");
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
      if (log_level >= LOG_NORMAL) perror("kim: read");
      exit(EXIT_FAILURE);
    }

    struct fuse_in_header in_header = *(struct fuse_in_header*) buf;
    void* payload = &buf[sizeof(struct fuse_in_header)];

    if (log_level >= LOG_VERBOSE) printf("kim: received %s (%u) from the kernel\n", fuse_opcode_enum_str[in_header.opcode], in_header.opcode);
    if (in_header.opcode == FUSE_INIT) {
      struct fuse_init_in init_in = *(struct fuse_init_in*) payload;
      if (log_level >= LOG_VERBOSE) printf("kim: kernel's FUSE protocol version is %u.%u (supported is %u.%u)\n", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
      if (init_in.major < PROTOCOL_VERSION_MAJOR) {
        if (log_level >= LOG_NORMAL) fprintf(stderr, "kim: kernel's FUSE protocol version is too old (%u.%u < %u.%u)", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
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
          if (log_level >= LOG_NORMAL) perror("kim: writev");
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
          if (log_level >= LOG_NORMAL) perror("kim: writev");
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
        if (log_level >= LOG_NORMAL) perror("kim: writev");
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
            if (log_level >= LOG_NORMAL) perror("kim: writev");
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
            if (log_level >= LOG_NORMAL) perror("kim: writev");
            exit(EXIT_FAILURE);
          }
        }
    }
  }

  return EXIT_SUCCESS;
}

