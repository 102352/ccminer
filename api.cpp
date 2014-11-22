/*
 * Copyright 2014 ccminer team
 *
 * Implementation by tpruvot (based on cgminer)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */
#define APIVERSION "1.2"

#ifdef WIN32
# define  _WINSOCK_DEPRECATED_NO_WARNINGS
# include <winsock2.h>
#endif

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/types.h>


#include "compat.h"
#include "miner.h"

#ifdef USE_WRAPNVML
#include "nvml.h"
#endif

#ifndef WIN32
# include <errno.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# define SOCKETTYPE long
# define SOCKETFAIL(a) ((a) < 0)
# define INVSOCK -1 /* INVALID_SOCKET */
# define INVINETADDR -1 /* INADDR_NONE */
# define CLOSESOCKET close
# define SOCKETINIT {}
# define SOCKERRMSG strerror(errno)
#else
# define SOCKETTYPE SOCKET
# define SOCKETFAIL(a) ((a) == SOCKET_ERROR)
# define INVSOCK INVALID_SOCKET
# define INVINETADDR INADDR_NONE
# define CLOSESOCKET closesocket
# define in_addr_t uint32_t
#endif

#define GROUP(g) (toupper(g))
#define PRIVGROUP GROUP('W')
#define NOPRIVGROUP GROUP('R')
#define ISPRIVGROUP(g) (GROUP(g) == PRIVGROUP)
#define GROUPOFFSET(g) (GROUP(g) - GROUP('A'))
#define VALIDGROUP(g) (GROUP(g) >= GROUP('A') && GROUP(g) <= GROUP('Z'))
#define COMMANDS(g) (apigroups[GROUPOFFSET(g)].commands)
#define DEFINEDGROUP(g) (ISPRIVGROUP(g) || COMMANDS(g) != NULL)
struct APIGROUPS {
	// This becomes a string like: "|cmd1|cmd2|cmd3|" so it's quick to search
	char *commands;
} apigroups['Z' - 'A' + 1]; // only A=0 to Z=25 (R: noprivs, W: allprivs)

struct IP4ACCESS {
	in_addr_t ip;
	in_addr_t mask;
	char group;
};

static int ips = 1;
static struct IP4ACCESS *ipaccess = NULL;

// Big enough for largest API request
//  though a PC with 100s of CPUs may exceed the size ...
// Current code assumes it can socket send this size also
#define MYBUFSIZ	16384

#define SOCK_REC_BUFSZ  256

// Socket is on 127.0.0.1
#define QUEUE	10

#define ALLIP4 "0.0.0.0"

static const char *localaddr = "127.0.0.1";
static const char *UNAVAILABLE = " - API will not be available";
static char *buffer = NULL;
static time_t startup = 0;
static int bye = 0;

extern char *opt_api_allow;
extern int opt_api_listen; /* port */
extern uint64_t global_hashrate;
extern uint32_t accepted_count;
extern uint32_t rejected_count;
extern int device_map[8];
extern char *device_name[8];

/***************************************************************/

