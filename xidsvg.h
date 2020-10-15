// XID SVG library
//
// TODO Connect to print server
// TODO Convert SVG to JSON
// TODO Disconnect


typedef void xid_status_t(const char *);        // Status reporting function
void xid_set_stratus(xid_status_t *);
j_t xid_compose(xml_t, int dpi, int rows, int cols);
const char *xid_connect(const char *sizeserver, const char *xidport, const char *keyfile, const char *certfile, j_stream_t * jin);
