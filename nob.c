#define NOB_IMPLEMENTATION
#include "nob.h"

#define BUILD_FOLDER "./build/"
#define SRC_FOLDER   "./src/"
#define THIRD_PART_FOLDER   "./third_party/"

int main(int argc, char **argv)
{
  GO_REBUILD_URSELF(argc, argv);

  if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
  
  Nob_Cmd cmd = {0};
  
  nob_cc(&cmd);
  nob_cc_flags(&cmd);
  nob_cc_output(&cmd, BUILD_FOLDER "hf");
  nob_cc_inputs(&cmd, "-I" THIRD_PART_FOLDER);
  nob_cc_inputs(&cmd,
                THIRD_PART_FOLDER "qrcodegen.c",
                THIRD_PART_FOLDER "picohttpparser.c");

  Nob_File_Paths children = {0};
  if (!nob_read_entire_dir(SRC_FOLDER, &children)) return 1;

  for (size_t i = 0; i < children.count; ++i) {
    const char *name = children.items[i];
    if (nob_sv_end_with(nob_sv_from_cstr(name), ".c")) {
      nob_cc_inputs(&cmd, nob_temp_sprintf("%s%s", SRC_FOLDER, name));
    }
  } 
  if (!nob_cmd_run(&cmd)) return 1;

  return 0;
  

}
