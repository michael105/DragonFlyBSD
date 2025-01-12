/*
 * Copyright 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 1995, 1996, 1997, 1998, 1999
 * The Internet Software Consortium.    All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The Internet Software Consortium nor the names
 *    of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INTERNET SOFTWARE CONSORTIUM AND
 * CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE INTERNET SOFTWARE CONSORTIUM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This software has been written for the Internet Software Consortium
 * by Ted Lemon <mellon@fugue.com> in cooperation with Vixie
 * Enterprises.  To learn more about the Internet Software Consortium,
 * see ``http://www.vix.com/isc''.  To learn more about Vixie
 * Enterprises, see ``http://www.vix.com''.
 *
 * This client was substantially modified and enhanced by Elliot Poger
 * for use on Linux while he was working on the MosquitoNet project at
 * Stanford.
 *
 * The current version owes much to Elliot's Linux enhancements, but
 * was substantially reorganized and partially rewritten by Ted Lemon
 * so as to use the same networking framework that the Internet Software
 * Consortium DHCP server uses.   Much system-specific configuration code
 * was moved into a shell script so that as support for more operating
 * systems is added, it will not be necessary to port and maintain
 * system-specific configuration code to these operating systems - instead,
 * the shell script can invoke the native tools to accomplish the same
 * purpose.
 */
#include <sys/ioctl.h>

#include <ctype.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include "dhcpd.h"
#include "privsep.h"

#define	CLIENT_PATH 		"PATH=/usr/bin:/usr/sbin:/bin:/sbin"
#define DEFAULT_LEASE_TIME	43200	/* 12 hours... */
#define TIME_MAX		2147483647
#define POLL_FAILURES		10
#define POLL_FAILURE_WAIT	1	/* Back off multiplier (seconds) */

char *path_dhclient_conf = _PATH_DHCLIENT_CONF;
char *path_dhclient_db = NULL;
char *orig_ifname;

int log_perror = 1;
int privfd;
int nullfd = -1;
int no_daemon;
int stayalive = 0;
int unknown_ok = 1;
int routefd = -1;
pid_t monitor_pid;

struct iaddr iaddr_broadcast = { 4, { 255, 255, 255, 255 } };
struct in_addr inaddr_any;
struct sockaddr_in sockaddr_broadcast;

struct interface_info *ifi;
struct client_state *client;
struct client_config *config;

int		findproto(char *, int);
struct sockaddr	*get_ifa(char *, int);
void		usage(void) __dead2;
int		check_option(struct client_lease *l, int option);
int		check_classless_option(unsigned char *data, int len);
int		ipv4addrs(char * buf);
int		res_hnok(const char *dn);
char		*option_as_string(unsigned int code, unsigned char *data, int len);
int		fork_privchld(int, int);
void		get_ifname(char *, char *);
static void	sig_handle(int sig);
static int	killclient(int fd);

time_t	scripttime;
static FILE *leaseFile;

int
findproto(char *cp, int n)
{
	struct sockaddr *sa;
	unsigned int i;

	if (n == 0)
		return -1;
	for (i = 1; i; i <<= 1) {
		if (i & n) {
			sa = (struct sockaddr *)cp;
			switch (i) {
			case RTA_IFA:
			case RTA_DST:
			case RTA_GATEWAY:
			case RTA_NETMASK:
				if (sa->sa_family == AF_INET)
					return AF_INET;
				if (sa->sa_family == AF_INET6)
					return AF_INET6;
				break;
			case RTA_IFP:
				break;
			}
			RT_ADVANCE(cp, sa);
		}
	}
	return (-1);
}

struct sockaddr *
get_ifa(char *cp, int n)
{
	struct sockaddr *sa;
	int i;

	if (n == 0)
		return (NULL);
	for (i = 1; i; i <<= 1)
		if (i & n) {
			sa = (struct sockaddr *)cp;
			if (i == RTA_IFA)
				return (sa);
			RT_ADVANCE(cp, sa);
		}

	return (NULL);
}
struct iaddr defaddr = { .len = 4 }; /* NULL is for silence warnings */

void
routehandler(void)
{
	int linkstat;
	char msg[2048];
	struct rt_msghdr *rtm;
	struct if_msghdr *ifm;
	struct ifa_msghdr *ifam;
	struct if_announcemsghdr *ifan;
	struct client_lease *l;
	struct sockaddr *sa;
	struct iaddr a;
	ssize_t n;
	char *errmsg, buf[64];

	do {
		n = read(routefd, &msg, sizeof(msg));
	} while (n == -1 && errno == EINTR);

	rtm = (struct rt_msghdr *)msg;
	if (n < sizeof(rtm->rtm_msglen) || n < rtm->rtm_msglen ||
	    rtm->rtm_version != RTM_VERSION)
		return;

	switch (rtm->rtm_type) {
	case RTM_NEWADDR:
		ifam = (struct ifa_msghdr *)rtm;
		if (ifam->ifam_index != ifi->index)
			break;
		if (findproto((char *)(ifam + 1), ifam->ifam_addrs) != AF_INET)
			break;
		sa = get_ifa((char *)(ifam + 1), ifam->ifam_addrs);
		if (sa == NULL) {
			errmsg = "sa == NULL";
			goto die;
		}

		if ((a.len = sizeof(struct in_addr)) > sizeof(a.iabuf))
			error("king bula sez: len mismatch");
		memcpy(a.iabuf, &((struct sockaddr_in *)sa)->sin_addr, a.len);
		if (addr_eq(a, defaddr))
			break;

		/* state_panic() can try unexpired existing leases */
		if (client->active && addr_eq(a, client->active->address))
			break;
		for (l = client->leases; l != NULL; l = l->next)
			if (addr_eq(a, l->address))
				break;

		if (l != NULL)
			/* new addr is the one we set */
			break;
		snprintf(buf, sizeof(buf), "%s: %s",
		    "new address not one we set", piaddr(a));
		errmsg = buf;
		goto die;
	case RTM_DELADDR:
		ifam = (struct ifa_msghdr *)rtm;
		if (ifam->ifam_index != ifi->index)
			break;
		if (findproto((char *)(ifam + 1), ifam->ifam_addrs) != AF_INET)
			break;
		/* XXX check addrs like RTM_NEWADDR instead of this? */
		if (scripttime == 0 || time(NULL) < scripttime + 10)
			break;
		errmsg = "interface address deleted";
		goto die;
	case RTM_IFINFO:
		ifm = (struct if_msghdr *)rtm;
		if (ifm->ifm_index != ifi->index)
			break;
		if ((rtm->rtm_flags & RTF_UP) == 0) {
			errmsg = "interface down";
			goto die;
		}

		linkstat =
		    LINK_STATE_IS_UP(ifm->ifm_data.ifi_link_state) ? 1 : 0;
		if (linkstat != ifi->linkstat) {
#ifdef DEBUG
			debug("link state %s -> %s",
			    ifi->linkstat ? "up" : "down",
			    linkstat ? "up" : "down");
#endif
			ifi->linkstat = interface_status(ifi->name);
			if (ifi->linkstat) {
				client->state = S_REBOOTING;
				state_reboot();
			}
		}
		break;
	case RTM_IFANNOUNCE:
		ifan = (struct if_announcemsghdr *)rtm;
		if (ifan->ifan_what == IFAN_DEPARTURE &&
		    ifan->ifan_index == ifi->index) {
			errmsg = "interface departure";
			goto die;
		}
		break;
	default:
		break;
	}
	return;

die:
	script_init("FAIL");
	script_go();
	error("routehandler: %s", errmsg);
}

