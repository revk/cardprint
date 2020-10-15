// XID SVG library
//
typedef void xid_status_t(const char *);        // Status reporting function
void xid_set_status(xid_status_t *);
j_t xid_compose(xml_t, int dpi, int rows, int cols);
const char *xid_connect(const char *xidserver, const char *keyfile, const char *certfile, j_stream_t * jin);
ssize_t xid_write_func(void *arg, void *buf, size_t len);

