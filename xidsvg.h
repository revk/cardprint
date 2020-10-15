// XID SVG library
//
// TODO Connect to print server
// TODO Convert SVG to JSON
// TODO Disconnect


typedef void xid_status_t(const char *);        // Status reporting function
j_t xid_compose(xml_t, int dpi, int rows, int cols, xid_status_t *);