static void gpustatus(int thr_id)
{
	if (thr_id >= 0 && thr_id < opt_n_threads) {
		struct cgpu_info *cgpu = &thr_info[thr_id].gpu;
		int gpuid = cgpu->gpu_id;
		char buf[512]; *buf = '\0';
		char pstate[8];
		char* card;

#ifdef USE_WRAPNVML
		cgpu->has_monitoring = true;
		cgpu->gpu_bus = gpu_busid(cgpu);
		cgpu->gpu_temp = gpu_temp(cgpu);
		cgpu->gpu_fan = gpu_fanpercent(cgpu);
		cgpu->gpu_pstate = gpu_pstate(cgpu);
#endif
		gpu_clocks(cgpu);

		// todo: can be 0 if set by algo (auto)
		if (opt_intensity == 0 && opt_work_size) {
			int i = 0;
			uint32_t ws = opt_work_size;
			while (ws > 1 && i++ < 32)
				ws = ws >> 1;
			cgpu->intensity = i;
		} else {
			cgpu->intensity = opt_intensity;
		}

		// todo: per gpu
		cgpu->accepted = accepted_count;
		cgpu->rejected = rejected_count;

		cgpu->khashes = stats_get_speed(cgpu->gpu_id, 0.0) / 1000.0;

		memset(pstate, 0, sizeof(pstate));
		if (cgpu->gpu_pstate != -1)
			snprintf(pstate, sizeof(pstate), "P%hu", cgpu->gpu_pstate);

		card = device_name[gpuid];

		snprintf(buf, sizeof(buf), "GPU=%d;BUS=%hd;CARD=%s;"
			"TEMP=%.1f;FAN=%d;FREQ=%d;PST=%s;KHS=%.2f;HWF=%d;I=%d|",
			gpuid, cgpu->gpu_bus, card, cgpu->gpu_temp, cgpu->gpu_fan,
			cgpu->gpu_clock, pstate, cgpu->khashes,
			cgpu->hw_errors, cgpu->intensity);

		// append to buffer for multi gpus
		strcat(buffer, buf);
	}
}

/**
* Returns gpu/thread specific stats
*/
static char *getthreads(char *params)
{
	*buffer = '\0';
	for (int i = 0; i < opt_n_threads; i++)
		gpustatus(i);
	return buffer;
}

/*****************************************************************************/

/**
* Returns miner global infos
*/
static char *getsummary(char *params)
{
	char algo[64]; *algo = '\0';
	time_t ts = time(NULL);
	double uptime = difftime(ts, startup);
	double accps = (60.0 * accepted_count) / (uptime ? uptime : 1.0);

	get_currentalgo(algo, sizeof(algo));

	*buffer = '\0';
	sprintf(buffer, "NAME=%s;VER=%s;API=%s;"
		"ALGO=%s;GPUS=%d;KHS=%.2f;ACC=%d;REJ=%d;"
		"ACCMN=%.3f;DIFF=%.6f;UPTIME=%.0f;TS=%u|",
		PACKAGE_NAME, PACKAGE_VERSION, APIVERSION,
		algo, num_processors, (double)global_hashrate / 1000.0,
		accepted_count, rejected_count,
		accps, global_diff, uptime, (uint32_t) ts);
	return buffer;
}

/*****************************************************************************/

static void gpuhwinfos(int gpu_id)
{
	char buf[256];
	char pstate[8];
	char* card;
	struct cgpu_info *cgpu = NULL;

	for (int g = 0; g < opt_n_threads; g++) {
		if (device_map[g] == gpu_id) {
			cgpu = &thr_info[g].gpu;
			break;
		}
	}

	if (cgpu == NULL)
		return;

#ifdef USE_WRAPNVML
	cgpu->has_monitoring = true;
	cgpu->gpu_bus = gpu_busid(cgpu);
	cgpu->gpu_temp = gpu_temp(cgpu);
	cgpu->gpu_fan = gpu_fanpercent(cgpu);
	cgpu->gpu_pstate = gpu_pstate(cgpu);
	gpu_info(cgpu);
#endif

	gpu_clocks(cgpu);

	memset(pstate, 0, sizeof(pstate));
	if (cgpu->gpu_pstate != -1)
		snprintf(pstate, sizeof(pstate), "P%hu", cgpu->gpu_pstate);

	card = device_name[gpu_id];

	snprintf(buf, sizeof(buf), "GPU=%d;BUS=%hd;CARD=%s;MEM=%lu;"
		"TEMP=%.1f;FAN=%d;FREQ=%d;MEMFREQ=%d;PST=%s;"
		"VID=%hx;PID=%hx;BIOS=%s|",
		gpu_id, cgpu->gpu_bus, card, cgpu->gpu_mem,
		cgpu->gpu_temp, cgpu->gpu_fan, cgpu->gpu_clock, cgpu->gpu_memclock,
		pstate,	cgpu->gpu_vid, cgpu->gpu_pid, cgpu->gpu_desc);

	strcat(buffer, buf);
}