int
main(int argc, char *argv[])
{
	int ch, fd;
	int pipe_fd[2];
	int quiet = 0;
	int dokillclient = 0;
	int i;
	struct passwd *pw;

	/* Initially, log errors to stderr as well as to syslogd. */
	openlog(getprogname(), LOG_PID | LOG_NDELAY, DHCPD_LOG_FACILITY);
	setlogmask(LOG_UPTO(LOG_INFO));

	signal(SIGINT, sig_handle);
	signal(SIGHUP, sig_handle);

	while ((ch = getopt(argc, argv, "c:dl:quwx")) != -1) {
		switch (ch) {
		case 'c':
			path_dhclient_conf = optarg;
			break;
		case 'd':
			no_daemon = 1;
			break;
		case 'l':
			path_dhclient_db = optarg;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'u':
			unknown_ok = 0;
			break;
		case 'w':
			stayalive = 1;
			break;
		case 'x':
			dokillclient = 1;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	/*
	 * dport wpa_supplicant uses 'start ifc' instead of 'ifc', allow
	 * the 'start' keyword.
	 */
	if (argc > 1) {
		if (strcmp(argv[0], "start") == 0) {
			--argc;
			++argv;
			stayalive = 1;
		} else if (strcmp(argv[0], "stop") == 0) {
			dokillclient = 1;
			--argc;
			++argv;
		}
	}

	if (argc != 1)
		usage();
	orig_ifname = argv[0];

	if (dokillclient) {
		char buf[256];

		snprintf(buf, sizeof(buf),
			 "/var/run/dhclient.%s.pid", orig_ifname);
		fd = open(buf, O_RDWR, 0644);
		if (fd < 0 || killclient(fd)) {
			fprintf(stderr,
				"no dhclient running on %s\n",
				orig_ifname);
		} else {
			fprintf(stderr,
				"stopping dhclient on %s\n",
				orig_ifname);
		}
		if (fd >= 0)
			close(fd);
		exit(1);
	}

	if ((nullfd = open(_PATH_DEVNULL, O_RDWR, 0)) == -1)
		error("cannot open %s: %m", _PATH_DEVNULL);

	/*
	 * If asked to stay alive forever get our daemon going right now
	 * Then set up to fork/monitor and refork on exit.
	 *
	 * When I say 'forever' I really mean it.  If there are configuration
	 * problems or missing interfaces or whatever, dhclient will wait
	 * 10 seconds and try again.
	 */
	if (stayalive) {
		pid_t pid;
		pid_t rpid;
		int omask;

		go_daemon();

		for (;;) {
			omask = sigblock(sigmask(SIGINT) | sigmask(SIGHUP));
			pid = fork();
			if (pid > 0)
				monitor_pid = pid;
			sigsetmask(omask);

			if (pid == 0)	/* child falls out of loop */
				break;
			while (pid > 0) {
				rpid = waitpid(pid, NULL, 0);
				if (rpid == pid)
					break;
				if (rpid != EINTR)
					break;
			}
			sleep(10);
		}
	}

	ifi = calloc(1, sizeof(*ifi));
	if (ifi == NULL)
		error("ifi calloc");
	client = calloc(1, sizeof(*client));
	if (client == NULL)
		error("client calloc");
	config = calloc(1, sizeof(*config));
	if (config == NULL)
		error("config calloc");

	get_ifname(ifi->name, argv[0]);

	if (path_dhclient_db == NULL && asprintf(&path_dhclient_db, "%s.%s",
	    _PATH_DHCLIENT_DB, ifi->name) == -1)
		error("asprintf");

	if (quiet)
		log_perror = 0;

	tzset();

	memset(&sockaddr_broadcast, 0, sizeof(sockaddr_broadcast));
	sockaddr_broadcast.sin_family = AF_INET;
	sockaddr_broadcast.sin_port = htons(REMOTE_PORT);
	sockaddr_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
	sockaddr_broadcast.sin_len = sizeof(sockaddr_broadcast);
	inaddr_any.s_addr = INADDR_ANY;

	read_client_conf();

	if (interface_status(ifi->name) == 0) {
		interface_link_forceup(ifi->name);
		/* Give it up to 4 seconds of silent grace to find link */
		i = -4;
	} else {
		i = 0;
	}

	while (!(ifi->linkstat = interface_status(ifi->name))) {
		if (i == 0)
			fprintf(stderr, "%s: no link ...", ifi->name);
		else if (i > 0)
			fprintf(stderr, ".");
		fflush(stderr);
		if (++i > config->link_timeout) {
			fprintf(stderr, " sleeping\n");
			goto dispatch;
		}
		sleep(1);
	}
	if (i > 0)
		fprintf(stderr, " got link\n");

 dispatch:
	if ((pw = getpwnam("_dhcp")) == NULL)
		error("no such user: _dhcp");

	if (pipe(pipe_fd) == -1)
		error("pipe");

	go_daemon();
	fork_privchld(pipe_fd[0], pipe_fd[1]);

	close(pipe_fd[0]);
	privfd = pipe_fd[1];

	if ((fd = open(path_dhclient_db, O_RDONLY|O_EXLOCK|O_CREAT, 0)) == -1)
		error("can't open and lock %s: %m", path_dhclient_db);
	read_client_leases();
	if ((leaseFile = fopen(path_dhclient_db, "w")) == NULL)
		error("can't open %s: %m", path_dhclient_db);
	rewrite_client_leases();
	close(fd);

	if ((routefd = socket(PF_ROUTE, SOCK_RAW, 0)) == -1)
		error("socket(PF_ROUTE, SOCK_RAW): %m");

	/* set up the interface */
	discover_interface();

	if (chroot(_PATH_VAREMPTY) == -1)
		error("chroot");
	if (chdir("/") == -1)
		error("chdir(\"/\")");

	if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) == -1)
		error("setresgid");
	if (setgroups(1, &pw->pw_gid) == -1)
		error("setgroups");
	if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) == -1)
		error("setresuid");

	endpwent();

	setproctitle("%s", ifi->name);

	if (ifi->linkstat) {
		client->state = S_REBOOTING;
		state_reboot();
	}
	dispatch();

	/* not reached */
	return (0);
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-dqu] [-c file] [-l file] interface\n",
	    getprogname());
	exit(1);
}

/*
 * Individual States:
 *
 * Each routine is called from the dhclient_state_machine() in one of
 * these conditions:
 * -> entering INIT state
 * -> recvpacket_flag == 0: timeout in this state
 * -> otherwise: received a packet in this state
 *
 * Return conditions as handled by dhclient_state_machine():
 * Returns 1, sendpacket_flag = 1: send packet, reset timer.
 * Returns 1, sendpacket_flag = 0: just reset the timer (wait for a milestone).
 * Returns 0: finish the nap which was interrupted for no good reason.
 *
 * Several per-interface variables are used to keep track of the process:
 *   active_lease: the lease that is being used on the interface
 *                 (null pointer if not configured yet).
 *   offered_leases: leases corresponding to DHCPOFFER messages that have
 *                   been sent to us by DHCP servers.
 *   acked_leases: leases corresponding to DHCPACK messages that have been
 *                 sent to us by DHCP servers.
 *   sendpacket: DHCP packet we're trying to send.
 *   destination: IP address to send sendpacket to
 * In addition, there are several relevant per-lease variables.
 *   T1_expiry, T2_expiry, lease_expiry: lease milestones
 * In the active lease, these control the process of renewing the lease;
 * In leases on the acked_leases list, this simply determines when we
 * can no longer legitimately use the lease.
 */
void
state_reboot(void)
{
	/* Cancel all timeouts, since a link state change gets us here
	   and can happen anytime. */
	cancel_timeout();

	/* If we don't remember an active lease, go straight to INIT. */
	if (!client->active || client->active->is_bootp) {
		client->state = S_INIT;
		state_init();
		return;
	}

	/* make_request doesn't initialize xid because it normally comes
	   from the DHCPDISCOVER, but we haven't sent a DHCPDISCOVER,
	   so pick an xid now. */
	client->xid = arc4random();

	/* Make a DHCPREQUEST packet, and set appropriate per-interface
	   flags. */
	make_request(client->active);
	client->destination = iaddr_broadcast;
	client->first_sending = time(NULL);
	client->interval = 0;

	send_request();
}

/*
 * Called when a lease has completely expired and we've
 * been unable to renew it.
 */
void
state_init(void)
{
	/* Make a DHCPDISCOVER packet, and set appropriate per-interface
	   flags. */
	make_discover(client->active);
	client->xid = client->packet.xid;
	client->destination = iaddr_broadcast;
	client->state = S_SELECTING;
	client->first_sending = time(NULL);
	client->interval = 0;

	send_discover();
}

/*
 * state_selecting is called when one or more DHCPOFFER packets
 * have been received and a configurable period of time has passed.
 */
void
state_selecting(void)
{
	struct client_lease *lp, *next, *picked;
	time_t cur_time;

	/* Cancel state_selecting and send_discover timeouts, since either
	   one could have got us here. */
	cancel_timeout();

	/* We have received one or more DHCPOFFER packets.   Currently,
	   the only criterion by which we judge leases is whether or
	   not we get a response when we arp for them. */
	picked = NULL;
	for (lp = client->offered_leases; lp; lp = next) {
		next = lp->next;

		if (!picked) {
			picked = lp;
		} else {
			make_decline(lp);
			send_decline();
			free_client_lease(lp);
		}
	}
	client->offered_leases = NULL;

	/* If we just tossed all the leases we were offered, go back
	   to square one. */
	if (!picked) {
		client->state = S_INIT;
		state_init();
		return;
	}
	picked->next = NULL;

	time(&cur_time);

	/* If it was a BOOTREPLY, we can just take the address right now. */
	if (!picked->options[DHO_DHCP_MESSAGE_TYPE].len) {
		client->new = picked;

		/* Make up some lease expiry times
		   XXX these should be configurable. */
		client->new->expiry = cur_time + 12000;
		client->new->renewal += cur_time + 8000;
		client->new->rebind += cur_time + 10000;

		client->state = S_REQUESTING;

		/* Bind to the address we received. */
		bind_lease();
		return;
	}

	/* Go to the REQUESTING state. */
	client->destination = iaddr_broadcast;
	client->state = S_REQUESTING;
	client->first_sending = cur_time;
	client->interval = 0;

	/* Make a DHCPREQUEST packet from the lease we picked. */
	make_request(picked);
	client->xid = client->packet.xid;

	/* Toss the lease we picked - we'll get it back in a DHCPACK. */
	free_client_lease(picked);

	send_request();
}

