#include <stdint.h>


#define BLOCK_SIZE 512

struct kim_fs_header {
  uint64_t blocks;
};

struct kim_fs_runtime {
  struct kim_fs_header header;
  int fd;
  bool initialized;
};

int kim_new_fs(int fd, uint64_t blocks);
int kim_open_fs(struct kim_fs_runtime* runtime, int fd);

int kim_fs_flush_all(struct kim_fs_runtime* runtime);

