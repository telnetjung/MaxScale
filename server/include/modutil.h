#ifndef _MODUTIL_H
#define _MODUTIL_H
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file modutil.h A set of useful routines for module writers
 *
 * @verbatim
 * Revision History
 *
 * Date         Who                     Description
 * 04/06/14     Mark Riddoch            Initial implementation
 * 24/06/14     Mark Riddoch            Add modutil_MySQL_Query to enable multipacket queries
 * 24/10/14     Massimiliano Pinto      Add modutil_send_mysql_err_packet to send a mysql ERR_Packet
 *
 * @endverbatim
 */
#include <buffer.h>
#include <dcb.h>
#include <string.h>
#include <maxscale_pcre2.h>

#define PTR_IS_RESULTSET(b) (b[0] == 0x01 && b[1] == 0x0 && b[2] == 0x0 && b[3] == 0x01)
#define PTR_IS_EOF(b) (b[0] == 0x05 && b[1] == 0x0 && b[2] == 0x0 && b[4] == 0xfe)
#define PTR_IS_OK(b) (b[4] == 0x00)
#define PTR_IS_ERR(b) (b[4] == 0xff)
#define PTR_IS_LOCAL_INFILE(b) (b[4] == 0xfb)
#define IS_FULL_RESPONSE(buf) (modutil_count_signal_packets(buf,0,0) == 2)
#define PTR_EOF_MORE_RESULTS(b) ((PTR_IS_EOF(b) && ptr[7] & 0x08))


extern int      modutil_is_SQL(GWBUF *);
extern int      modutil_is_SQL_prepare(GWBUF *);
extern int      modutil_extract_SQL(GWBUF *, char **, int *);
extern int      modutil_MySQL_Query(GWBUF *, char **, int *, int *);
extern char*    modutil_get_SQL(GWBUF *);
extern GWBUF*   modutil_replace_SQL(GWBUF *, char *);
extern char*    modutil_get_query(GWBUF* buf);
extern int      modutil_send_mysql_err_packet(DCB *, int, int, int, const char *, const char *);
GWBUF*          modutil_get_next_MySQL_packet(GWBUF** p_readbuf);
GWBUF*          modutil_get_complete_packets(GWBUF** p_readbuf);
int             modutil_MySQL_query_len(GWBUF* buf, int* nbytes_missing);
void            modutil_reply_parse_error(DCB* backend_dcb, char* errstr, uint32_t flags);
void            modutil_reply_auth_error(DCB* backend_dcb, char* errstr, uint32_t flags);
int             modutil_count_statements(GWBUF* buffer);
GWBUF*          modutil_create_query(char* query);
GWBUF*          modutil_create_mysql_err_msg(int             packet_number,
                                             int             affected_rows,
                                             int             merrno,
                                             const char      *statemsg,
                                             const char      *msg);
int modutil_count_signal_packets(GWBUF*, int, int, int*);
mxs_pcre2_result_t modutil_mysql_wildcard_match(const char* pattern, const char* string);

/** Character and token searching functions */
char* strnchr_esc(char* ptr, char c, int len);
char* strnchr_esc_mysql(char* ptr, char c, int len);
bool is_mysql_statement_end(const char* start, int len);
bool is_mysql_sp_end(const char* start, int len);
char* modutil_get_canonical(GWBUF* querybuf);

#endif