void
dhcpack(struct iaddr client_addr, struct option_data *options)
{
	struct client_lease *lease;
	time_t cur_time;


	if (client->state != S_REBOOTING &&
	    client->state != S_REQUESTING &&
	    client->state != S_RENEWING &&
	    client->state != S_REBINDING)
		return;


	lease = packet_to_lease(options);
	if (!lease) {
		note("packet_to_lease failed.");
		return;
	}

	client->new = lease;

	/* Stop resending DHCPREQUEST. */
	cancel_timeout();

	/* Figure out the lease time. */
	if (client->new->options[DHO_DHCP_LEASE_TIME].data)
		client->new->expiry =
		    getULong(client->new->options[DHO_DHCP_LEASE_TIME].data);
	else
		client->new->expiry = DEFAULT_LEASE_TIME;
	/* A number that looks negative here is really just very large,
	   because the lease expiry offset is unsigned. */
	if (client->new->expiry < 0)
		client->new->expiry = TIME_MAX;
	/* XXX should be fixed by resetting the client state */
	if (client->new->expiry < 60)
		client->new->expiry = 60;

	/* Take the server-provided renewal time if there is one;
	   otherwise figure it out according to the spec. */
	if (client->new->options[DHO_DHCP_RENEWAL_TIME].len)
		client->new->renewal =
		    getULong(client->new->options[DHO_DHCP_RENEWAL_TIME].data);
	else
		client->new->renewal = client->new->expiry / 2;

	/* Same deal with the rebind time. */
	if (client->new->options[DHO_DHCP_REBINDING_TIME].len)
		client->new->rebind =
		    getULong(client->new->options[DHO_DHCP_REBINDING_TIME].data);
	else
		client->new->rebind = client->new->renewal +
		    client->new->renewal / 2 + client->new->renewal / 4;

	time(&cur_time);

	client->new->expiry += cur_time;
	/* Lease lengths can never be negative. */
	if (client->new->expiry < cur_time)
		client->new->expiry = TIME_MAX;
	client->new->renewal += cur_time;
	if (client->new->renewal < cur_time)
		client->new->renewal = TIME_MAX;
	client->new->rebind += cur_time;
	if (client->new->rebind < cur_time)
		client->new->rebind = TIME_MAX;

	bind_lease();
}

void
bind_lease(void)
{
	/* Run the client script with the new parameters. */
	script_init((client->state == S_REQUESTING ? "BOUND" :
	    (client->state == S_RENEWING ? "RENEW" :
		(client->state == S_REBOOTING ? "REBOOT" : "REBIND"))));
	if (client->active && client->state != S_REBOOTING)
		script_write_params("old_", client->active);
	script_write_params("new_", client->new);
	script_go();

	/* Replace the old active lease with the new one. */
	if (client->active)
		free_client_lease(client->active);
	client->active = client->new;
	client->new = NULL;

	/* Write out new leases file. */
	rewrite_client_leases();

	/* Set timeout to start the renewal process. */
	set_timeout(client->active->renewal, state_bound);

	note("bound to %s -- renewal in %lld seconds.",
	    piaddr(client->active->address),
	    (long long)(client->active->renewal - time(NULL)));
	client->state = S_BOUND;
}

/*
 * state_bound is called when we've successfully bound to a particular
 * lease, but the renewal time on that lease has expired.   We are
 * expected to unicast a DHCPREQUEST to the server that gave us our
 * original lease.
 */
void
state_bound(void)
{
	/* T1 has expired. */
	make_request(client->active);
	client->xid = client->packet.xid;

	if (client->active->options[DHO_DHCP_SERVER_IDENTIFIER].len == 4) {
		memcpy(client->destination.iabuf,
		    client->active->options[DHO_DHCP_SERVER_IDENTIFIER].data,
		    4);
		client->destination.len = 4;
	} else
		client->destination = iaddr_broadcast;

	client->first_sending = time(NULL);
	client->interval = 0;
	client->state = S_RENEWING;

	send_request();
}

void
dhcpoffer(struct iaddr client_addr, struct option_data *options)
{
	struct client_lease *lease, *lp;
	int i;
	time_t stop_selecting;
	char *name = options[DHO_DHCP_MESSAGE_TYPE].len ? "DHCPOFFER" :
	    "BOOTREPLY";


	if (client->state != S_SELECTING)
		return;


	/* If this lease doesn't supply the minimum required parameters,
	   blow it off. */
	for (i = 0; config->required_options[i]; i++) {
		if (!options[config->required_options[i]].len) {
			note("%s isn't satisfactory.", name);
			return;
		}
	}

	/* If we've already seen this lease, don't record it again. */
	for (lease = client->offered_leases;
	    lease; lease = lease->next) {
		if (lease->address.len == sizeof(client->packet.yiaddr) &&
		    !memcmp(lease->address.iabuf,
		    &client->packet.yiaddr, lease->address.len)) {
#ifdef DEBUG
			debug("%s already seen.", name);
#endif
			return;
		}
	}

	lease = packet_to_lease(options);
	if (!lease) {
		note("packet_to_lease failed.");
		return;
	}

	/*
	 * Reject offers whose subnet is already configured on another
	 * interface.
	 */
	if (subnet_exists(lease))
		return;

	/* If this lease was acquired through a BOOTREPLY, record that
	   fact. */
	if (!options[DHO_DHCP_MESSAGE_TYPE].len)
		lease->is_bootp = 1;

	/* Figure out when we're supposed to stop selecting. */
	stop_selecting = client->first_sending + config->select_interval;

	/* If this is the lease we asked for, put it at the head of the
	   list, and don't mess with the arp request timeout. */
	if (addr_eq(lease->address, client->requested_address)) {
		lease->next = client->offered_leases;
		client->offered_leases = lease;
	} else {
		/* Put the lease at the end of the list. */
		lease->next = NULL;
		if (!client->offered_leases)
			client->offered_leases = lease;
		else {
			for (lp = client->offered_leases; lp->next;
			    lp = lp->next)
				;	/* nothing */
			lp->next = lease;
		}
	}

	/* If the selecting interval has expired, go immediately to
	   state_selecting().  Otherwise, time out into
	   state_selecting at the select interval. */
	if (stop_selecting <= time(NULL))
		state_selecting();
	else {
		set_timeout(stop_selecting, state_selecting);
	}
}

/*
 * Allocate a client_lease structure and initialize it from the
 * parameters in the specified packet.
 */
struct client_lease *
packet_to_lease(struct option_data *options)
{
	struct client_lease *lease;
	int i;

	lease = malloc(sizeof(struct client_lease));

	if (!lease) {
		warning("dhcpoffer: no memory to record lease.");
		return (NULL);
	}

	memset(lease, 0, sizeof(*lease));

	/* Copy the lease options. */
	for (i = 0; i < 256; i++) {
		if (options[i].len) {
			lease->options[i] = options[i];
			options[i].data = NULL;
			options[i].len = 0;
			if (!check_option(lease, i)) {
				warning("Invalid lease option - ignoring offer");
				free_client_lease(lease);
				return (NULL);
			}
		}
	}

	lease->address.len = sizeof(client->packet.yiaddr);
	memcpy(lease->address.iabuf, &client->packet.yiaddr,
	    lease->address.len);

	/* If the server name was filled out, copy it. */
	if ((!lease->options[DHO_DHCP_OPTION_OVERLOAD].len ||
	    !(lease->options[DHO_DHCP_OPTION_OVERLOAD].data[0] & 2)) &&
	    client->packet.sname[0]) {
		lease->server_name = malloc(DHCP_SNAME_LEN + 1);
		if (!lease->server_name) {
			warning("dhcpoffer: no memory for server name.");
			free_client_lease(lease);
			return (NULL);
		}
		memcpy(lease->server_name, client->packet.sname,
		    DHCP_SNAME_LEN);
		lease->server_name[DHCP_SNAME_LEN] = '\0';
		if (!res_hnok(lease->server_name)) {
			warning("Bogus server name %s", lease->server_name);
			free(lease->server_name);
			lease->server_name = NULL;
		}
	}

	/* Ditto for the filename. */
	if ((!lease->options[DHO_DHCP_OPTION_OVERLOAD].len ||
	    !(lease->options[DHO_DHCP_OPTION_OVERLOAD].data[0] & 1)) &&
	    client->packet.file[0]) {
		/* Don't count on the NUL terminator. */
		lease->filename = malloc(DHCP_FILE_LEN + 1);
		if (!lease->filename) {
			warning("dhcpoffer: no memory for filename.");
			free_client_lease(lease);
			return (NULL);
		}
		memcpy(lease->filename, client->packet.file, DHCP_FILE_LEN);
		lease->filename[DHCP_FILE_LEN] = '\0';
	}
	return lease;
}