/**
 * To finish
 */
static void cpuhwinfos()
{
	char buf[256];
	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "CPU=|");
	strcat(buffer, buf);
}

/**
 * Returns gpu and system (todo) informations
 */
static char *gethwinfos(char *params)
{
	*buffer = '\0';
	for (int i = 0; i < num_processors; i++)
		gpuhwinfos(i);
	cpuhwinfos();
	return buffer;
}

/**
 * Returns the last 20 scans stats (not the same as shares)
 * optional param thread id (default all)
 */
static char *gethistory(char *params)
{
	struct stats_data data[50];
	int thrid = params ? atoi(params) : -1;
	char *p = buffer;
	*buffer = '\0';
	int records = stats_get_history(thrid, data, ARRAY_SIZE(data));
	for (int i = 0; i < records; i++) {
		time_t ts = data[i].tm_stat;
		p += sprintf(p, "GPU=%d;H=%u;KHS=%.2f;DIFF=%.6f;"
				"COUNT=%u;FOUND=%u;TS=%u|",
			data[i].gpu_id, data[i].height, data[i].hashrate, data[i].difficulty,
			data[i].hashcount, data[i].hashfound, (uint32_t)ts);
	}
	return buffer;
}

/**
 * Some debug infos about memory usage
 */
static char *getmeminfo(char *params)
{
	uint64_t smem, hmem, totmem;
	uint32_t srec, hrec;

	stats_getmeminfo(&smem, &srec);
	hashlog_getmeminfo(&hmem, &hrec);
	totmem = smem + hmem;

	*buffer = '\0';
	sprintf(buffer, "STATS=%u;HASHLOG=%u;MEM=%lu|",
		srec, hrec, totmem);

	return buffer;
}

static char *gethelp(char *params);
struct CMDS {
	const char *name;
	char *(*func)(char *);
} cmds[] = {
	{ "summary", getsummary },
	{ "threads", getthreads },
	{ "histo",   gethistory },
	{ "meminfo", getmeminfo },
	{ "hwinfo",  gethwinfos },
	/* keep it the last */
	{ "help",    gethelp },
};
#define CMDMAX ARRAY_SIZE(cmds)

static char *gethelp(char *params)
{
	*buffer = '\0';
	char * p = buffer;
	for (int i = 0; i < CMDMAX-1; i++)
		p += sprintf(p, "%s\n", cmds[i].name);
	return buffer;
}


static int send_result(SOCKETTYPE c, char *result)
{
	int n;
	if (!result) {
		n = send(c, "", 1, 0);
	} else {
		// ignore failure - it's closed immediately anyway
		n = send(c, result, strlen(result) + 1, 0);
	}
	return n;
}

/*
 * N.B. IP4 addresses are by Definition 32bit big endian on all platforms
 */
