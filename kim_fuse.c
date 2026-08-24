#include <fcntl.h>
#include <linux/fuse.h>
#include <stdbool.h>
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
    if (umount(DEFAULT_MOUNT_POINT) == -1)
      perror("umount");
}

void signal_handler(int sig) {
  (void) sig;

  exit(1);
}

int main(void) {
  int ret = 0;
  long PAGE_SIZE = sysconf(_SC_PAGESIZE);

  atexit(cleanup);
  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);

  fuse_fd = open(FUSE_FILE, O_RDWR);
  if (fuse_fd == -1) {
    perror("open '" FUSE_FILE "'");
    exit(1);
  }

  char options[64];
  snprintf(options, sizeof(options), "fd=%d,rootmode=40000,user_id=%u,group_id=%u", fuse_fd, getuid(), getgid());
  mounted = true;
  if (mount("fuse", DEFAULT_MOUNT_POINT, "fuse", 0, options) == -1) {
    perror("mount");
    exit(1);
  }

  unsigned long buf_size = (MAX_PAGES + 1) * PAGE_SIZE;
  char buf[buf_size];
  long count;
  bool running = true;
  while (running) {
    count = read(fuse_fd, buf, buf_size);
    if (count == -1) {
      ret = 1;
      perror("read");
      running = false;
      break;
    }

    struct fuse_in_header in_header = *(struct fuse_in_header*) buf;
    void* payload = &buf[sizeof(struct fuse_in_header)];

    switch (in_header.opcode) {
      case FUSE_INIT:
        ;
        struct fuse_init_in init_in = *(struct fuse_init_in*) payload;
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
          .time_gran = 1000000000,
          .max_pages = MAX_PAGES,
          .map_alignment = 0,
          .flags2 = 0,
        };
        struct iovec iov[] = {
          { &out_header, sizeof(struct fuse_out_header) },
          { &init_out, sizeof(struct fuse_init_out) }
        };

        if (writev(fuse_fd, iov, 2) == -1) {
          perror("writev");
          exit(1);
        }
        break;
    }
  }

  return ret;
}

