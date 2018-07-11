/*
 * Copyright (C) 2000-2007 Hewlett-Packard Company
 * Copyright (C) 2007 Ted Bullock <tbullock@comlore.com>
 * 
 * This file is part of httperf, a web server performance measurment tool.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * In addition, as a special exception, the copyright holders give permission
 * to link the code of this work with the OpenSSL project's "OpenSSL" library
 * (or with modified versions of it that use the same license as the "OpenSSL" 
 * library), and distribute linked combinations including the two.  You must
 * obey the GNU General Public License in all respects for all of the code
 * used other than "OpenSSL".  If you modify this file, you may extend this
 * exception to your version of the file, but you are not obligated to do so.
 * If you do not wish to do so, delete this exception statement from your
 * version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA 
 */

/*
 * Fundamentals:
 * 
 * There are three subsystems to httperf:
 * 
 * 1) The load generator which determines what URI is fetched next.
 * 
 * 2) The core engine that handles the mechanics of issuing a request.
 * 
 * 3) The instrumentation infrastructure that measures various aspects of the
 * transaction(s).
 * 
 * Since there is considerable potential variation in all three, it seems like 
 * an event-based approach might be ideal in tying the three together.
 * Ideally, it should be possible to write a new load generator without
 * modifications to the other subsystems.  Similarly, it should be possible to 
 * add instrumentation without requiring changes to the load generator or http 
 * engine.
 * 
 * Axioms: - The only point at which the client will fall back is if the
 * client itself is overloaded.  There is no point trying to fix up this
 * case---simply declare defeat and abort the test. 
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __FreeBSD__
/* Required for fpsetmask() under FreeBSD */
#include <ieeefp.h>
#endif

#include <sys/time.h>

#include <generic_types.h>
#include <sys/resource.h>	/* after sys/types.h for BSD (in generic_types.h) */

#include <object.h>
#include <timer.h>
#include <conn.h>
#include <call.h>
#include <core.h>
#include <localevent.h>
#include <httperf.h>


#ifdef HAVE_SSL
#  include <openssl/rand.h>
#endif

#define RATE_INTERVAL	5.0

const char     *prog_name;
int             verbose;
int             periodic_stats;
Cmdline_Params  param;
Time            test_time_start;
Time            test_time_stop;
struct rusage   test_rusage_start;
struct rusage   test_rusage_stop;
size_t          object_type_size[OBJ_NUM_TYPES];

#ifdef HAVE_SSL
SSL_CTX        *ssl_ctx;
#endif

#ifdef DEBUG
int             debug_level;
#endif

static Time     perf_sample_start;

static struct option longopts[] = {
	{"add-header", required_argument, (int *) &param.additional_header, 0},
	{"add-header-file", required_argument, (int *) &param.additional_header_file, 0 },
	{"burst-length", required_argument, (int *) &param.burst_len, 0},
	{"client", required_argument, (int *) &param.client, 0},
	{"close-with-reset", no_argument, &param.close_with_reset, 1},
	{"debug", required_argument, 0, 'd'},
	{"failure-status", required_argument, &param.failure_status, 0},
	{"help", no_argument, 0, 'h'},
	{"hog", no_argument, &param.hog, 1},
	{"http-version", required_argument, (int *) &param.http_version, 0},
	{"max-connections", required_argument, (int *) &param.max_conns, 0},
	{"max-piped-calls", required_argument, (int *) &param.max_piped, 0},
	{"method", required_argument, (int *) &param.method, 0},
	{"myaddr", required_argument, (int *) &param.myaddr, 0},
	{"no-host-hdr", no_argument, &param.no_host_hdr, 1},
	{"num-calls", required_argument, (int *) &param.num_calls, 0},
	{"num-conns", required_argument, (int *) &param.num_conns, 0},
	{"period", required_argument, (int *) &param.rate.mean_iat, 0},
	{"port", required_argument, (int *) &param.port, 0},
	{"print-reply", optional_argument, &param.print_reply, 0},
	{"print-request", optional_argument, &param.print_request, 0},
	{"rate", required_argument, (int *) &param.rate, 0},
	{"recv-buffer", required_argument, (int *) &param.recv_buffer_size, 0},
	{"retry-on-failure", no_argument, &param.retry_on_failure, 1},
	{"runtime", required_argument, (int *) &param.runtime, 0},
	{"send-buffer", required_argument, (int *) &param.send_buffer_size, 0},
	{"server", required_argument, (int *) &param.server, 0},
	{"server-name", required_argument, (int *) &param.server_name, 0},
	{"servers", required_argument, (int *) &param.servers, 0},
	{"uri", required_argument, (int *) &param.uri, 0},
	{"session-cookies", no_argument, (int *) &param.session_cookies, 1},
#ifdef HAVE_SSL
	{"ssl", no_argument, &param.use_ssl, 1},
	{"ssl-ciphers", required_argument, (int *) &param.ssl_cipher_list, 0},
	{"ssl-no-reuse", no_argument, &param.ssl_reuse, 0},
        {"ssl-certificate", required_argument, (int *) &param.ssl_cert,     0},
        {"ssl-key",      required_argument, (int *) &param.ssl_key,         0},
        {"ssl-verify",   optional_argument, (int *) &param.ssl_verify,      0},
        {"ssl-ca-file",  required_argument, (int *) &param.ssl_ca_file,     0},
        {"ssl-ca-path",  required_argument, (int *) &param.ssl_ca_path,     0},
        {"ssl-protocol", required_argument, &param.ssl_protocol,            0},
#endif
	{"think-timeout", required_argument, (int *) &param.think_timeout, 0},
	{"timeout", required_argument, (int *) &param.timeout, 0},
	{"use-timer-cache", no_argument, &param.use_timer_cache, 1},
	{"verbose", no_argument, 0, 'v'},
	{"version", no_argument, 0, 'V'},
	{"periodic-stats", no_argument, 0, 'n'},
	{"wlog", required_argument, (int *) &param.wlog, 0},
	{"wsess", required_argument, (int *) &param.wsess, 0},
	{"wsesslog", required_argument, (int *) &param.wsesslog, 0},
	{"wsesspage", required_argument, (int *) &param.wsesspage, 0},
	{"wset", required_argument, (int *) &param.wset, 0},
	{0, 0, 0, 0}
};

