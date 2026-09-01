#define NOB_IMPLEMENTATION
#include "../nob.h"


int main(int argc, char** argv) {
  NOB_GO_REBUILD_URSELF(argc, argv);

  nob_mkdir_if_not_exists("build/");

  Nob_Cmd cmd = {0};

  if (argc > 1 && argv[1][0] == 'b')
    nob_cmd_append(&cmd, "gcc", "-x", "c", "-Wall", "-Wextra", "-Wconversion", "-pedantic", "-O3", "-D_FILE_OFFSET_BITS=64", "fuse.c", "common.c", "lib/kim.c", "-o", "build/kim");
  else
    nob_cmd_append(&cmd, "gcc", "-x", "c", "-Wall", "-Wextra", "-Wconversion", "-pedantic", "-O0", "-fsanitize=address,undefined,bool,enum", "-ggdb", "-D_FILE_OFFSET_BITS=64", "fuse.c", "common.c", "lib/kim.c", "-o", "build/kim");
  if (!nob_cmd_run(&cmd)) return 1;

  return 0;
}