void
dhcpnak(struct iaddr client_addr, struct option_data *options)
{

	if (client->state != S_REBOOTING &&
	    client->state != S_REQUESTING &&
	    client->state != S_RENEWING &&
	    client->state != S_REBINDING)
		return;


	if (!client->active) {
		note("DHCPNAK with no active lease.");
		return;
	}

	free_client_lease(client->active);
	client->active = NULL;

	/* Stop sending DHCPREQUEST packets... */
	cancel_timeout();

	client->state = S_INIT;
	state_init();
}

/*
 * Send out a DHCPDISCOVER packet, and set a timeout to send out another
 * one after the right interval has expired.  If we don't get an offer by
 * the time we reach the panic interval, call the panic function.
 */
void
send_discover(void)
{
	time_t cur_time;
	int interval, increase = 1;

	time(&cur_time);

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - client->first_sending;

	/* If we're past the panic timeout, call the script and tell it
	   we haven't found anything for this interface yet. */
	if (interval > config->timeout) {
		state_panic();
		return;
	}

	/*
	 * If we're supposed to increase the interval, do so.  If it's
	 * currently zero (i.e., we haven't sent any packets yet), set
	 * it to initial_interval; otherwise, add to it a random
	 * number between zero and two times itself.  On average, this
	 * means that it will double with every transmission.
	 */
	if (increase) {
		if (!client->interval)
			client->interval = config->initial_interval;
		else {
			client->interval += (arc4random() >> 2) %
			    (2 * client->interval);
		}

		/* Don't backoff past cutoff. */
		if (client->interval > config->backoff_cutoff)
			client->interval = ((config->backoff_cutoff / 2)
				 + ((arc4random() >> 2) %
				    config->backoff_cutoff));
	} else if (!client->interval)
		client->interval = config->initial_interval;

	/* If the backoff would take us to the panic timeout, just use that
	   as the interval. */
	if (cur_time + client->interval >
	    client->first_sending + config->timeout)
		client->interval = (client->first_sending +
			 config->timeout) - cur_time + 1;

	/* Record the number of seconds since we started sending. */
	if (interval < 65536)
		client->packet.secs = htons(interval);
	else
		client->packet.secs = htons(65535);
	client->secs = client->packet.secs;

	note("DHCPDISCOVER on %s to %s port %d interval %ld",
	    ifi->name, inet_ntoa(sockaddr_broadcast.sin_addr),
	    ntohs(sockaddr_broadcast.sin_port), client->interval);

	send_packet(inaddr_any, &sockaddr_broadcast, NULL);

	set_timeout_interval(client->interval, send_discover);
}

/*
 * state_panic gets called if we haven't received any offers in a preset
 * amount of time.   When this happens, we try to use existing leases
 * that haven't yet expired, and failing that, we call the client script
 * and hope it can do something.
 */
void
state_panic(void)
{
	struct client_lease *loop = client->active;
	struct client_lease *lp;
	time_t cur_time;

	note("No DHCPOFFERS received.");

	/* We may not have an active lease, but we may have some
	   predefined leases that we can try. */
	if (!client->active && client->leases)
		goto activate_next;

	/* Run through the list of leases and see if one can be used. */
	time(&cur_time);
	while (client->active) {
		if (client->active->expiry > cur_time) {
			note("Trying recorded lease %s",
			    piaddr(client->active->address));
			/* Run the client script with the existing
			   parameters. */
			script_init("TIMEOUT");
			script_write_params("new_", client->active);

			/* If the old lease is still good and doesn't
			   yet need renewal, go into BOUND state and
			   timeout at the renewal time. */
			if (!script_go()) {
				if (cur_time < client->active->renewal) {
					client->state = S_BOUND;
					note("bound: renewal in %lld seconds.",
					    (long long)(client->active->renewal
					    - cur_time));
					set_timeout(client->active->renewal,
					    state_bound);
				} else {
					client->state = S_BOUND;
					note("bound: immediate renewal.");
					state_bound();
				}
				return;
			}
		}

		/* If there are no other leases, give up. */
		if (!client->leases) {
			client->leases = client->active;
			client->active = NULL;
			break;
		}

activate_next:
		/* Otherwise, put the active lease at the end of the
		   lease list, and try another lease.. */
		for (lp = client->leases; lp->next; lp = lp->next)
			;
		lp->next = client->active;
		if (lp->next)
			lp->next->next = NULL;
		client->active = client->leases;
		client->leases = client->leases->next;

		/* If we already tried this lease, we've exhausted the
		   set of leases, so we might as well give up for
		   now. */
		if (client->active == loop)
			break;
		else if (!loop)
			loop = client->active;
	}

	/* No leases were available, or what was available didn't work, so
	   tell the shell script that we failed to allocate an address,
	   and try again later. */
	note("No working leases in persistent database - sleeping.");
	script_init("FAIL");
	script_go();
	client->state = S_INIT;
	set_timeout_interval(config->retry_interval, state_init);
}

void
send_request(void)
{
	struct sockaddr_in destination;
	struct in_addr from;
	time_t cur_time;
	int interval;

	time(&cur_time);

	/* Figure out how long it's been since we started transmitting. */
	interval = (int)(cur_time - client->first_sending);

	/* If we're in the INIT-REBOOT or REQUESTING state and we're
	   past the reboot timeout, go to INIT and see if we can
	   DISCOVER an address... */
	/* XXX In the INIT-REBOOT state, if we don't get an ACK, it
	   means either that we're on a network with no DHCP server,
	   or that our server is down.  In the latter case, assuming
	   that there is a backup DHCP server, DHCPDISCOVER will get
	   us a new address, but we could also have successfully
	   reused our old address.  In the former case, we're hosed
	   anyway.  This is not a win-prone situation. */
	if ((client->state == S_REBOOTING ||
	    client->state == S_REQUESTING) &&
	    interval > config->reboot_timeout) {
		client->state = S_INIT;
		cancel_timeout();
		state_init();
		return;
	}

	/* If the lease has expired, relinquish the address and go back
	   to the INIT state. */
	if (client->state != S_REQUESTING &&
	    cur_time > client->active->expiry) {
		/* Run the client script with the new parameters. */
		script_init("EXPIRE");
		script_write_params("old_", client->active);
		script_go();

		client->state = S_INIT;
		state_init();
		return;
	}

	/* Do the exponential backoff... */
	if (!client->interval)
		client->interval = config->initial_interval;
	else
		client->interval += ((arc4random() >> 2) %
		    (2 * client->interval));

	/* Don't backoff past cutoff. */
	if (client->interval > config->backoff_cutoff)
		client->interval = ((config->backoff_cutoff / 2) +
		    ((arc4random() >> 2) % client->interval));

	/* If the backoff would take us to the expiry time, just set the
	   timeout to the expiry time. */
	if (client->state != S_REQUESTING && cur_time + client->interval >
	    client->active->expiry)
		client->interval = client->active->expiry - cur_time + 1;

	/* If the lease T2 time has elapsed, or if we're not yet bound,
	   broadcast the DHCPREQUEST rather than unicasting. */
	memset(&destination, 0, sizeof(destination));
	if (client->state == S_REQUESTING ||
	    client->state == S_REBOOTING ||
	    cur_time > client->active->rebind)
		destination.sin_addr.s_addr = INADDR_BROADCAST;
	else
		memcpy(&destination.sin_addr.s_addr, client->destination.iabuf,
		    sizeof(destination.sin_addr.s_addr));
	destination.sin_port = htons(REMOTE_PORT);
	destination.sin_family = AF_INET;
	destination.sin_len = sizeof(destination);

	if (client->state != S_REQUESTING)
		memcpy(&from, client->active->address.iabuf, sizeof(from));
	else
		from.s_addr = INADDR_ANY;

	/* Record the number of seconds since we started sending. */
	if (client->state == S_REQUESTING)
		client->packet.secs = client->secs;
	else {
		if (interval < 65536)
			client->packet.secs = htons(interval);
		else
			client->packet.secs = htons(65535);
	}

	note("DHCPREQUEST on %s to %s port %d", ifi->name,
	    inet_ntoa(destination.sin_addr), ntohs(destination.sin_port));

	send_packet(from, &destination, NULL);

	set_timeout_interval(client->interval, send_request);
}

