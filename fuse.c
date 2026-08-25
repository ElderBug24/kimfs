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


#define FUSE_FILE "/dev/fuse"
#define DEFAULT_MOUNT_POINT "/mnt/kim"
#define MAX_PAGES 64

#define PROTOCOL_VERSION_MAJOR 7
#define PROTOCOL_VERSION_MINOR 39

int fuse_fd = -1;
bool mounted = false;

void cleanup(void) {
  if (fuse_fd != -1)
    if (close(fuse_fd) == -1)
      perror("close");

  if (mounted)
    if (umount2(DEFAULT_MOUNT_POINT, MNT_FORCE) == -1)
      perror("umount");
}

void signal_handler(int sig) {
  (void) sig;

  exit(EXIT_FAILURE);
}

int main(void) {
  long PAGE_SIZE = sysconf(_SC_PAGESIZE);

  atexit(cleanup);
  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);

  fuse_fd = open(FUSE_FILE, O_RDWR);
  if (fuse_fd == -1) {
    perror("open '" FUSE_FILE "'");
    exit(EXIT_FAILURE);
  }


  char options[256];
  snprintf(options, sizeof(options), "fd=%d,rootmode=040777,user_id=%u,group_id=%u,default_permissions,allow_other,subtype=kim", fuse_fd, getuid(), getgid()); // TODO: fsname=NAME for the name of the backing storage file
  mounted = true;
  if (mount("fuse", DEFAULT_MOUNT_POINT, "fuse", 0, options) == -1) {
    perror("mount");
    exit(EXIT_FAILURE);
  }

  unsigned long buf_size = (MAX_PAGES + 1) * PAGE_SIZE;
  char buf[buf_size];
  long count;
  bool running = true;
  bool initialized = false;
  while (running) {
    count = read(fuse_fd, buf, buf_size);
    if (count == -1) {
      perror("read");
      exit(EXIT_FAILURE);
    }

    struct fuse_in_header in_header = *(struct fuse_in_header*) buf;
    void* payload = &buf[sizeof(struct fuse_in_header)];

    printf("[INFO] received op %u from the kernel\n", in_header.opcode);
    if (in_header.opcode == FUSE_INIT) {
      struct fuse_init_in init_in = *(struct fuse_init_in*) payload;
      printf("[INFO] kernel's FUSE protocol version is %u.%u (supported is %u.%u)\n", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
      if (init_in.major < PROTOCOL_VERSION_MAJOR) {
        fprintf(stderr, "error: kernel's FUSE protocol version is too old (%u.%u < %u.%u)", init_in.major, init_in.minor, PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
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
          perror("writev");
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
          .max_write = MAX_PAGES * PAGE_SIZE,
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
          perror("writev");
          exit(EXIT_FAILURE);
        }

        initialized = true;
      }
      continue;
    } else if (!initialized) goto refuse_req;
    switch (in_header.opcode) {
      default:
        goto refuse_req;
    }
    continue;

refuse_req:
    ;
    struct fuse_out_header out_header = (struct fuse_out_header) {
      .len = sizeof(struct fuse_out_header),
      .error = -EUNATCH,
      .unique = in_header.unique
    };

    struct iovec iov[] = {
      { &out_header, sizeof(struct fuse_out_header) }
    };

    if (writev(fuse_fd, iov, 1) == -1) {
      perror("writev");
      exit(EXIT_FAILURE);
    }
  }

  return EXIT_SUCCESS;
}

