#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "fs.h"


int kim_new_fs(int fd, uint64_t blocks) {
  if (ftruncate(fd, blocks * BLOCK_SIZE) == -1) return -1;

  struct kim_fs_header header = (struct kim_fs_header) {
    .blocks = blocks
  };

  if (lseek(fd, 0, SEEK_SET) == (off_t) -1) return -1;
  if(write(fd, &header, sizeof(struct kim_fs_header)) == -1) return -1;

  return 0;
}

int kim_open_fs(struct kim_fs_runtime* runtime, int fd) {
  struct kim_fs_header header;
  if (lseek(fd, 0, SEEK_SET) == (off_t) -1) return -1;
  if (read(fd, &header, sizeof(struct kim_fs_header)) == -1) return -1;

  // TODO: check size

  *runtime = (struct kim_fs_runtime) {
    .header = header,
    .fd = fd,
    .initialized = true
  };

  return 0;
}

int kim_fs_flush_all(struct kim_fs_runtime* runtime) {
  if (!runtime->initialized) {
    errno = EINVAL;
    return -1;
  }

  if (lseek(runtime->fd, 0, SEEK_SET) == (off_t) -1) return -1;
  if(write(runtime->fd, &runtime->header, sizeof(struct kim_fs_header)) == -1) return -1;

  return 0;
}

