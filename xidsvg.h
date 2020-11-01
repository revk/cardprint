// XID SVG library
//
j_t xid_compose(xml_t, int dpi, int rows, int cols);
j_t xid_connect(const char *xidserver, const char *keyfile, const char *certfile, ajl_t * i, ajl_t * o);
void xid_disconnect(ajl_t *, ajl_t *);
ssize_t xid_write_func(void *arg, void *buf, size_t len);