static void
usage(void)
{
	printf("Usage: %s "
	       "[-hdvV] [--add-header S] [--burst-length N] [--client N/N]\n"
	       "\t[--close-with-reset] [--debug N] [--failure-status N]\n"
	       "\t[--help] [--hog] [--http-version S] [--max-connections N]\n"
	       "\t[--max-piped-calls N] [--method S] [--no-host-hdr]\n"
	       "\t[--num-calls N] [--num-conns N] [--session-cookies]\n"
	       "\t[--period [d|u|e]T1[,T2]|[v]T1,D1[,T2,D2]...[,Tn,Dn]\n"
	       "\t[--print-reply [header|body]] [--print-request [header|body]]\n"
	       "\t[--rate X] [--recv-buffer N] [--retry-on-failure] [--send-buffer N]\n"
	       "\t[--server S|--servers file] [--server-name S] [--port N] [--uri S] "
	       "[--myaddr S]\n"
#ifdef HAVE_SSL
	       "\t[--ssl] [--ssl-ciphers L] [--ssl-no-reuse]\n"
               "\t[--ssl-certificate file] [--ssl-key file]\n"
               "\t[--ssl-ca-file file] [--ssl-ca-path path]\n"
               "\t[--ssl-verify [yes|no]] [--ssl-protocol S]\n"
#endif
	       "\t[--think-timeout X] [--timeout X] [--verbose] [--version]\n"
	       "\t[--wlog y|n,file] [--wsess N,N,X] [--wsesslog N,X,file]\n"
	       "\t[--wset N,X]\n"
	       "\t[--runtime X]\n"
	       "\t[--use-timer-cache]\n"
	       "\t[--periodic-stats]\n", prog_name);
}

void
panic(const char *msg, ...)
{
	va_list         va;

	va_start(va, msg);
	vfprintf(stderr, msg, va);
	va_end(va);
	exit(1);
}

void
no_op(void)
{
}

static void
perf_sample(struct Timer *t, Any_Type regarg)
{
	Any_Type        callarg;

	callarg.d = 1.0 / (timer_now() - perf_sample_start);
	event_signal(EV_PERF_SAMPLE, 0, callarg);

	/*
	 * prepare for next sample interval: 
	 */
	perf_sample_start = timer_now();
	if (timer_schedule(perf_sample, regarg, RATE_INTERVAL) == NULL)
		panic("%s(%d): Received NULL from timer_schedule\n", __func__,
		      __LINE__);
}

