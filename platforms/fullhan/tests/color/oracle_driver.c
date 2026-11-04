#include "../../src/native/isp/isp_color_auto.h"
#include <errno.h>
int fh_linux_io_isp_read(void *io,uint32_t off,uint32_t *out){(void)io;(void)off;(void)out;return -ENOTSUP;}
int fh_linux_io_isp_write(void *io,uint32_t off,uint32_t value){(void)io;(void)off;(void)value;return -ENOTSUP;}
