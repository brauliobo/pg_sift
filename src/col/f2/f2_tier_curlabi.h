/*
 * col/f2/f2_tier_curlabi.h - the part of the libcurl easy interface that f2_tier_curl.c uses, declared by hand: pg_sift links no libcurl and needs no curl header to build. The numbers are those of
 * curl.h and have been stable since 7.x; tests/test_curl_abi.c compares every one with the curl.h of the build host when there is one.
 */
#ifndef SIFT_F2_TIER_CURLABI_H
#define SIFT_F2_TIER_CURLABI_H

#ifndef CURLINC_CURL_H
typedef void CURL;
typedef int CURLcode;
typedef int64 curl_off_t;
struct curl_slist;
#endif

#define SC_OK						0
#define SC_ABORTED_BY_CALLBACK		42
#define SC_GLOBAL_DEFAULT			3
#define SC_READFUNC_ABORT			0x10000000
#define SC_INFO_RESPONSE_CODE		0x200002

#define SC_WRITEDATA				10001
#define SC_URL						10002
#define SC_READDATA					10009
#define SC_HEADERDATA				10029
#define SC_USERAGENT				10018
#define SC_HTTPHEADER				10023
#define SC_CUSTOMREQUEST			10036
#define SC_XFERINFODATA				10057
#define SC_LOW_SPEED_LIMIT			19
#define SC_LOW_SPEED_TIME			20
#define SC_NOPROGRESS				43
#define SC_NOBODY					44
#define SC_UPLOAD					46
#define SC_FOLLOWLOCATION			52
#define SC_CONNECTTIMEOUT			78
#define SC_PROTOCOLS				181
#define SC_PROTO_HTTP_HTTPS			3L	/* CURLPROTO_HTTP | CURLPROTO_HTTPS */
#define SC_NOSIGNAL					99
#define SC_WRITEFUNCTION			20011
#define SC_READFUNCTION				20012
#define SC_HEADERFUNCTION			20079
#define SC_XFERINFOFUNCTION			20219
#define SC_INFILESIZE_LARGE			30115

#endif							/* SIFT_F2_TIER_CURLABI_H */
