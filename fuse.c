#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/fuse.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "fs.h"


#define KIM_VERSION_MAJOR 0u
#define KIM_VERSION_MINOR 0u
#define KIM_VERSION_PATCH 3u

#define FILEPATH_ROOT "/"
#define FILEPATH_NULL "/dev/null"
#define FILEPATH_FUSE "/dev/fuse"
#define MAX_PAGES 64u

#define PROTOCOL_VERSION_MAJOR 7u
#define PROTOCOL_VERSION_MINOR 39u

#if (FUSE_KERNEL_VERSION != PROTOCOL_VERSION_MAJOR)
#error "<linux/fuse.h> header version mismatch"
#endif

enum {
  LOG_QUIET   = 0,
  LOG_NORMAL  = 1,
  LOG_VERBOSE = 2
} log_level = LOG_NORMAL;
bool unmount = false;

int fd = -1;
int fuse_fd = -1;
struct kim_fs_runtime runtime = { .initialized = false };
char* mountpoint = NULL;
bool mounted = false;
char* executable_name;
long PAGE_SIZE = -1;

void cleanup(void) {
  if (runtime.initialized)
    if (kim_fs_flush_all(&runtime) == -1)
      if (log_level >= LOG_NORMAL)
        warn("kim_fs_flush_all");

  if (fd != -1)
    if (close(fd) == -1)
      if (log_level >= LOG_NORMAL)
        warn("close");

  if (fuse_fd != -1)
    if (close(fuse_fd) == -1)
      if (log_level >= LOG_NORMAL)
        warn("close");

  if (mounted)
    if (unmount)
      if (umount(mountpoint) == -1)
        if (log_level >= LOG_NORMAL)
          warn("umount");
}

void signal_handler(int sig) {
  if (log_level >=LOG_VERBOSE)
    switch (sig) {
      case SIGINT:
        fprintf(stderr, "\r%s: Interrupted by SIGINT\n", executable_name);
        break;
      case SIGTERM:
        fprintf(stderr, "\r%s: Interrupted by SIGTERM\n", executable_name);
        break;
      case SIGHUP:
        fprintf(stderr, "\r%s: Interrupted by SIGHUP\n", executable_name);
        break;
    }

  exit(EXIT_FAILURE);
}

int kim_fuse_new(char* filepath, unsigned long long blocks, unsigned long block_size) {
  fd = open(filepath, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd == -1) {
    if (log_level >= LOG_NORMAL)
      warn("open %s", filepath);
    errno = EIO;
    return -1;
  }

  if (kim_new_fs(fd, (uint64_t) blocks, (uint32_t) block_size) == -1) {
    if (log_level >= LOG_NORMAL)
      warn("kim_new_fs");
    errno = EIO;
    return -1;
  }

  return 0;
}

