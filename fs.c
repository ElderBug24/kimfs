#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"


int kim_new_fs(int fd, uint64_t blocks, uint32_t block_size) {
  if (block_size < sizeof(struct kim_fs_header) || blocks == 0) {
    errno = EINVAL;
    return -1;
  }

  if (ftruncate(fd, (off_t)(blocks * (uint64_t)block_size)) == -1)
    return -1;

  struct kim_fs_header header = (struct kim_fs_header) {
    .blocks = blocks,
    .block_size = block_size
  };

  if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
    return -1;
  if (write(fd, &header, sizeof(struct kim_fs_header)) == -1)
    return -1;

  return 0;
}

int kim_open_fs(struct kim_fs_runtime* runtime, int fd) {
  struct kim_fs_header header;
  if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
    return -1;
  if (read(fd, &header, sizeof(struct kim_fs_header)) == -1)
    return -1;

  struct stat buf;
  fstat(fd, &buf);
  if (buf.st_size < (off_t) 0 || (uintmax_t) buf.st_size < (uintmax_t) (header.blocks * (uint64_t) header.block_size)) {
    errno = EINVAL;
    return -1;
  }

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

  if (lseek(runtime->fd, 0, SEEK_SET) == (off_t)-1)
    return -1;
  if (write(runtime->fd, &runtime->header, sizeof(struct kim_fs_header)) == -1)
    return -1;

  return 0;
}

