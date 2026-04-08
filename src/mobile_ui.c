#include "mobile_ui.h"

#include "net.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

int mobile_ui_is_tty_stdout(void) {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) ? 1 : 0;
#else
  return isatty(fileno(stdout)) ? 1 : 0;
#endif
}

int mobile_ui_build_url(uint16_t port,
                        char *url_out,
                        size_t url_out_cap,
                        int *phone_reachable_out) {
  const char *host = NULL;
  char discovered_ip[64];
  if (url_out == NULL || url_out_cap == 0 || phone_reachable_out == NULL ||
      port == 0) {
    return 1;
  }

  if (net_primary_ipv4(discovered_ip, sizeof(discovered_ip)) == 0) {
    host = discovered_ip;
    *phone_reachable_out = 1;
  } else {
    host = "127.0.0.1";
    *phone_reachable_out = 0;
  }

  int n = snprintf(url_out, url_out_cap, "http://%s:%u/", host,
                   (unsigned)port);
  if (n < 0 || (size_t)n >= url_out_cap) {
    return 1;
  }

  return 0;
}

int mobile_ui_print_qr(FILE *out, const char *url) {
  uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
  uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  const int border = 4;

  if (out == NULL || url == NULL || url[0] == '\0') {
    return 1;
  }

  if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                            qrcodegen_Mask_AUTO, true)) {
    return 1;
  }

  int size = qrcodegen_getSize(qr);
  for (int y = -border; y < size + border; y += 2) {
    for (int x = -border; x < size + border; x++) {
      int top = (x >= 0 && x < size && y >= 0 && y < size)
                  ? qrcodegen_getModule(qr, x, y)
                  : 0;
      int bottom = (x >= 0 && x < size && (y + 1) >= 0 && (y + 1) < size)
                     ? qrcodegen_getModule(qr, x, y + 1)
                     : 0;
      if (top && bottom) {
        fputs("\xE2\x96\x88", out);
      } else if (top) {
        fputs("\xE2\x96\x80", out);
      } else if (bottom) {
        fputs("\xE2\x96\x84", out);
      } else {
        fputc(' ', out);
      }
    }
    fputc('\n', out);
  }

  if (ferror(out)) {
    return 1;
  }

  return 0;
}
