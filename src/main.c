#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include "server.h"
#include "client.h"
#include "stdint.h"
#include "stdlib.h"
#include "helper.h"
#include "stdbool.h"

typedef enum {
  server_mode,
  client_mode,
  init_mode
} state;

state flag = init_mode;

// const char *ip = "127.0.0.1";
// uint16_t port = 9000;
// char *file_path = NULL;

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s -s <server_path> [-p <port>]\n"
          "  %s -c <file_path> [-i <ip>] [-p <port>]\n",
          argv0, argv0);
}

static int parse_port(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') return 1;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 1;
  if (v == 0 || v > 65535UL) return 1;
  *out = (uint16_t)v;
  return 0;
}



// int main(int argc, char **argv) {
//   int opt;
//   char *server_path = NULL;

//   while ((opt = getopt(argc, argv, "s:c:i:p:h")) != -1) {
//     if (opt == 's') {
//       if (flag == client_mode) {
//         fprintf(stderr, "cannot use -s and -c together\n");
//         usage(argv[0]);
//         return 1;
//       }
//       flag = server_mode;
//       server_path = optarg;
//     } else if (opt == 'c') {
//       if (flag == server_mode) {
//         fprintf(stderr, "cannot use -s and -c together\n");
//         usage(argv[0]);
//         return 1;
//       }
//       flag = client_mode;
//       file_path = optarg;
//     } else if (opt == 'i') {
//       ip = optarg;
//     } else if (opt == 'p') {
//       if (parse_port(optarg, &port) != 0) {
//         fprintf(stderr, "invalid port: %s\n", optarg)
//         return 1;
//       }
//     } else if (opt == 'h') {
//       usage(argv[0]);
//       return 0;
//     } else {
//       usage(argv[0]);
//       return 1;
//     }
//   }

//   if (flag == server_mode) {
//     if (server_path == NULL) {
//       fprintf(stderr, "missing -s <server_path>\n");
//       usage(argv[0]);
//       return 1;
//     }
//     return server(server_path, port);
//   }

//   if (flag == client_mode) {
//     if (file_path == NULL) {
//       fprintf(stderr, "missing -c <file_path>\n");
//       usage(argv[0]);
//       return 1;
//     }
//     return client(file_path, ip, port);
//   }

//   usage(argv[0]);
//   return 1;
// }


//TODO rewrite main logic
int main(int argc, char **argv) {
  int opt;
  
  state st = init_mode;
  char *path = NULL;
  char *ip = "127.0.0.1";
  uint16_t port = 9000;
  

  while ((opt = getopt(argc, argv, "s:c:i:p:h")) != -1) {
    printf("opt:%c ", opt);
    
    switch (opt) {
      case 's':
        if (st == client_mode) {
          fprintf(stderr, "cannot use -c and -s together\n");
          goto BAD_USE;
        }
        st = server_mode;
        path = optarg;
        // if (checkpath(optarg)) path = optarg;
        // else goto BAD_USE;
        break;
      case 'c':
        if (st == server_mode) {
          fprintf(stderr, "cannot use -s and -c together\n");
          goto BAD_USE;
        }
        st = client_mode;
        path = optarg;
        // if (checkpath(optarg)) path = optarg;
        // else goto BAD_USE;
        break;
      case 'i':
        ip = optarg;
        break;
      case 'p':
        if (parse_port(optarg, &port) != 0) {
          fprintf(stderr, "invalid port: %s\n", optarg);
          goto BAD_USE;
        }
        break;
      case 'h':
        usage(argv[0]);
        return 0;
    }
  }

  if (st == server_mode)
    return server(path, port);
  else if (st == client_mode)
    return client(path, ip, port);
      

BAD_USE:
  usage(argv[0]);
  return 1;
  
}
