#include <stdint.h>
#include <unistd.h>

#include "fs.h"


struct kim_fs_header {
  uint64_t blocks;
};

struct kim_fs_runtime {
  int fd;
  struct kim_fs_header header;
};

int kim_fs_new(int fd, uint64_t blocks) {
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

  *runtime = (struct kim_fs_runtime) {
    .fd = fd,
    .header = header
  };

  return 0;
}

int kim_fs_flush_all(struct kim_fs_runtime* runtime) {
  if (lseek(runtime->fd, 0, SEEK_SET) == (off_t) -1) return -1;
  if(write(runtime->fd, &runtime->header, sizeof(struct kim_fs_header)) == -1) return -1;

  return 0;
}