int
main(int argc, char **argv)
{
	extern Load_Generator uri_fixed, uri_wlog, uri_wset, conn_rate,
	    call_seq;
	extern Load_Generator wsess, wsesslog, wsesspage, sess_cookie, misc;
	extern Stat_Collector stats_basic, session_stat;
	extern Stat_Collector stats_print_reply;
	extern char    *optarg;
	int             session_workload = 0;
	int             num_gen = 3;
	Load_Generator *gen[5] = {
		&call_seq,
		&uri_fixed,
		&conn_rate,
	};
	int             num_stats = 1;
	Stat_Collector *stat[3] = {
		&stats_basic
	};
	int             i, ch, longindex;
	u_int           minor, major;
	char           *end, *name;
	Any_Type        arg;
	void           *flag;
	Time            t;

	int             numRates = 0;

#ifdef __FreeBSD__
	/*
	 * This works around a bug in earlier versions of FreeBSD that cause
	 * non-finite IEEE arithmetic to cause SIGFPE instead of the
	 * non-finite arithmetic as defined by IEEE.  
	 */
	fpsetmask(0);
#endif

	object_type_size[OBJ_CONN] = sizeof(Conn);
	object_type_size[OBJ_CALL] = sizeof(Call);

	param.http_version = 0x10001;	/* default to HTTP/1.1 */
	param.client.id = 0;
	param.client.num_clients = 1;
	param.port = -1;
	param.uri = "/";
	param.num_calls = 1;
	param.burst_len = 1;
	param.num_conns = 1;
	/*
	 * These should be set to the minimum of 2*bandwidth*delay and the
	 * maximum request/reply size for single-call connections.  
	 */
	param.send_buffer_size = 4096;
	param.recv_buffer_size = 16384;
	param.rate.dist = DETERMINISTIC;
#ifdef HAVE_SSL
	param.ssl_reuse = 1;
        param.ssl_verify = 0;
        param.ssl_protocol = 0;
#endif

	/*
	 * get program name: 
	 */
	prog_name = strrchr(argv[0], '/');
	if (prog_name)
		++prog_name;
	else
		prog_name = argv[0];

	/*
	 * process command line options: 
	 */
	while ((ch =
		getopt_long(argc, argv, "d:hvVn", longopts, &longindex)) >= 0) {
		switch (ch) {
		case 0:
			flag = longopts[longindex].flag;

			if (flag == &param.method)
				param.method = optarg;
			else if (flag == &param.additional_header)
				param.additional_header = optarg;
			else if (flag == &param.additional_header_file)
				param.additional_header_file = optarg;
			else if (flag == &param.num_calls) {
				errno = 0;
				param.num_calls = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal number of calls %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.http_version) {
				if (sscanf(optarg, "%u.%u", &major, &minor) !=
				    2) {
					fprintf(stderr,
						"%s: illegal version number %s\n",
						prog_name, optarg);
					exit(1);
				}
				param.http_version =
				    (major << 16) | (minor & 0xffff);
			} else if (flag == &param.myaddr) {
				core_add_addresses(optarg);
			} else if (flag == &param.burst_len) {
				errno = 0;
				param.burst_len = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end
				    || param.burst_len < 1) {
					fprintf(stderr,
						"%s: illegal burst-length %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.failure_status) {
				errno = 0;
				param.failure_status =
				    strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end
				    || param.failure_status <= 0) {
					fprintf(stderr,
						"%s: illegal failure status %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.num_conns) {
				errno = 0;
				param.num_conns = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal number of connections %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.max_conns) {
				errno = 0;
				param.max_conns = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal max. # of connection %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.max_piped) {
				errno = 0;
				param.max_piped = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal max. # of piped calls %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.port) {
				errno = 0;
				param.port = strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end
				    || (unsigned) param.port > 0xffff) {
					fprintf(stderr,
						"%s: illegal port number %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.print_request
				   || flag == &param.print_reply) {
				int             val;

				if (!optarg)
					val = PRINT_HEADER | PRINT_BODY;
				else
					switch (tolower(optarg[0])) {
					case 'h':
						val = PRINT_HEADER;
						break;
					case 'b':
						val = PRINT_BODY;
						break;
					default:
						val =
						    PRINT_HEADER | PRINT_BODY;
						break;
					}
				*(int *) flag = val;
			} else if (flag == &param.rate) {
				errno = 0;
				param.rate.rate_param = strtod(optarg, &end);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal request rate %s\n",
						prog_name, optarg);
					exit(1);
				}
				if (param.rate.rate_param <= 0.0)
					param.rate.mean_iat = 0.0;
				else
					param.rate.mean_iat =
					    1 / param.rate.rate_param;
				param.rate.dist = DETERMINISTIC;
			} else if (flag == &param.rate.mean_iat) {	/* --period 
									 */
				param.rate.dist = DETERMINISTIC;
				if (!isdigit(*optarg))
					switch (tolower(*optarg++)) {
					case 'd':
						param.rate.dist =
						    DETERMINISTIC;
						break;
					case 'u':
						param.rate.dist = UNIFORM;
						break;
					case 'e':
						param.rate.dist = EXPONENTIAL;
						break;
					case 'v':
						param.rate.dist = VARIABLE;
						break;
					default:
						fprintf(stderr,
							"%s: illegal interarrival distribution "
							"'%c' in %s\n",
							prog_name, optarg[-1],
							optarg - 1);
						exit(1);
					}

				/*
				 * remaining params depend on selected
				 * distribution: 
				 */
				errno = 0;
				switch (param.rate.dist) {
				case DETERMINISTIC:
				case EXPONENTIAL:
					param.rate.mean_iat =
					    strtod(optarg, &end);
					if (errno == ERANGE || end == optarg
					    || *end
					    || param.rate.mean_iat < 0) {
						fprintf(stderr,
							"%s: illegal mean interarrival "
							"time %s\n", prog_name,
							optarg);
						exit(1);
					}
					break;

				case UNIFORM:
					param.rate.min_iat =
					    strtod(optarg, &end);
					if (errno == ERANGE || end == optarg
					    || param.rate.min_iat < 0) {
						fprintf(stderr,
							"%s: illegal minimum interarrival "
							"time %s\n", prog_name,
							optarg);
						exit(1);
					}
					if (*end != ',') {
						fprintf(stderr,
							"%s: minimum interarrival time not "
							"followed by `,MAX_IAT' (rest: `%s')\n",
							prog_name, end);
						exit(1);
					}
					optarg = end + 1;
					param.rate.max_iat =
					    strtod(optarg, &end);
					if (errno == ERANGE || end == optarg
					    || *end
					    || param.rate.max_iat < 0) {
						fprintf(stderr,
							"%s: illegal request period %s\n",
							prog_name, optarg);
						exit(1);
					}
					param.rate.mean_iat =
					    0.5 * (param.rate.min_iat +
						   param.rate.max_iat);
					break;

				case VARIABLE:
					while (1) {
						if (numRates >= NUM_RATES) {
							fprintf(stderr,
								"%s: too many rates\n",
								prog_name);
							exit(1);
						}

						param.rate.iat[numRates] =
						    strtod(optarg, &end);
						if (errno == ERANGE
						    || end == optarg
						    || param.rate.
						    iat[numRates] < 0) {
							fprintf(stderr,
								"%s: illegal minimum interarrival"
								" time %s\n",
								prog_name,
								optarg);
							exit(1);
						}

						if (*end != ',') {
							fprintf(stderr,
								"%s: interarrival time not "
								"followed by `,duration' (rest: `%s')\n",
								prog_name,
								end);
							exit(1);
						}

						optarg = end + 1;
						param.rate.duration[numRates] =
						    strtod(optarg, &end);
						if (errno == ERANGE
						    || end == optarg
						    || param.rate.
						    duration[numRates] < 0) {
							fprintf(stderr,
								"%s: illegal duration %s\n",
								prog_name,
								optarg);
							exit(1);
						}

						if (numRates == 0)
							param.rate.mean_iat =
							    param.rate.
							    iat[numRates];
						else
							param.rate.mean_iat +=
							    param.rate.
							    iat[numRates];

						numRates++;

						if (*end != ',') {
							param.rate.numRates =
							    numRates;
							break;
						} else
							optarg = end + 1;
					}

					param.rate.mean_iat /= numRates;
					break;

				default:
					fprintf(stderr,
						"%s: internal error parsing %s\n",
						prog_name, optarg);
					exit(1);
					break;
				}
				param.rate.rate_param =
				    ((param.rate.mean_iat <= 0.0)
				     ? 0.0 : (1.0 / param.rate.mean_iat));
			} else if (flag == &param.recv_buffer_size) {
				errno = 0;
				param.recv_buffer_size =
				    strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end
				    || param.port > 0xffff) {
					fprintf(stderr,
						"%s: illegal receive buffer size %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.send_buffer_size) {
				errno = 0;
				param.send_buffer_size =
				    strtoul(optarg, &end, 10);
				if (errno == ERANGE || end == optarg || *end
				    || param.port > 0xffff) {
					fprintf(stderr,
						"%s: illegal send buffer size %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.client) {
				errno = 0;
				param.client.id = strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE) {
					fprintf(stderr,
						"%s: bad client id (rest: `%s')\n",
						prog_name, optarg);
					exit(1);
				}

				if (*end != '/') {
					fprintf(stderr,
						"%s: client id not followed by `/' (rest: `%s')\n",
						prog_name, end);
					exit(1);
				}
				optarg = end + 1;

				param.client.num_clients =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE
				    || param.client.id >=
				    param.client.num_clients) {
					fprintf(stderr,
						"%s: bad number of clients (rest: `%s')\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.server)
				param.server = optarg;
			else if (flag == &param.server_name)
				param.server_name = optarg;
			else if (flag == &param.servers)
				param.servers = optarg;
#ifdef HAVE_SSL
			else if (flag == &param.ssl_cipher_list)
				param.ssl_cipher_list = optarg;
                        else if (flag == &param.ssl_cert)
                                param.ssl_cert = optarg;
                        else if (flag == &param.ssl_key)
                                param.ssl_key = optarg;
                        else if (flag == &param.ssl_verify)
                            {
                                if (!optarg)
                                    param.ssl_verify = 1;
                                else
                                    switch (tolower (optarg[0]))
                                    {
                                    case 'y': param.ssl_verify = 1; break;
                                    case 'n': param.ssl_verify = 0; break;
                                    default:  param.ssl_verify = 0; break;
                                    }
                            }
                        else if (flag == &param.ssl_ca_file)
                                param.ssl_ca_file = optarg;
                        else if (flag == &param.ssl_ca_path)
                                param.ssl_ca_path = optarg;
                        else if (flag == &param.ssl_protocol)
                        {
                            if (strcasecmp (optarg, "auto") == 0)
                                param.ssl_protocol = 0;
#ifndef OPENSSL_NO_SSL2
                            else if (strcasecmp (optarg, "SSLv2") == 0)
                                param.ssl_protocol = 2;
#endif
#ifndef OPENSSL_NO_SSL3
                            else if (strcasecmp (optarg, "SSLv3") == 0)
                                param.ssl_protocol = 3;
#endif
                            else if (strcasecmp (optarg, "TLSv1") == 0)
                                param.ssl_protocol = 4;
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                            else if (strcasecmp (optarg, "TLSv1_3") == 0)
                                param.ssl_protocol = 5;
#endif
                            else
                            {
                                fprintf (stderr, "%s: illegal SSL protocol %s\n",
                                        prog_name, optarg);
                                exit (1);
                            }
                        }
#endif
			else if (flag == &param.uri)
				param.uri = optarg;
			else if (flag == &param.think_timeout) {
				errno = 0;
				param.think_timeout = strtod(optarg, &end);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal think timeout value %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.timeout) {
				errno = 0;
				param.timeout = strtod(optarg, &end);
				if (errno == ERANGE || end == optarg || *end) {
					fprintf(stderr,
						"%s: illegal connect timeout %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.runtime) {
				errno = 0;
				param.runtime = strtod(optarg, &end);
				if (errno == ERANGE || end == optarg || *end ||
				    param.runtime <= 0.0) {
					fprintf(stderr,
						"%s: illegal runtime value %s\n",
						prog_name, optarg);
					exit(1);
				}
			} else if (flag == &param.wlog) {
				gen[1] = &uri_wlog;	/* XXX fix
							 * me---somehow */

				param.wlog.do_loop = (*optarg == 'y')
				    || (*optarg == 'Y');
				param.wlog.file = optarg + 2;
			} else if (flag == &param.wsess) {
				num_gen = 2;	/* XXX fix me---somehow */
				gen[0] = &wsess;

				stat[num_stats++] = &session_stat;

				errno = 0;
				name = "bad number of sessions (1st param)";
				param.wsess.num_sessions =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wsess_param;
				optarg = end + 1;

				name =
				    "bad number of calls per session (2nd param)";
				if (*end != ',')
					goto bad_wsess_param;
				optarg = end + 1;

				param.wsess.num_calls =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wsess_param;

				name = "bad user think time (3rd param)";
				if (*end != ',')
					goto bad_wsess_param;
				optarg = end + 1;

				param.wsess.think_time = strtod(optarg, &end);
				if (end == optarg || errno == ERANGE
				    || param.wsess.think_time < 0.0)
					goto bad_wsess_param;

				name = "extraneous parameter";
				if (*end) {
				      bad_wsess_param:
					fprintf(stderr,
						"%s: %s in --wsess arg (rest: `%s')",
						prog_name, name, end);
					if (errno)
						fprintf(stderr, ": %s",
							strerror(errno));
					fputc('\n', stderr);
					exit(1);
				}
				session_workload = 1;
			} else if (flag == &param.wsesspage) {
				num_gen = 2;	/* XXX fix me---somehow */
				gen[0] = &wsesspage;

				stat[num_stats++] = &session_stat;

				errno = 0;
				name = "bad number of sessions (1st param)";
				param.wsesspage.num_sessions =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wsesspage_param;
				optarg = end + 1;

				name =
				    "bad number of user requests per session (2nd param)";
				if (*end != ',')
					goto bad_wsesspage_param;
				optarg = end + 1;

				param.wsesspage.num_reqs =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wsesspage_param;

				name = "bad user think time (3rd param)";
				if (*end != ',')
					goto bad_wsesspage_param;
				optarg = end + 1;

				param.wsesspage.think_time =
				    strtod(optarg, &end);
				if (end == optarg || errno == ERANGE
				    || param.wsesspage.think_time < 0.0)
					goto bad_wsesspage_param;

				name = "extraneous parameter";
				if (*end) {
				      bad_wsesspage_param:
					fprintf(stderr,
						"%s: %s in --wsesspage arg (rest: `%s')",
						prog_name, name, end);
					if (errno)
						fprintf(stderr, ": %s",
							strerror(errno));
					fputc('\n', stderr);
					exit(1);
				}
				session_workload = 1;
			} else if (flag == &param.wsesslog) {
				num_gen = 1;	/* XXX fix me---somehow */
				gen[0] = &wsesslog;

				stat[num_stats++] = &session_stat;

				errno = 0;
				name = "bad number of sessions (1st param)";
				param.wsesslog.num_sessions =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wsesslog_param;
				optarg = end + 1;

				name = "bad user think time (2nd param)";
				if (*end != ',')
					goto bad_wsesslog_param;
				optarg = end + 1;

				param.wsesslog.think_time =
				    strtod(optarg, &end);
				if (end == optarg || errno == ERANGE
				    || param.wsesslog.think_time < 0.0)
					goto bad_wsesslog_param;

				name = "bad session filename (3rd param)";
				if (*end != ',')
					goto bad_wsesslog_param;
				optarg = end + 1;

				/*
				 * simulate parsing of string 
				 */
				param.wsesslog.file = optarg;
				if ((end = strchr(optarg, ',')) == NULL)
					/*
					 * must be last param, position end at 
					 * final \0 
					 */
					end = optarg + strlen(optarg);
				else
					/*
					 * terminate end of string 
					 */
					*end++ = '\0';
				optarg = end;

				name = "extraneous parameter";
				if (*end) {
				      bad_wsesslog_param:
					fprintf(stderr,
						"%s: %s in --wsesslog arg (rest: `%s')",
						prog_name, name, end);
					if (errno)
						fprintf(stderr, ": %s",
							strerror(errno));
					fputc('\n', stderr);
					exit(1);
				}
				session_workload = 1;
			} else if (flag == &param.wset) {
				gen[1] = &uri_wset;	/* XXX fix
							 * me---somehow */

				errno = 0;
				name = "bad working set size (1st parameter)";
				param.wset.num_files =
				    strtoul(optarg, &end, 0);
				if (end == optarg || errno == ERANGE)
					goto bad_wset_param;

				name = "bad target miss rate (2nd parameter)";
				if (*end != ',')
					goto bad_wset_param;
				optarg = end + 1;

				param.wset.target_miss_rate =
				    strtod(optarg, &end);
				if (end == optarg || errno == ERANGE
				    || param.wset.target_miss_rate < 0.0
				    || param.wset.target_miss_rate > 1.0)
					goto bad_wset_param;

				name = "extraneous parameter";
				if (*end) {
				      bad_wset_param:
					fprintf(stderr,
						"%s: %s in --wset arg (rest: `%s')",
						prog_name, name, optarg);
					if (errno)
						fprintf(stderr, ": %s",
							strerror(errno));
					fputc('\n', stderr);
					exit(1);
				}
			}
			break;

		case 'd':
#ifdef DEBUG
			errno = 0;
			debug_level = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stderr, "%s: illegal debug level %s\n",
					prog_name, optarg);
				exit(1);
			}
#else
			fprintf(stderr,
				"%s: sorry, need to recompile with -DDEBUG on...\n",
				prog_name);
#endif
			break;

		case 'v':
			++verbose;
			break;

		case 'V':
			printf("%s: httperf-" VERSION " compiled " __DATE__
			       " with"
#ifndef DEBUG
			       "out"
#endif
			       " DEBUG with"
#ifndef TIME_SYSCALLS
			       "out"
#endif
			       " TIME_SYSCALLS.\n", prog_name);
			exit(0);

		case 'n':
			++periodic_stats;
			break;

		case 'h':
			usage();
			exit(0);

		case ':':
			fprintf(stderr,
				"%s: parameter missing for option %s\n",
				prog_name, longopts[longindex].name);
			exit(1);

		case '?':
			/*
			 * Invalid or ambiguous option name or extraneous
			 * parameter. getopt_long () already issued an
			 * explanation to the user, so all we do is call it
			 * quites.  
			 */
			exit(1);

		default:
			fprintf(stderr,
				"%s: getopt_long: unexpected value (%d)\n",
				prog_name, ch);
			exit(1);
		}
	}

	if (param.server != NULL && param.servers != NULL) {
		fprintf(stderr,
			"%s: --server S or --servers file\n",
			prog_name);
		exit(-1);
	}

	if (param.server == NULL && param.servers == NULL)
		param.server = "localhost";

#ifdef HAVE_SSL
	if (param.use_ssl) {
		char            buf[1024];

		if (param.port < 0)
			param.port = 443;

                SSL_library_init ();
		SSL_load_error_strings ();
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
                SSLeay_add_all_algorithms ();
#endif

		switch (param.ssl_protocol)
                {
                    /* 0/auto for highest available */
                    case 0: 
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                    ssl_ctx = SSL_CTX_new (TLS_client_method ()); break;
#else
                    ssl_ctx = SSL_CTX_new (SSLv23_client_method ()); break;
#endif

#ifndef OPENSSL_NO_SSL2
                    /* 2/SSLv2 */
                    case 2: 
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                    ssl_ctx = SSL_CTX_new (TLS_client_method ());
                    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_NO_TLSv1_2 | SSL_OP_NO_TLSv1_3);
break;
#else
                    ssl_ctx = SSL_CTX_new (SSLv2_client_method ()); break;
#endif
#endif

#ifndef OPENSSL_NO_SSL3
                    /* 3/SSLv3 */
                    case 3: 
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                    ssl_ctx = SSL_CTX_new (TLS_client_method ());
                    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_NO_TLSv1_2 | SSL_OP_NO_TLSv1_3);
break;
#else
                    ssl_ctx = SSL_CTX_new (SSLv3_client_method ()); break;
#endif
#endif
                    /* 4/TLSv1 */
                    case 4: 
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                    ssl_ctx = SSL_CTX_new (TLS_client_method ()); 
                    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3); break;
#else
                    ssl_ctx = SSL_CTX_new (TLSv1_client_method ()); break;
#endif

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
                    /* 5/TLSv1_3 */
                    case 5: ssl_ctx = SSL_CTX_new (TLS_client_method ()); 
                    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_NO_TLSv1_2); break;
#endif
                }
      
		if (!ssl_ctx) {
			ERR_print_errors_fp(stderr);
			exit(-1);
		}

		memset(buf, 0, sizeof(buf));
		RAND_seed(buf, sizeof(buf));
                
                /* set server certificate verification */
                if (param.ssl_verify == 1)
                    SSL_CTX_set_verify (ssl_ctx, SSL_VERIFY_PEER, NULL);
                else
                    SSL_CTX_set_verify (ssl_ctx, SSL_VERIFY_NONE, NULL);

                /* set default certificate authority verification path */
                SSL_CTX_set_default_verify_paths (ssl_ctx);

                /* load extra certificate authority files and paths */
                if (param.ssl_ca_file || param.ssl_ca_path)
                {
                    int ssl_err = SSL_CTX_load_verify_locations
                    (ssl_ctx, param.ssl_ca_file, param.ssl_ca_path);
                    if (DBG > 2)
                        fprintf (stderr, "SSL_CTX_load_verify_locations returned %d\n",
                            ssl_err);
                }

                /* if using client SSL authentication, load the certificate file */
                if (param.ssl_cert)
                {
                    int ssl_err = SSL_CTX_use_certificate_file
                        (ssl_ctx, param.ssl_cert, SSL_FILETYPE_PEM);
                    if (DBG > 2)
                        fprintf (stderr, "SSL_CTX_use_certificate_file returned %d\n",
                            ssl_err);
                }

                /* also load the client key */
                if (param.ssl_key)
                {
                    int ssl_err = SSL_CTX_use_PrivateKey_file
                        (ssl_ctx, param.ssl_key, SSL_FILETYPE_PEM);
                    if (DBG > 2)
                        fprintf (stderr, "SSL_CTX_use_PrivateKey_file returned %d\n",
                            ssl_err);
                }

                /* check client certificate and key consistency */
                if (param.ssl_cert && param.ssl_key)
                {
                    int ssl_err = SSL_CTX_check_private_key (ssl_ctx);
                    if (DBG > 2)
                        fprintf (stderr, "SSL_CTX_check_private_key returned %d\n",
                            ssl_err);
                    if (!ssl_err)
                    {
                        fprintf (stderr,
                            "SSL certificate and key failed consistency check\n");
                        exit (1);
                    }
                }      
	}
#endif
	if (param.port < 0)
		param.port = 80;

	if (param.print_reply || param.print_request)
		stat[num_stats++] = &stats_print_reply;

	if (param.session_cookies) {
		if (!session_workload) {
			fprintf(stderr,
				"%s: --session-cookie requires session-oriented "
				"workload (e.g., --wsess)\n", prog_name);
			exit(-1);
		}
		gen[num_gen++] = &sess_cookie;
	}

	if (param.additional_header || param.additional_header_file ||
	    param.method)
		gen[num_gen++] = &misc;

	/*
	 * echo command invocation for logging purposes: 
	 */
	printf("%s", prog_name);
	if (verbose)
		printf(" --verbose");
	switch (param.print_reply) {
	case 0:
		break;
	case PRINT_HEADER:
		printf(" --print-reply=header");
		break;
	case PRINT_BODY:
		printf(" --print-reply=body");
		break;
	default:
		printf(" --print-reply");
		break;
	}
	switch (param.print_request) {
	case 0:
		break;
	case PRINT_HEADER:
		printf(" --print-request=header");
		break;
	case PRINT_BODY:
		printf(" --print-request=body");
		break;
	default:
		printf(" --print-request");
		break;
	}
	if (param.hog)
		printf(" --hog");
	if (param.close_with_reset)
		printf(" --close-with-reset");
	if (param.think_timeout > 0)
		printf(" --think-timeout=%g", param.think_timeout);
	if (param.timeout > 0)
		printf(" --timeout=%g", param.timeout);
	if (param.runtime > 0)
		printf(" --runtime=%g", param.runtime);
	printf(" --client=%u/%u", param.client.id, param.client.num_clients);
	if (param.server)
		printf(" --server=%s", param.server);
	if (param.server_name)
		printf(" --server_name=%s", param.server_name);
	if (param.servers)
		printf(" --servers=%s", param.servers);
	if (param.port)
		printf(" --port=%d", param.port);
	if (param.uri)
		printf(" --uri=%s", param.uri);
	if (param.failure_status)
		printf(" --failure-status=%u", param.failure_status);
	if (param.http_version != 0x10001)
		printf(" --http-version=%u.%u", param.http_version >> 16,
		       param.http_version & 0xffff);
	if (param.max_conns)
		printf(" --max-connections=%lu", param.max_conns);
	if (param.max_piped)
		printf(" --max-piped-calls=%lu", param.max_piped);
	if (param.rate.rate_param > 0.0) {
		switch (param.rate.dist) {
		case DETERMINISTIC:
			/*
			 * for backwards compatibility, continue to use
			 * --rate: 
			 */
			printf(" --rate=%g", param.rate.rate_param);
			break;

		case UNIFORM:
			printf(" --period=u%g,%g",
			       param.rate.min_iat, param.rate.max_iat);
			break;

		case EXPONENTIAL:
			printf(" --period=e%g", param.rate.mean_iat);
			break;

		case VARIABLE:
			{
				int             m;
				printf(" --period=v");
				for (m = 0; m < param.rate.numRates; m++) {
					if (m != 0)
						printf(",");
					printf("%g,%g", param.rate.iat[m],
					       param.rate.duration[m]);
				}
			}
			break;

		default:
			printf("--period=??");
			break;
		}
	}
	printf(" --send-buffer=%lu", param.send_buffer_size);
	if (param.retry_on_failure)
		printf(" --retry-on-failure");
	printf(" --recv-buffer=%lu", param.recv_buffer_size);
	if (param.session_cookies)
		printf(" --session-cookies");
#ifdef HAVE_SSL
	if (param.use_ssl)
		printf(" --ssl");
	if (param.ssl_cipher_list)
		printf(" --ssl-ciphers=%s", param.ssl_cipher_list);
	if (!param.ssl_reuse)
		printf(" --ssl-no-reuse");
        if (param.ssl_cert) printf (" --ssl-cert=%s", param.ssl_cert);
        if (param.ssl_key) printf (" --ssl-key=%s", param.ssl_key);
        if (param.ssl_ca_file) printf (" --ssl-ca-file=%s", param.ssl_ca_file);
        if (param.ssl_ca_path) printf (" --ssl-ca-path=%s", param.ssl_ca_path);
        if (param.ssl_verify) printf (" --ssl-verify");
        switch (param.ssl_protocol)
        {
            case 0: printf (" --ssl-protocol=auto");  break;
#ifndef OPENSSL_NO_SSL2
            case 2: printf (" --ssl-protocol=SSLv2"); break;
#endif
#ifndef OPENSSL_NO_SSL3
            case 3: printf (" --ssl-protocol=SSLv3"); break;
#endif
            case 4: printf (" --ssl-protocol=TLSv1"); break;
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
            case 5: printf (" --ssl-protocol=TLSv1_3"); break;
#endif
        }
#endif
	if (param.additional_header)
		printf(" --add-header='%s'", param.additional_header);
	if (param.additional_header_file)
		printf(" --add-header-file='%s'", param.additional_header_file);
	if (param.method)
		printf(" --method=%s", param.method);
	if (param.use_timer_cache)
		printf(" --use-timer-cache");
	if (param.wsesslog.num_sessions) {
		/*
		 * This overrides any --wsess, --num-conns, --num-calls,
		 * --burst-length and any uri generator 
		 */
		printf(" --wsesslog=%u,%.3f,%s", param.wsesslog.num_sessions,
		       param.wsesslog.think_time, param.wsesslog.file);
	} else if (param.wsesspage.num_sessions) {
		printf(" --wsesspage=%u,%u,%.3f", param.wsesspage.num_sessions,
		       param.wsesspage.num_reqs, param.wsesspage.think_time);
	} else {
		if (param.wsess.num_sessions)
			printf(" --wsess=%u,%u,%.3f", param.wsess.num_sessions,
			       param.wsess.num_calls, param.wsess.think_time);
		else {
			if (param.num_conns)
				printf(" --num-conns=%lu", param.num_conns);
			if (param.num_calls)
				printf(" --num-calls=%lu", param.num_calls);
		}
		if (param.burst_len != 1)
			printf(" --burst-length=%lu", param.burst_len);
		if (param.wset.num_files)
			printf(" --wset=%u,%.3f",
			       param.wset.num_files,
			       param.wset.target_miss_rate);
	}
	if (periodic_stats)
		printf(" --periodic-stats");
	printf("\n");

	if (timer_init() == false) {
		fprintf(stderr,
			"%s: timer_init(): failed initialization (%d)\n",
			prog_name, __LINE__);
		exit(1);
	}
	core_init();

	signal(SIGINT, (void (*)()) core_exit);

	for (i = 0; i < num_stats; ++i)
		(*stat[i]->init) ();
	for (i = 0; i < num_gen; ++i)
		(*gen[i]->init) ();

	/*
	 * Update `now'.  This is to keep things accurate even when some of
	 * the initialization routines take a long time to execute.  
	 */
	timer_now_forced();

	/*
	 * ensure that clients sample rates at different times: 
	 */
	t = (param.client.id + 1.0) * RATE_INTERVAL / param.client.num_clients;
	arg.l = 0;
	timer_schedule(perf_sample, arg, t);
	perf_sample_start = timer_now();

	for (i = 0; i < num_gen; ++i)
		(*gen[i]->start) ();
	for (i = 0; i < num_stats; ++i)
		(*stat[i]->start) ();

	getrusage(RUSAGE_SELF, &test_rusage_start);
	test_time_start = timer_now();
	core_loop();
	test_time_stop = timer_now();
	getrusage(RUSAGE_SELF, &test_rusage_stop);

	for (i = 0; i < num_stats; ++i)
		(*stat[i]->stop) ();
	for (i = 0; i < num_gen; ++i)
		(*gen[i]->stop) ();
	for (i = 0; i < num_stats; ++i)
		(*stat[i]->dump) ();

	timer_free_all();

	return 0;
}