void
send_decline(void)
{
	note("DHCPDECLINE on %s to %s port %d", ifi->name,
	    inet_ntoa(sockaddr_broadcast.sin_addr),
	    ntohs(sockaddr_broadcast.sin_port));

	send_packet(inaddr_any, &sockaddr_broadcast, NULL);
}

void
make_discover(struct client_lease *lease)
{
	unsigned char discover = DHCPDISCOVER;
	struct option_data options[256];
	int i;

	memset(options, 0, sizeof(options));
	memset(&client->packet, 0, sizeof(client->packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPDISCOVER */
	i = DHO_DHCP_MESSAGE_TYPE;
	options[i].data = &discover;
	options[i].len = sizeof(discover);

	/* Request the options we want */
	i  = DHO_DHCP_PARAMETER_REQUEST_LIST;
	options[i].data = config->requested_options;
	options[i].len = config->requested_option_count;

	/* If we had an address, try to get it again. */
	if (lease) {
		client->requested_address = lease->address;
		i = DHO_DHCP_REQUESTED_ADDRESS;
		options[i].data = lease->address.iabuf;
		options[i].len = lease->address.len;
	} else
		client->requested_address.len = 0;

	/* Send any options requested in the config file. */
	for (i = 0; i < 256; i++)
		if (!options[i].data &&
		    config->send_options[i].data) {
			options[i].data = config->send_options[i].data;
			options[i].len = config->send_options[i].len;
		}

	/* Set up the option buffer to fit in a minimal UDP packet. */
	i = cons_options(options);
	if (i == -1 || client->packet.options[i] != DHO_END)
		error("options do not fit in DHCPDISCOVER packet.");
	client->packet_length = DHCP_FIXED_NON_UDP+i+1;
	if (client->packet_length < BOOTP_MIN_LEN)
		client->packet_length = BOOTP_MIN_LEN;

	client->packet.op = BOOTREQUEST;
	client->packet.htype = ifi->hw_address.htype;
	client->packet.hlen = ifi->hw_address.hlen;
	client->packet.hops = 0;
	client->packet.xid = arc4random();
	client->packet.secs = 0; /* filled in by send_discover. */
	client->packet.flags = 0;

	memset(&client->packet.ciaddr, 0, sizeof(client->packet.ciaddr));
	memset(&client->packet.yiaddr, 0, sizeof(client->packet.yiaddr));
	memset(&client->packet.siaddr, 0, sizeof(client->packet.siaddr));
	memset(&client->packet.giaddr, 0, sizeof(client->packet.giaddr));
	memcpy(client->packet.chaddr, ifi->hw_address.haddr,
	    ifi->hw_address.hlen);
}

void
make_request(struct client_lease * lease)
{
	unsigned char request = DHCPREQUEST;
	struct option_data options[256];
	int i;

	memset(options, 0, sizeof(options));
	memset(&client->packet, 0, sizeof(client->packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPREQUEST */
	i = DHO_DHCP_MESSAGE_TYPE;
	options[i].data = &request;
	options[i].len = sizeof(request);

	/* Request the options we want */
	i = DHO_DHCP_PARAMETER_REQUEST_LIST;
	options[i].data = config->requested_options;
	options[i].len = config->requested_option_count;

	/* If we are requesting an address that hasn't yet been assigned
	   to us, use the DHCP Requested Address option. */
	if (client->state == S_REQUESTING) {
		/* Send back the server identifier... */
		i = DHO_DHCP_SERVER_IDENTIFIER;
		options[i].data = lease->options[i].data;
		options[i].len = lease->options[i].len;
	}
	if (client->state == S_REQUESTING ||
	    client->state == S_REBOOTING) {
		client->requested_address = lease->address;
		i = DHO_DHCP_REQUESTED_ADDRESS;
		options[i].data = lease->address.iabuf;
		options[i].len = lease->address.len;
	} else
		client->requested_address.len = 0;

	/* Send any options requested in the config file. */
	for (i = 0; i < 256; i++)
		if (!options[i].data && config->send_options[i].data) {
			options[i].data = config->send_options[i].data;
			options[i].len = config->send_options[i].len;
		}

	/* Set up the option buffer to fit in a minimal UDP packet. */
	i = cons_options(options);
	if (i == -1 || client->packet.options[i] != DHO_END)
		error("options do not fit in DHCPREQUEST packet.");
	client->packet_length = DHCP_FIXED_NON_UDP+i+1;
	if (client->packet_length < BOOTP_MIN_LEN)
		client->packet_length = BOOTP_MIN_LEN;

	client->packet.op = BOOTREQUEST;
	client->packet.htype = ifi->hw_address.htype;
	client->packet.hlen = ifi->hw_address.hlen;
	client->packet.hops = 0;
	client->packet.xid = client->xid;
	client->packet.secs = 0; /* Filled in by send_request. */
	client->packet.flags = 0;

	/* If we own the address we're requesting, put it in ciaddr;
	   otherwise set ciaddr to zero. */
	if (client->state == S_BOUND ||
	    client->state == S_RENEWING ||
	    client->state == S_REBINDING) {
		memcpy(&client->packet.ciaddr,
		    lease->address.iabuf, lease->address.len);
	} else {
		memset(&client->packet.ciaddr, 0,
		    sizeof(client->packet.ciaddr));
	}

	memset(&client->packet.yiaddr, 0, sizeof(client->packet.yiaddr));
	memset(&client->packet.siaddr, 0, sizeof(client->packet.siaddr));
	memset(&client->packet.giaddr, 0, sizeof(client->packet.giaddr));
	memcpy(client->packet.chaddr, ifi->hw_address.haddr,
	    ifi->hw_address.hlen);
}

void
make_decline(struct client_lease *lease)
{
	struct option_data options[256];
	unsigned char decline = DHCPDECLINE;
	int i;

	memset(options, 0, sizeof(options));
	memset(&client->packet, 0, sizeof(client->packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPDECLINE */
	i = DHO_DHCP_MESSAGE_TYPE;
	options[i].data = &decline;
	options[i].len = sizeof(decline);

	/* Send back the server identifier... */
	i = DHO_DHCP_SERVER_IDENTIFIER;
	options[i].data = lease->options[i].data;
	options[i].len = lease->options[i].len;

	/* Send back the address we're declining. */
	i = DHO_DHCP_REQUESTED_ADDRESS;
	options[i].data = lease->address.iabuf;
	options[i].len = lease->address.len;

	/* Send the uid if the user supplied one. */
	i = DHO_DHCP_CLIENT_IDENTIFIER;
	if (config->send_options[i].len) {
		options[i].data = config->send_options[i].data;
		options[i].len = config->send_options[i].len;
	}

	/* Set up the option buffer to fit in a minimal UDP packet. */
	i = cons_options(options);
	if (i == -1 || client->packet.options[i] != DHO_END)
		error("options do not fit in DHCPDECLINE packet.");
	client->packet_length = DHCP_FIXED_NON_UDP+i+1;
	if (client->packet_length < BOOTP_MIN_LEN)
		client->packet_length = BOOTP_MIN_LEN;

	client->packet.op = BOOTREQUEST;
	client->packet.htype = ifi->hw_address.htype;
	client->packet.hlen = ifi->hw_address.hlen;
	client->packet.hops = 0;
	client->packet.xid = client->xid;
	client->packet.secs = 0; /* Filled in by send_request. */
	client->packet.flags = 0;

	/* ciaddr must always be zero. */
	memset(&client->packet.ciaddr, 0, sizeof(client->packet.ciaddr));
	memset(&client->packet.yiaddr, 0, sizeof(client->packet.yiaddr));
	memset(&client->packet.siaddr, 0, sizeof(client->packet.siaddr));
	memset(&client->packet.giaddr, 0, sizeof(client->packet.giaddr));
	memcpy(client->packet.chaddr, ifi->hw_address.haddr,
	    ifi->hw_address.hlen);
}

void
free_client_lease(struct client_lease *lease)
{
	int i;

	if (lease->server_name)
		free(lease->server_name);
	if (lease->filename)
		free(lease->filename);
	for (i = 0; i < 256; i++) {
		if (lease->options[i].len)
			free(lease->options[i].data);
	}
	free(lease);
}

void
rewrite_client_leases(void)
{
	struct client_lease *lp;

	if (!leaseFile)	/* XXX */
		error("lease file not open");

	fflush(leaseFile);
	rewind(leaseFile);

	for (lp = client->leases; lp; lp = lp->next) {
		if (client->active && addr_eq(lp->address,
			client->active->address))
			continue;
		write_client_lease(lp);
	}

	if (client->active)
		write_client_lease(client->active);

	fflush(leaseFile);
	ftruncate(fileno(leaseFile), ftello(leaseFile));
	fsync(fileno(leaseFile));
}

void
write_client_lease(struct client_lease *lease)
{
	struct tm *t;
	int i;

	/* If the lease came from the config file, we don't need to stash
	   a copy in the lease database. */
	if (lease->is_static)
		return;

	if (!leaseFile)	/* XXX */
		error("lease file not open");

	fprintf(leaseFile, "lease {\n");
	if (lease->is_bootp)
		fprintf(leaseFile, "  bootp;\n");
	fprintf(leaseFile, "  interface \"%s\";\n", ifi->name);
	fprintf(leaseFile, "  fixed-address %s;\n", piaddr(lease->address));
	if (lease->filename)
		fprintf(leaseFile, "  filename \"%s\";\n", lease->filename);
	if (lease->server_name)
		fprintf(leaseFile, "  server-name \"%s\";\n",
		    lease->server_name);
	for (i = 0; i < 256; i++)
		if (lease->options[i].len)
			fprintf(leaseFile, "  option %s %s;\n",
			    dhcp_options[i].name,
			    pretty_print_option(i, &lease->options[i], 1));

	t = gmtime(&lease->renewal);
	fprintf(leaseFile, "  renew %d %d/%d/%d %02d:%02d:%02d;\n",
	    t->tm_wday, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
	    t->tm_hour, t->tm_min, t->tm_sec);
	t = gmtime(&lease->rebind);
	fprintf(leaseFile, "  rebind %d %d/%d/%d %02d:%02d:%02d;\n",
	    t->tm_wday, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
	    t->tm_hour, t->tm_min, t->tm_sec);
	t = gmtime(&lease->expiry);
	fprintf(leaseFile, "  expire %d %d/%d/%d %02d:%02d:%02d;\n",
	    t->tm_wday, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
	    t->tm_hour, t->tm_min, t->tm_sec);
	fprintf(leaseFile, "}\n");
	fflush(leaseFile);
}

void
script_init(char *reason)
{
	size_t		 len;
	struct imsg_hdr	 hdr;
	struct buf	*buf;

	hdr.code = IMSG_SCRIPT_INIT;
	hdr.len = sizeof(struct imsg_hdr) + sizeof(size_t) + strlen(reason);
	buf = buf_open(hdr.len);

	buf_add(buf, &hdr, sizeof(hdr));
	len = strlen(reason);
	buf_add(buf, &len, sizeof(len));
	buf_add(buf, reason, len);

	buf_close(privfd, buf);
}

void
priv_script_init(char *reason)
{
	client->scriptEnvsize = 100;
	if (client->scriptEnv == NULL)
		client->scriptEnv =
		    calloc(client->scriptEnvsize, sizeof(char *));
	if (client->scriptEnv == NULL)
		error("script_init: no memory for environment");

	client->scriptEnv[0] = strdup(CLIENT_PATH);
	if (client->scriptEnv[0] == NULL)
		error("script_init: no memory for environment");

	client->scriptEnv[1] = NULL;

	script_set_env("", "interface", ifi->name);

	script_set_env("", "reason", reason);
}

void
priv_script_write_params(char *prefix, struct client_lease *lease)
{
	char buf[256];
	struct option_data o;
	int i;

	script_set_env(prefix, "ip_address", piaddr(lease->address));

	if (lease->options[DHO_SUBNET_MASK].len &&
	    (lease->options[DHO_SUBNET_MASK].len <
	    sizeof(lease->address.iabuf))) {
		struct iaddr netmask, subnet, broadcast;

		memcpy(netmask.iabuf, lease->options[DHO_SUBNET_MASK].data,
		    lease->options[DHO_SUBNET_MASK].len);
		netmask.len = lease->options[DHO_SUBNET_MASK].len;

		subnet = subnet_number(lease->address, netmask);
		if (subnet.len) {
			script_set_env(prefix, "network_number",
			    piaddr(subnet));
			if (!lease->options[DHO_BROADCAST_ADDRESS].len) {
				broadcast = broadcast_addr(subnet, netmask);
				if (broadcast.len)
					script_set_env(prefix,
					    "broadcast_address",
					    piaddr(broadcast));
			}
		}
	}

	if (lease->filename)
		script_set_env(prefix, "filename", lease->filename);
	if (lease->server_name)
		script_set_env(prefix, "server_name",
		    lease->server_name);

	for (i = 0; i < 256; i++) {
		if (!dhcp_option_ev_name(buf, sizeof(buf), &dhcp_options[i]))
			continue;

		switch (config->default_actions[i]) {
		case ACTION_IGNORE:
			 break;

		case ACTION_DEFAULT:
			if (lease->options[i].len)
				script_set_env(prefix, buf,
				    pretty_print_option(i, &lease->options[i],
					0));
			else if (config->defaults[i].len)
				script_set_env(prefix, buf,
				    pretty_print_option(i, &config->defaults[i],
					0));
			break;

		case ACTION_SUPERSEDE:
			if (config->defaults[i].len)
				script_set_env(prefix, buf,
				    pretty_print_option(i, &config->defaults[i],
					0));
			break;

		case ACTION_PREPEND:
			o.len = config->defaults[i].len + lease->options[i].len;
			if (o.len > 0) {
				o.data = calloc(1, o.len);
				if (o.data == NULL)
					error("no space to prepend '%s' to %s",
					    config->defaults[i].data,
					    dhcp_options[i].name);
				memcpy(o.data, config->defaults[i].data,
				    config->defaults[i].len);
				memcpy(o.data + config->defaults[i].len,
				    lease->options[i].data,
				    lease->options[i].len);
				script_set_env(prefix, buf,
				    pretty_print_option(i, &o, 0));
				free(o.data);
			}
			break;

		case ACTION_APPEND:
			o.len = config->defaults[i].len + lease->options[i].len;
			if (o.len > 0) {
				o.data = calloc(1, o.len);
				if (o.data == NULL)
					error("no space to append '%s' to %s",
					    config->defaults[i].data,
					    dhcp_options[i].name);
				memcpy(o.data, lease->options[i].data,
				    lease->options[i].len);
				memcpy(o.data + lease->options[i].len,
				    config->defaults[i].data,
				    config->defaults[i].len);
				script_set_env(prefix, buf,
				    pretty_print_option(i, &o, 0));
				free(o.data);
			}
			break;
		}
	}

	snprintf(buf, sizeof(buf), "%d", (int)lease->expiry);
	script_set_env(prefix, "expiry", buf);
}

void
script_write_params(char *prefix, struct client_lease *lease)
{
	size_t		 fn_len = 0, sn_len = 0, pr_len = 0;
	struct imsg_hdr	 hdr;
	struct buf	*buf;
	int		 i;

	if (lease->filename != NULL)
		fn_len = strlen(lease->filename);
	if (lease->server_name != NULL)
		sn_len = strlen(lease->server_name);
	if (prefix != NULL)
		pr_len = strlen(prefix);

	hdr.code = IMSG_SCRIPT_WRITE_PARAMS;
	hdr.len = sizeof(hdr) + sizeof(struct client_lease) +
	    sizeof(size_t) + fn_len + sizeof(size_t) + sn_len +
	    sizeof(size_t) + pr_len;

	for (i = 0; i < 256; i++)
		hdr.len += sizeof(int) + lease->options[i].len;

	scripttime = time(NULL);

	buf = buf_open(hdr.len);

	buf_add(buf, &hdr, sizeof(hdr));
	buf_add(buf, lease, sizeof(struct client_lease));
	buf_add(buf, &fn_len, sizeof(fn_len));
	buf_add(buf, lease->filename, fn_len);
	buf_add(buf, &sn_len, sizeof(sn_len));
	buf_add(buf, lease->server_name, sn_len);
	buf_add(buf, &pr_len, sizeof(pr_len));
	buf_add(buf, prefix, pr_len);

	for (i = 0; i < 256; i++) {
		buf_add(buf, &lease->options[i].len,
		    sizeof(lease->options[i].len));
		buf_add(buf, lease->options[i].data,
		    lease->options[i].len);
	}

	buf_close(privfd, buf);
}

int
script_go(void)
{
	struct imsg_hdr	 hdr;
	struct buf	*buf;
	int		 ret;

	scripttime = time(NULL);

	hdr.code = IMSG_SCRIPT_GO;
	hdr.len = sizeof(struct imsg_hdr);

	buf = buf_open(hdr.len);

	buf_add(buf, &hdr, sizeof(hdr));
	buf_close(privfd, buf);

	bzero(&hdr, sizeof(hdr));
	buf_read(privfd, &hdr, sizeof(hdr));
	if (hdr.code != IMSG_SCRIPT_GO_RET)
		error("unexpected msg type %u", hdr.code);
	if (hdr.len != sizeof(hdr) + sizeof(int))
		error("received corrupted message");
	buf_read(privfd, &ret, sizeof(ret));

	return (ret);
}

int
priv_script_go(void)
{
	char *scriptName, *argv[2], **envp;
	int pid, wpid, wstatus;

	scripttime = time(NULL);

	scriptName = config->script_name;
	envp = client->scriptEnv;

	argv[0] = scriptName;
	argv[1] = NULL;

	pid = fork();
	if (pid < 0) {
		error("fork: %m");
	} else if (pid) {
		do {
			wpid = wait(&wstatus);
		} while (wpid != pid && wpid > 0);
		if (wpid < 0) {
			error("wait: %m");
		}
	} else {
		execve(scriptName, argv, envp);
		error("execve (%s, ...): %m", scriptName);
	}

	script_flush_env();

	return (WEXITSTATUS(wstatus));
}

void
script_set_env(const char *prefix, const char *name, const char *value)
{
	int i, j, namelen;

	/* No `` or $() command substitution allowed in environment values! */
	for (j = 0; j < strlen(value); j++)
		switch (value[j]) {
		case '`':
		case '$':
			warning("illegal character (%c) in value '%s'",
			    value[j], value);
			/* Ignore this option */
			return;
		}

	namelen = strlen(name);

	for (i = 0; client->scriptEnv[i]; i++)
		if (strncmp(client->scriptEnv[i], name, namelen) == 0 &&
		    client->scriptEnv[i][namelen] == '=')
			break;

	if (client->scriptEnv[i])
		/* Reuse the slot. */
		free(client->scriptEnv[i]);
	else {
		/* New variable.  Expand if necessary. */
		if (i >= client->scriptEnvsize - 1) {
			char **newscriptEnv;
			int newscriptEnvsize = client->scriptEnvsize + 50;

			newscriptEnv = realloc(client->scriptEnv,
			    newscriptEnvsize);
			if (newscriptEnv == NULL) {
				free(client->scriptEnv);
				client->scriptEnv = NULL;
				client->scriptEnvsize = 0;
				error("script_set_env: no memory for variable");
			}
			client->scriptEnv = newscriptEnv;
			client->scriptEnvsize = newscriptEnvsize;
		}
		/* need to set the NULL pointer at end of array beyond
		   the new slot. */
		client->scriptEnv[i + 1] = NULL;
	}
	/* Allocate space and format the variable in the appropriate slot. */
	client->scriptEnv[i] = malloc(strlen(prefix) + strlen(name) + 1 +
	    strlen(value) + 1);
	if (client->scriptEnv[i] == NULL)
		error("script_set_env: no memory for variable assignment");
	snprintf(client->scriptEnv[i], strlen(prefix) + strlen(name) +
	    1 + strlen(value) + 1, "%s%s=%s", prefix, name, value);
}

void
script_flush_env(void)
{
	int i;

	for (i = 0; client->scriptEnv[i]; i++) {
		free(client->scriptEnv[i]);
		client->scriptEnv[i] = NULL;
	}
	client->scriptEnvsize = 0;
}

int
dhcp_option_ev_name(char *buf, size_t buflen, const struct option *option)
{
	size_t i;

	for (i = 0; option->name[i]; i++) {
		if (i + 1 == buflen)
			return 0;
		if (option->name[i] == '-')
			buf[i] = '_';
		else
			buf[i] = option->name[i];
	}

	buf[i] = 0;
	return 1;
}

void
go_daemon(void)
{
	char buf[256];
	int fd;

	/*
	 * Only once
	 */
	if (no_daemon == 2)
		return;

	/*
	 * Setup pidfile, kill any dhclient already running for this
	 * interface.
	 */
	snprintf(buf, sizeof(buf), "/var/run/dhclient.%s.pid", orig_ifname);
	fd = open(buf, O_RDWR|O_CREAT, 0644);
	if (fd >= 0) {
		if (killclient(fd)) {
			fprintf(stderr,
				"starting dhclient on %s\n",
				orig_ifname);
		} else {
			fprintf(stderr,
				"restarting dhclient on %s\n",
				orig_ifname);
		}
	}

	/*
	 * Daemonize if requested
	 */
	if (no_daemon == 0) {
		/* Stop logging to stderr... */
		log_perror = 0;

		if (daemon(1, 0) == -1)
			error("daemon");

		/* we are chrooted, daemon(3) fails to open /dev/null */
		if (nullfd != -1) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			close(nullfd);
			nullfd = -1;
		}
	}

	/*
	 * No further daemonizations, write out pid file and lock.
	 */
	no_daemon = 2;
	if (fd >= 0) {
		lseek(fd, 0L, SEEK_SET);
		ftruncate(fd, 0);
		snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
		write(fd, buf, strlen(buf));
		flock(fd, LOCK_EX);
		/* leave descriptor open and locked */
	}
}

int
check_option(struct client_lease *l, int option)
{
	char *opbuf;
	char *sbuf;

	/* we use this, since this is what gets passed to dhclient-script */

	opbuf = pretty_print_option(option, &l->options[option], 0);

	sbuf = option_as_string(option, l->options[option].data,
	    l->options[option].len);

	switch (option) {
	case DHO_SUBNET_MASK:
	case DHO_SWAP_SERVER:
	case DHO_BROADCAST_ADDRESS:
	case DHO_DHCP_SERVER_IDENTIFIER:
	case DHO_ROUTER_SOLICITATION_ADDRESS:
	case DHO_DHCP_REQUESTED_ADDRESS:
		if (ipv4addrs(opbuf) == 0) {
			warning("Invalid IP address in option %s: %s",
			    dhcp_options[option].name, opbuf);
			return (0);
		}
		if (l->options[option].len != 4) { /* RFC 2132 */
			warning("warning: Only 1 IP address allowed in "
			    "%s option; length %d, must be 4",
			    dhcp_options[option].name,
			    l->options[option].len);
			l->options[option].len = 4;
		}
		return (1);
	case DHO_TIME_SERVERS:
	case DHO_NAME_SERVERS:
	case DHO_ROUTERS:
	case DHO_DOMAIN_NAME_SERVERS:
	case DHO_LOG_SERVERS:
	case DHO_COOKIE_SERVERS:
	case DHO_LPR_SERVERS:
	case DHO_IMPRESS_SERVERS:
	case DHO_RESOURCE_LOCATION_SERVERS:
	case DHO_NIS_SERVERS:
	case DHO_NTP_SERVERS:
	case DHO_NETBIOS_NAME_SERVERS:
	case DHO_NETBIOS_DD_SERVER:
	case DHO_FONT_SERVERS:
		if (ipv4addrs(opbuf) == 0) {
			warning("Invalid IP address in option %s: %s",
			    dhcp_options[option].name, opbuf);
			return (0);
		}
		return (1);
	case DHO_HOST_NAME:
	case DHO_DOMAIN_NAME:
	case DHO_NIS_DOMAIN:
		if (!res_hnok(sbuf)) {
			warning("Bogus Host Name option %d: %s (%s)", option,
			    sbuf, opbuf);
			l->options[option].len = 0;
			free(l->options[option].data);
		}
		return (1);
	case DHO_PAD:
	case DHO_TIME_OFFSET:
	case DHO_BOOT_SIZE:
	case DHO_MERIT_DUMP:
	case DHO_ROOT_PATH:
	case DHO_EXTENSIONS_PATH:
	case DHO_IP_FORWARDING:
	case DHO_NON_LOCAL_SOURCE_ROUTING:
	case DHO_POLICY_FILTER:
	case DHO_MAX_DGRAM_REASSEMBLY:
	case DHO_DEFAULT_IP_TTL:
	case DHO_PATH_MTU_AGING_TIMEOUT:
	case DHO_PATH_MTU_PLATEAU_TABLE:
	case DHO_INTERFACE_MTU:
	case DHO_ALL_SUBNETS_LOCAL:
	case DHO_PERFORM_MASK_DISCOVERY:
	case DHO_MASK_SUPPLIER:
	case DHO_ROUTER_DISCOVERY:
	case DHO_STATIC_ROUTES:
	case DHO_TRAILER_ENCAPSULATION:
	case DHO_ARP_CACHE_TIMEOUT:
	case DHO_IEEE802_3_ENCAPSULATION:
	case DHO_DEFAULT_TCP_TTL:
	case DHO_TCP_KEEPALIVE_INTERVAL:
	case DHO_TCP_KEEPALIVE_GARBAGE:
	case DHO_VENDOR_ENCAPSULATED_OPTIONS:
	case DHO_NETBIOS_NODE_TYPE:
	case DHO_NETBIOS_SCOPE:
	case DHO_X_DISPLAY_MANAGER:
	case DHO_DHCP_LEASE_TIME:
	case DHO_DHCP_OPTION_OVERLOAD:
	case DHO_DHCP_MESSAGE_TYPE:
	case DHO_DHCP_PARAMETER_REQUEST_LIST:
	case DHO_DHCP_MESSAGE:
	case DHO_DHCP_MAX_MESSAGE_SIZE:
	case DHO_DHCP_RENEWAL_TIME:
	case DHO_DHCP_REBINDING_TIME:
	case DHO_DHCP_CLASS_IDENTIFIER:
	case DHO_DHCP_CLIENT_IDENTIFIER:
	case DHO_DHCP_USER_CLASS_ID:
	case DHO_TFTP_SERVER:
	case DHO_END:
		return (1);
	case DHO_CLASSLESS_ROUTES:
		return (check_classless_option(l->options[option].data,
					       l->options[option].len));
	default:
		if (!unknown_ok)
			warning("unknown dhcp option value 0x%x", option);
		return (unknown_ok);
	}
}

/* RFC 3442 The Classless Static Routes option checks */
int
check_classless_option(unsigned char *data, int len)
{
	int i = 0;
	unsigned char width;
	in_addr_t addr, mask;

	if (len < 5) {
		warning("Too small length: %d", len);
		return (0);
	}
	while(i < len) {
		width = data[i++];
		if (width == 0) {
			i += 4;
			continue;
		} else if (width < 9) {
			addr =  (in_addr_t)(data[i]     << 24);
			i += 1;
		} else if (width < 17) {
			addr =  (in_addr_t)(data[i]     << 24) +
				(in_addr_t)(data[i + 1] << 16);
			i += 2;
		} else if (width < 25) {
			addr =  (in_addr_t)(data[i]     << 24) +
				(in_addr_t)(data[i + 1] << 16) +
				(in_addr_t)(data[i + 2] << 8);
			i += 3;
		} else if (width < 33) {
			addr =  (in_addr_t)(data[i]     << 24) +
				(in_addr_t)(data[i + 1] << 16) +
				(in_addr_t)(data[i + 2] << 8)  +
				data[i + 3];
			i += 4;
		} else {
			warning("Incorrect subnet width: %d", width);
			return (0);
		}
		mask = (in_addr_t)(~0) << (32 - width);
		addr = ntohl(addr);
		mask = ntohl(mask);

		/*
		 * From RFC 3442:
		 * ... After deriving a subnet number and subnet mask
		 * from each destination descriptor, the DHCP client
		 * MUST zero any bits in the subnet number where the
		 * corresponding bit in the mask is zero...
		 */
		if ((addr & mask) != addr) {
			addr &= mask;
			data[i - 1] = (unsigned char)(
				(addr >> (((32 - width)/8)*8)) & 0xFF);
		}
		i += 4;
	}
	if (i > len) {
		warning("Incorrect data length: %d (must be %d)", len, i);
		return (0);
	}
	return (1);
}

int
res_hnok(const char *name)
{
	const char *dn = name;
	int pch = '.', ch = *dn++;
	int warn = 0;

	while (ch != '\0') {
		int nch = *dn++;

		if (ch == '.') {
			;
		} else if (pch == '.' || nch == '.' || nch == '\0') {
			if (!isalnum(ch))
				return (0);
		} else if (!isalnum(ch) && ch != '-' && ch != '_')
				return (0);
		else if (ch == '_' && warn == 0) {
			warning("warning: hostname %s contains an "
			    "underscore which violates RFC 952", name);
			warn++;
		}
		pch = ch, ch = nch;
	}
	return (1);
}

/* Does buf consist only of dotted decimal ipv4 addrs?
 * return how many if so,
 * otherwise, return 0
 */
int
ipv4addrs(char * buf)
{
	struct in_addr jnk;
	int count = 0;

	while (inet_aton(buf, &jnk) == 1){
		count++;
		while (*buf == '.' || isdigit(*buf))
			buf++;
		if (*buf == '\0')
			return (count);
		while (*buf ==  ' ')
			buf++;
	}
	return (0);
}

char *
option_as_string(unsigned int code, unsigned char *data, int len)
{
	static char optbuf[32768]; /* XXX */
	char *op = optbuf;
	int opleft = sizeof(optbuf);
	unsigned char *dp = data;

	if (code > 255)
		error("option_as_string: bad code %d", code);

	for (; dp < data + len; dp++) {
		if (!isascii(*dp) || !isprint(*dp)) {
			if (dp + 1 != data + len || *dp != 0) {
				size_t oplen;
				snprintf(op, opleft, "\\%03o", *dp);
				oplen = strlen(op);
				op += oplen;
				opleft -= oplen;
			}
		} else if (*dp == '"' || *dp == '\'' || *dp == '$' ||
		    *dp == '`' || *dp == '\\') {
			*op++ = '\\';
			*op++ = *dp;
			opleft -= 2;
		} else {
			*op++ = *dp;
			opleft--;
		}
	}
	if (opleft < 1)
		goto toobig;
	*op = 0;
	return optbuf;
toobig:
	warning("dhcp option too large");
	return "<error>";
}

int
fork_privchld(int fd, int fd2)
{
	struct pollfd pfd[1];
	int nfds, pfail = 0;
	pid_t pid;
	int omask;

	omask = sigblock(sigmask(SIGINT)|sigmask(SIGHUP));
	pid = fork();
	if (pid > 0)
		monitor_pid = pid;
	sigsetmask(omask);

	switch (pid) {
	case -1:
		error("cannot fork");
		break;
	case 0:
		break;
	default:
		return (0);
	}

	if (chdir("/") == -1)
		error("chdir(\"/\")");

	setproctitle("%s [priv]", ifi->name);

	dup2(nullfd, STDIN_FILENO);
	dup2(nullfd, STDOUT_FILENO);
	dup2(nullfd, STDERR_FILENO);
	close(nullfd);
	close(fd2);

	for (;;) {
		pfd[0].fd = fd;
		pfd[0].events = POLLIN;
		if ((nfds = poll(pfd, 1, INFTIM)) == -1)
			if (errno != EINTR)
				error("poll error");

		/*
		 * Handle temporary errors, but bail if they persist.
		 */
		if (nfds == 0 || !(pfd[0].revents & POLLIN)) {
			if (pfail > POLL_FAILURES)
				error("poll failed > %d times", POLL_FAILURES);
			sleep(pfail * POLL_FAILURE_WAIT);
			pfail++;
			continue;
		}

		dispatch_imsg(fd);
	}
}

void
get_ifname(char *ifname, char *arg)
{
	struct ifgroupreq ifgr;
	struct ifg_req *ifg;
	int s, len;

	if (!strcmp(arg, "egress")) {
		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s == -1)
			error("socket error");
		bzero(&ifgr, sizeof(ifgr));
		strlcpy(ifgr.ifgr_name, "egress", sizeof(ifgr.ifgr_name));
		if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) == -1) {
			if (errno == ENOENT)
				error("no interface in group egress found");
			error("ioctl SIOCGIFGMEMB: %m");
		}
		len = ifgr.ifgr_len;
		if ((ifgr.ifgr_groups = calloc(1, len)) == NULL)
			error("get_ifname");
		if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) == -1)
			error("ioctl SIOCGIFGMEMB: %m");

		arg = NULL;
		for (ifg = ifgr.ifgr_groups;
		     ifg && len >= sizeof(struct ifg_req); ifg++) {
			len -= sizeof(struct ifg_req);
			if (arg)
				error("too many interfaces in group egress");
			arg = ifg->ifgrq_member;
		}

		if (strlcpy(ifi->name, arg, IFNAMSIZ) >= IFNAMSIZ)
			error("Interface name too long: %m");

		free(ifgr.ifgr_groups);
		close(s);
	} else if (strlcpy(ifi->name, arg, IFNAMSIZ) >= IFNAMSIZ)
		error("Interface name too long");
}

static
void
sig_handle(int signo)
{
	if (monitor_pid > 0)
		kill(monitor_pid, signo);
	fprintf(stderr, "killed by signal\n");
	exit(1);
}

static
int
killclient(int fd)
{
	int noclient = 1;

	/*
	 * Kill previously running dhclient
	 */
	if (flock(fd, LOCK_EX|LOCK_NB) < 0) {
		char buf[256];
		ssize_t n;
		pid_t pid;

		lseek(fd, 0L, SEEK_SET);
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			buf[n-1] = 0;
			pid = strtol(buf, NULL, 10);
			if ((int)pid > 0) {
				noclient = 0;
				kill(pid, SIGINT);
			}
		}
		if (flock(fd, LOCK_EX|LOCK_NB) < 0)
			usleep(20000);
		while (flock(fd, LOCK_EX|LOCK_NB) < 0)
			sleep(1);
	}
	return noclient;
}
