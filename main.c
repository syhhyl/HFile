#include <stdio.h>
#include <unistd.h>
#include <getopt.h>


int main(int argc, char *argv[]) {
  int opt = getopt(argc, argv, "sc"); 
  if (opt == -1) {
    perror("can't find -s or -c\n");
    return -1;
  }
  if (opt == 'c') {
    printf("-c\n");
  } else if (opt == 's'){
    printf("-s\n");
  } else {
    printf("no support");
  }
  return 0;
}
