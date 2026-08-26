#define NOB_IMPLEMENTATION
#include "../nob.h"


int main(int argc, char** argv) {
  NOB_GO_REBUILD_URSELF(argc, argv);

  nob_mkdir_if_not_exists("build/");

  Nob_Cmd cmd = {0};

  nob_cmd_append(&cmd, "gcc", "-x", "c", "-Wall", "-Wextra", "-Wconversion", "-pedantic", "-ggdb", "fuse.c", "common.c", "-o", "build/fuse");
  if (!nob_cmd_run(&cmd)) return 1;
  nob_cmd_append(&cmd, "build/fuse");
  if (!nob_cmd_run(&cmd)) return 1;

  return 0;
}

