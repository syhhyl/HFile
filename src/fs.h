
#ifdef _WIN32
  #ifndef O_BINARY
    #ifdef _O_BINARY
      #define O_BINARY _O_BINARY
    #else
      #define O_BINARY 0
    #endif
  #endif

  #define read _read
  #define write _write
  #define open _open
#endif

int fd_close(int fd);