static void setup_ipaccess()
{
	char *buf, *ptr, *comma, *slash, *dot;
	int ipcount, mask, octet, i;
	char group;

	buf = (char*) calloc(1, strlen(opt_api_allow) + 1);
	if (unlikely(!buf))
		proper_exit(1);//, "Failed to malloc ipaccess buf");

	strcpy(buf, opt_api_allow);
	ipcount = 1;
	ptr = buf;
	while (*ptr) if (*(ptr++) == ',')
		ipcount++;

	// possibly more than needed, but never less
	ipaccess = (struct IP4ACCESS *) calloc(ipcount, sizeof(struct IP4ACCESS));
	if (unlikely(!ipaccess))
		proper_exit(1);//, "Failed to calloc ipaccess");

	ips = 0;
	ptr = buf;
	while (ptr && *ptr) {
		while (*ptr == ' ' || *ptr == '\t')
			ptr++;

		if (*ptr == ',') {
			ptr++;
			continue;
		}

		comma = strchr(ptr, ',');
		if (comma)
			*(comma++) = '\0';

		group = NOPRIVGROUP;

		if (isalpha(*ptr) && *(ptr+1) == ':') {
			if (DEFINEDGROUP(*ptr))
				group = GROUP(*ptr);
			ptr += 2;
		}

		ipaccess[ips].group = group;

		if (strcmp(ptr, ALLIP4) == 0)
			ipaccess[ips].ip = ipaccess[ips].mask = 0;
		else
		{
			slash = strchr(ptr, '/');
			if (!slash)
				ipaccess[ips].mask = 0xffffffff;
			else {
				*(slash++) = '\0';
				mask = atoi(slash);
				if (mask < 1 || mask > 32)
					goto popipo; // skip invalid/zero

				ipaccess[ips].mask = 0;
				while (mask-- >= 0) {
					octet = 1 << (mask % 8);
					ipaccess[ips].mask |= (octet << (24 - (8 * (mask >> 3))));
				}
			}

			ipaccess[ips].ip = 0; // missing default to '.0'
			for (i = 0; ptr && (i < 4); i++) {
				dot = strchr(ptr, '.');
				if (dot)
					*(dot++) = '\0';
				octet = atoi(ptr);

				if (octet < 0 || octet > 0xff)
					goto popipo; // skip invalid

				ipaccess[ips].ip |= (octet << (24 - (i * 8)));

				ptr = dot;
			}

			ipaccess[ips].ip &= ipaccess[ips].mask;
		}

		ips++;
popipo:
		ptr = comma;
	}

	free(buf);
}

static bool check_connect(struct sockaddr_in *cli, char **connectaddr, char *group)
{
	bool addrok = false;

	*connectaddr = inet_ntoa(cli->sin_addr);

	*group = NOPRIVGROUP;
	if (opt_api_allow) {
		int client_ip = htonl(cli->sin_addr.s_addr);
		for (int i = 0; i < ips; i++) {
			if ((client_ip & ipaccess[i].mask) == ipaccess[i].ip) {
				addrok = true;
				*group = ipaccess[i].group;
				break;
			}
		}
	}
	else
		addrok = (strcmp(*connectaddr, localaddr) == 0);

	return addrok;
}