int kim_fuse_mount(char* filepath, char* mountpoint, bool daemonize) {
  fd = open(filepath, O_RDWR);
  if (fd == -1) {
    if (log_level >= LOG_NORMAL)
      warn("open %s", filepath);
    errno = EIO;
    return -1;
  }
  if (kim_open_fs(&runtime, fd) == -1) {
    if (log_level >= LOG_NORMAL)
      warn("kim_open_fs");
    errno = EIO;
    return -1;
  }

  fuse_fd = open(FILEPATH_FUSE, O_RDWR);
  if (fuse_fd == -1) {
    if (log_level >= LOG_NORMAL)
      warn("open " FILEPATH_FUSE);
    errno = EIO;
    return -1;
  }

  char options[128];
  snprintf(options, sizeof(options), "fd=%d,rootmode=040777,user_id=%ju,group_id=%ju,default_permissions,allow_other", fuse_fd, (uintmax_t) getuid(), (uintmax_t) getgid());
  if (mount(filepath, mountpoint, "fuse.kim", 0, options) == -1) { // TODO: simplify filepath
    if (log_level >= LOG_NORMAL)
      warn("mount");
    errno = EIO;
    return -1;
  }
  mounted = true;

  if (daemonize) { // TODO: log to a temp file
    pid_t pid = fork();
    if (pid < (pid_t) 0) {
      if (log_level >= LOG_NORMAL)
        warn("fork");
      errno = EIO;
      return -1;
    }
    if (pid > (pid_t) 0)
      exit(EXIT_SUCCESS);

    if (setsid() == (pid_t) -1) {
      if (log_level >= LOG_NORMAL)
        warn("setsid");
      errno = EIO;
      return -1;
    }

    pid = fork();
    if (pid < (pid_t) 0) {
      if (log_level >= LOG_NORMAL)
        warn("fork");
      errno = EIO;
      return -1;
    }
    if (pid > (pid_t) 0) {
      if (log_level >= LOG_VERBOSE)
        printf("%s: Daemonized successfully. Daemon PID: %jd\n", executable_name, (intmax_t) pid);
      exit(EXIT_SUCCESS);
    }

    if (chdir(FILEPATH_ROOT) == -1) {
      if (log_level >= LOG_NORMAL)
        warn("chdir " FILEPATH_ROOT);
      errno = EIO;
      return -1;
    }

    int null_fd = open(FILEPATH_NULL, O_RDWR);
    if (null_fd == -1) {
      if (log_level >= LOG_NORMAL)
        warn("open " FILEPATH_NULL);
      errno = EIO;
      return -1;
    }
    if    (dup2(null_fd, STDIN_FILENO)  == -1
        || dup2(null_fd, STDOUT_FILENO) == -1
        || dup2(null_fd, STDERR_FILENO) == -1) {
      if (log_level >= LOG_NORMAL)
        warn("dup2");
      errno = EIO;
      return -1;
    }
    if (null_fd > 2) {
      if (close(null_fd) == -1) {
        if (log_level >= LOG_NORMAL)
          warn("close");
        errno = EIO;
        return -1;
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
      if (log_level >= LOG_NORMAL)
        warn("read");
      errno = EIO;
      return -1;
    }

    struct fuse_in_header in_header = *(struct fuse_in_header*) buf;
    void* payload = &buf[sizeof(struct fuse_in_header)];

    if (log_level >= LOG_VERBOSE)
      printf("%s: Received %s (%u) from the kernel\n", executable_name, fuse_opcode_enum_str[in_header.opcode], in_header.opcode);
    if (in_header.opcode == FUSE_INIT) {
      struct fuse_init_in init_in = *(struct fuse_init_in*) payload;
      if (log_level >= LOG_VERBOSE)
        printf("%s: Kernel's FUSE protocol version is %u.%u (supported is %u.%u)\n", executable_name, init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
      if (init_in.major < PROTOCOL_VERSION_MAJOR) {
        if (log_level >= LOG_NORMAL)
          warnx("Kernel's FUSE protocol version is too old (%u.%u < %u.%u)", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
        errno = EPROTONOSUPPORT;
        return -1;
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
          if (log_level >= LOG_NORMAL)
            warn("writev");
          errno = EIO;
          return -1;
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
          .max_background = 1, // TODO: go multithreaded
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
          if (log_level >= LOG_NORMAL)
            warn("writev");
          errno = EIO;
          return -1;
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
        if (log_level >= LOG_NORMAL)
          warn("writev");
        errno = EIO;
        return -1;
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
            if (log_level >= LOG_NORMAL)
              warn("writev");
            errno = EIO;
            return -1;
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
            if (log_level >= LOG_NORMAL)
              warn("writev");
            errno = EIO;
            return -1;
          }
        }
    }
  }

  return 0;
}

void usage(int argc, char** argv) {
  (void) argc;

  printf(
      "usage: %s [<options>...] <command> [<args>...]\n"
      "       %s [<options>...] <filepath> <mountpoint>\n"
      "options:\n"
      "  --version      | -V           display version\n"
      "  --help         | -h           display this help\n"
      "                                If version or help is displayed,\n"
      "                                the program exits without executing any command.\n"
      "  --verbose      | -v           verbose mode\n"
      "  --quiet        | -q           quiet mode\n"
      "  --[no-]daemon                 daemonize the server\n"
      "  --[no-]unmount | -u           automatically unmount if possible\n"
      "commands:\n"
      "  new   [<options>...] <filepath> <blocks> <block_size>\n"
      "                                create a new filesystem in <filepath>\n"
      "  mount [<options>...] <filepath> <mountpoint>\n"
      "                                mount <filepath> to <mountpoint>\n"
      "\n",
      argv[0], argv[0]);
}

int main(int argc, char** argv) { // TODO: replace warn & warnx
  executable_name = argv[0];

  PAGE_SIZE = sysconf(_SC_PAGESIZE);
  if (PAGE_SIZE == -1) {
    if (log_level >= LOG_NORMAL)
      warn("sysconf _SC_PAGESIZE");
    exit(EXIT_FAILURE);
  }

  atexit(cleanup);
  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);

  executable_name = strrchr(argv[0], '/');
  if (executable_name == NULL)
    executable_name = argv[0];
  else
    executable_name += 1;

  log_level = LOG_NORMAL;
  bool display_version = false;
  bool display_help = false;
  bool daemonize = true;

  unsigned long long new_blocks;
  unsigned long new_block_size;
  char* filepath = NULL;

  enum command_e {
    COMMAND_NONE,
    COMMAND_NEW,
    COMMAND_MOUNT
  } program_command = COMMAND_NONE;

  if (argc == 0) {
    errno = EIO;
    if (log_level >= LOG_NORMAL)
      warn(NULL);
    exit(EXIT_FAILURE);
  } else if (argc == 1) {
    if (log_level >= LOG_NORMAL) {
      warnx("No input provided");
      usage(argc, argv);
    }
    exit(EXIT_FAILURE);
  } else {
    enum command_e command = COMMAND_MOUNT;
    enum {
      EXPECT_NONE,
      EXPECT_NEW_FILEPATH,
      EXPECT_NEW_BLOCKS,
      EXPECT_NEW_BLOCK_SIZE,
      EXPECT_MOUNT_FILEPATH,
      EXPECT_MOUNT_MOUNTPOINT,
    } expecting = EXPECT_MOUNT_FILEPATH;
    bool expect_only_arg = false;
    for (int i = 1; i < argc; ++i) {
      if (expect_only_arg) goto label_expect_arg;

      if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0)
        display_version = true;
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        display_help = true;
      else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
        log_level = LOG_VERBOSE;
      else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
        log_level = LOG_QUIET;
      else if (strcmp(argv[i], "--daemon") == 0)
        daemonize = true;
      else if (strcmp(argv[i], "--no-daemon") == 0)
        daemonize = false;
      else if (strcmp(argv[i], "--unmount") == 0 || strcmp(argv[i], "-u") == 0)
        unmount = true;
      else if (strcmp(argv[i], "--no-unmount") == 0)
        unmount = false;
      else if (strcmp(argv[i], "new") == 0) {
        if (expecting == EXPECT_NONE) {
          if (log_level >= LOG_NORMAL)
            warnx("Only one command may be executed");
          exit(EXIT_FAILURE);
        }
        command = COMMAND_NEW;
        expecting = EXPECT_NEW_FILEPATH;
        expect_only_arg = true;
      } else if (strcmp(argv[i], "mount") == 0) {
        if (expecting == EXPECT_NONE) {
          if (log_level >= LOG_NORMAL)
            warnx("Only one command may be executed");
          exit(EXIT_FAILURE);
        }
        command = COMMAND_MOUNT;
        expecting = EXPECT_MOUNT_FILEPATH;
        expect_only_arg = true;
      } else {
label_expect_arg:
        expect_only_arg = true;
        switch (expecting) {
          case EXPECT_NONE:
            if (log_level >= LOG_NORMAL)
              warnx("Unknown argument '%s'", argv[i]);
            exit(EXIT_FAILURE);
            break;
          case EXPECT_NEW_FILEPATH:
            filepath = argv[i];
            expecting = EXPECT_NEW_BLOCKS;
            break;
          case EXPECT_NEW_BLOCKS:
            {
              errno = 0;
              char* end = NULL;
              unsigned long long blocks = strtoull(argv[i], &end, 0);
              if (errno != 0) {
                if (log_level >= LOG_NORMAL)
                  warn("strtoull");
                exit(EXIT_FAILURE);
              } else if (end == argv[i]) {
                if (log_level >= LOG_NORMAL)
                  warnx("Could not parse '%s' as an unsigned integer", argv[1]);
                exit(EXIT_FAILURE);
              }

              new_blocks = blocks;
              expecting = EXPECT_NEW_BLOCK_SIZE;
              break;
            }
          case EXPECT_NEW_BLOCK_SIZE:
            {
              errno = 0;
              char* end = NULL;
              unsigned long blocks = strtoul(argv[i], &end, 0);
              if (errno != 0) {
                if (log_level >= LOG_NORMAL)
                  warn("strtoul");
                exit(EXIT_FAILURE);
              } else if (end == argv[i]) {
                if (log_level >= LOG_NORMAL)
                  warnx("Could not parse '%s' as an unsigned integer", argv[1]);
                exit(EXIT_FAILURE);
              }

              new_block_size = blocks;
              expecting = EXPECT_NONE;
              command = COMMAND_NONE;
              program_command = COMMAND_NEW;
              expect_only_arg = false;
              break;
            }
          case EXPECT_MOUNT_FILEPATH:
            filepath = argv[i];
            expecting = EXPECT_MOUNT_MOUNTPOINT;
            break;
          case EXPECT_MOUNT_MOUNTPOINT:
            mountpoint = argv[i];
            expecting = EXPECT_NONE;
            command = COMMAND_NONE;
            program_command = COMMAND_MOUNT;
            expect_only_arg = false;
            break;
        }
      }
    }

    if (!(command == COMMAND_MOUNT && expecting == EXPECT_MOUNT_FILEPATH) && (command != COMMAND_NONE || expecting != EXPECT_NONE)) {
      if (log_level >= LOG_NORMAL) {
        warnx("Incomplete command");
        usage(argc, argv);
      }
      exit(EXIT_FAILURE);
    }
  }

  if (display_version)
    printf("kim %u.%u.%u (FUSE %u.%u)\n", KIM_VERSION_MAJOR, KIM_VERSION_MINOR, KIM_VERSION_PATCH, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
  if (display_help)
    usage(argc, argv);
  if (display_version || display_help)
    exit(EXIT_SUCCESS);

  switch (program_command) {
    case COMMAND_NONE:
      {
        if (log_level >= LOG_NORMAL) {
          warnx("No command provided");
          usage(argc, argv);
        }
        exit(EXIT_FAILURE);
      }
    case COMMAND_NEW:
      {
        if (kim_fuse_new(filepath, new_blocks, new_block_size) == -1) {
          if (log_level >= LOG_NORMAL)
            warn("kim_fuse_new");
          exit(EXIT_FAILURE);
        }
        break;
      }
    case COMMAND_MOUNT:
      {
        struct __user_cap_header_struct header = {
          .version = _LINUX_CAPABILITY_VERSION_3,
          .pid = 0
        };

        struct __user_cap_data_struct data[2];

        if (syscall(SYS_capget, &header, &data) == -1) {
          if (log_level >= LOG_NORMAL)
            warn("syscall SYS_capget");
          exit(EXIT_FAILURE);
        }

        bool cap_sys_admin = (data[CAP_SYS_ADMIN / 32].effective & (1U << (CAP_SYS_ADMIN % 32))) != 0;

        if (!cap_sys_admin)
          if (log_level >= LOG_VERBOSE)
            warnx("CAP_SYS_ADMIN capability may be required to mount this filesystem");

        if (kim_fuse_mount(filepath, mountpoint, daemonize) == -1) {
          if (log_level >= LOG_NORMAL)
            warn("kim_fuse_mount");
          exit(EXIT_FAILURE);
        }
        break;
      }
  }

  return EXIT_SUCCESS;
}

