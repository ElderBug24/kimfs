#include <stdint.h>


#define BLOCK_SIZE 512

struct kim_fs_runtime;

int kim_fs_new(int fd, uint64_t blocks);
int kim_open_fs(struct kim_fs_runtime* runtime, int fd);

int kim_fs_flush_all(struct kim_fs_runtime* runtime);