static void api()
{
	const char *addr = opt_api_allow;
	short int port = opt_api_listen; // 4068
	char buf[MYBUFSIZ];
	int c, n, bound;
	char *connectaddr;
	char *binderror;
	char group;
	time_t bindstart;
	struct sockaddr_in serv;
	struct sockaddr_in cli;
	socklen_t clisiz;
	bool addrok = false;
	long long counter;
	char *result;
	char *params;
	int i;

	SOCKETTYPE *apisock;
	if (!opt_api_listen && opt_debug) {
		applog(LOG_DEBUG, "API disabled");
		return;
	}

	if (opt_api_allow) {
		setup_ipaccess();
		if (ips == 0) {
			applog(LOG_WARNING, "API not running (no valid IPs specified)%s", UNAVAILABLE);
		}
	}

	apisock = (SOCKETTYPE*) calloc(1, sizeof(*apisock));
	*apisock = INVSOCK;

	sleep(1);

	*apisock = socket(AF_INET, SOCK_STREAM, 0);
	if (*apisock == INVSOCK) {
		applog(LOG_ERR, "API initialisation failed (%s)%s", strerror(errno), UNAVAILABLE);
		return;
	}

	memset(&serv, 0, sizeof(serv));
	serv.sin_family = AF_INET;
	serv.sin_addr.s_addr = inet_addr(addr);
	if (serv.sin_addr.s_addr == (in_addr_t)INVINETADDR) {
		applog(LOG_ERR, "API initialisation 2 failed (%s)%s", strerror(errno), UNAVAILABLE);
		return;
	}

	serv.sin_port = htons(port);

#ifndef WIN32
	// On linux with SO_REUSEADDR, bind will get the port if the previous
	// socket is closed (even if it is still in TIME_WAIT) but fail if
	// another program has it open - which is what we want
	int optval = 1;
	// If it doesn't work, we don't really care - just show a debug message
	if (SOCKETFAIL(setsockopt(*apisock, SOL_SOCKET, SO_REUSEADDR, (void *)(&optval), sizeof(optval))))
	        applog(LOG_DEBUG, "API setsockopt SO_REUSEADDR failed (ignored): %s", SOCKERRMSG);
#else
	// On windows a 2nd program can bind to a port>1024 already in use unless
	// SO_EXCLUSIVEADDRUSE is used - however then the bind to a closed port
	// in TIME_WAIT will fail until the timeout - so we leave the options alone
#endif

	// try for 1 minute ... in case the old one hasn't completely gone yet
	bound = 0;
	bindstart = time(NULL);
	while (bound == 0) {
		if (bind(*apisock, (struct sockaddr *)(&serv), sizeof(serv)) < 0) {
			binderror = strerror(errno);
			if ((time(NULL) - bindstart) > 61)
				break;
			else {
				if (!opt_quiet || opt_debug)
					applog(LOG_WARNING, "API bind to port %d failed - trying again in 15sec", port);
				sleep(15);
			}
		}
		else
			bound = 1;
	}

	if (bound == 0) {
		applog(LOG_WARNING, "API bind to port %d failed (%s)%s", port, binderror, UNAVAILABLE);
		free(apisock);
		return;
	}

	if (SOCKETFAIL(listen(*apisock, QUEUE))) {
		applog(LOG_ERR, "API initialisation 3 failed (%s)%s", strerror(errno), UNAVAILABLE);
		CLOSESOCKET(*apisock);
		free(apisock);
		return;
	}

	buffer = (char *) calloc(1, MYBUFSIZ + 1);

	counter = 0;
	while (bye == 0) {
		counter++;

		clisiz = sizeof(cli);
		if (SOCKETFAIL(c = accept(*apisock, (struct sockaddr *)(&cli), &clisiz))) {
			applog(LOG_ERR, "API failed (%s)%s", strerror(errno), UNAVAILABLE);
			CLOSESOCKET(*apisock);
			free(apisock);
			free(buffer);
			return;
		}

		addrok = check_connect(&cli, &connectaddr, &group);
		if (opt_debug && opt_protocol)
			applog(LOG_DEBUG, "API: connection from %s - %s",
				connectaddr, addrok ? "Accepted" : "Ignored");

		if (addrok) {
			bool fail;
			n = recv(c, &buf[0], SOCK_REC_BUFSZ, 0);

			fail = SOCKETFAIL(n);
			if (fail)
				buf[0] = '\0';
			else if (n > 0 && buf[n-1] == '\n') {
				/* telnet compat \r\n */
				buf[n-1] = '\0'; n--;
				if (n > 0 && buf[n-1] == '\r')
					buf[n-1] = '\0';
			}
			buf[n] = '\0';

			//if (opt_debug && opt_protocol && n > 0)
			//	applog(LOG_DEBUG, "API: recv command: (%d) '%s'+char(%x)", n, buf, buf[n-1]);

			if (!fail) {
				params = strchr(buf, '|');
				if (params != NULL)
					*(params++) = '\0';

				if (opt_debug && opt_protocol && n > 0)
					applog(LOG_DEBUG, "API: exec command %s(%s)", buf, params);

				for (i = 0; i < CMDMAX; i++) {
					if (strcmp(buf, cmds[i].name) == 0) {
						result = (cmds[i].func)(params);
						send_result(c, result);
						break;
					}
				}
				CLOSESOCKET(c);
			}
		}
	}

	CLOSESOCKET(*apisock);
	free(apisock);
	free(buffer);
}

/* external access */
void *api_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;

	startup = time(NULL);
	api();

	tq_freeze(mythr->q);

	return NULL;
}
