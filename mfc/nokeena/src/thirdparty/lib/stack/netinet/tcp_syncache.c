/*-
 * Copyright (c) 2001 McAfee, Inc.
 * Copyright (c) 2006 Andre Oppermann, Internet Business Solutions AG
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Jonathan Lemon
 * and McAfee Research, the Security Research Division of McAfee, Inc. under
 * DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"), as part of the
 * DARPA CHATS research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/sys/netinet/tcp_syncache.c,v 1.130.2.6 2007/12/20 12:34:32 ru Exp $");

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipsec.h"
#include "opt_mac.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/md5.h>
#include <sys/proc.h>		/* for proc0 declaration */
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/syslog.h>

#include <vm/uma.h>

#include <net/if.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_var.h>
#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>
#include <netinet/ip_options.h>
#ifdef INET6
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet6/nd6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/in6_pcb.h>
#endif
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_syncache.h>
#ifdef INET6
#include <netinet6/tcp6_var.h>
#endif

#ifdef IPSEC
#include <netipsec/ipsec.h>
#ifdef INET6
#include <netipsec/ipsec6.h>
#endif
#include <netipsec/key.h>
#endif /*IPSEC*/

#include <machine/in_cksum.h>

#include <security/mac/mac_framework.h>

static int tcp_syncookies = 1;
SYSCTL_INT(_net_inet_tcp, OID_AUTO, syncookies, CTLFLAG_RW,
    &tcp_syncookies, 0,
    "Use TCP SYN cookies if the syncache overflows");

static int tcp_syncookiesonly = 0;
SYSCTL_INT(_net_inet_tcp, OID_AUTO, syncookies_only, CTLFLAG_RW,
    &tcp_syncookiesonly, 0,
    "Use only TCP SYN cookies");

#define	SYNCOOKIE_SECRET_SIZE	8	/* dwords */
#define	SYNCOOKIE_LIFETIME	16	/* seconds */

struct syncache {
	TAILQ_ENTRY(syncache)	sc_hash;
	struct		in_conninfo sc_inc;	/* addresses */
	int		sc_rxttime;		/* retransmit time */
	u_int16_t	sc_rxmits;		/* retransmit counter */

	u_int32_t	sc_tsreflect;		/* timestamp to reflect */
	u_int32_t	sc_ts;			/* our timestamp to send */
	u_int32_t	sc_tsoff;		/* ts offset w/ syncookies */
	u_int32_t	sc_flowlabel;		/* IPv6 flowlabel */
	tcp_seq		sc_irs;			/* seq from peer */
	tcp_seq		sc_iss;			/* our ISS */
	struct		mbuf *sc_ipopts;	/* source route */

	u_int16_t	sc_peer_mss;		/* peer's MSS */
	u_int16_t	sc_wnd;			/* advertised window */
	u_int8_t	sc_ip_ttl;		/* IPv4 TTL */
	u_int8_t	sc_ip_tos;		/* IPv4 TOS */
	u_int8_t	sc_requested_s_scale:4,
			sc_requested_r_scale:4;
	u_int8_t	sc_flags;
#define SCF_NOOPT	0x01			/* no TCP options */
#define SCF_WINSCALE	0x02			/* negotiated window scaling */
#define SCF_TIMESTAMP	0x04			/* negotiated timestamps */
						/* MSS is implicit */
#define SCF_UNREACH	0x10			/* icmp unreachable received */
#define SCF_SIGNATURE	0x20			/* send MD5 digests */
#define SCF_SACK	0x80			/* send SACK option */
#ifndef DISABLE_TCP_OFFLOAD
	void		*sc_pspare[2];		/* toepcb / toe_usrreqs */
#endif			
#ifdef MAC
	struct label	*sc_label;		/* MAC label reference */
#endif
};

struct syncache_head {
	struct mtx	sch_mtx;
	TAILQ_HEAD(sch_head, syncache)	sch_bucket;
	struct callout	sch_timer;
	int		sch_nextc;
	u_int		sch_length;
	u_int		sch_oddeven;
	u_int32_t	sch_secbits_odd[SYNCOOKIE_SECRET_SIZE];
	u_int32_t	sch_secbits_even[SYNCOOKIE_SECRET_SIZE];
	u_int		sch_reseed;		/* time_uptime, seconds */
};

static void	 syncache_drop(struct syncache *, struct syncache_head *);
static void	 syncache_free(struct syncache *);
static void	 syncache_insert(struct syncache *, struct syncache_head *);
struct syncache *syncache_lookup(struct in_conninfo *, struct syncache_head **);
static int	 syncache_respond(struct syncache *);
static struct	 socket *syncache_socket(struct syncache *, struct socket *,
		    struct mbuf *m);
static void	 syncache_timeout(struct syncache *sc, struct syncache_head *sch,
		    int docallout);
static void	 syncache_timer(void *);
static void	 syncookie_generate(struct syncache_head *, struct syncache *,
		    u_int32_t *);
static struct syncache
		*syncookie_lookup(struct in_conninfo *, struct syncache_head *,
		    struct syncache *, struct tcpopt *, struct tcphdr *,
		    struct socket *);

/*
 * Transmit the SYN,ACK fewer times than TCP_MAXRXTSHIFT specifies.
 * 3 retransmits corresponds to a timeout of 3 * (1 + 2 + 4 + 8) == 45 seconds,
 * the odds are that the user has given up attempting to connect by then.
 */
#define SYNCACHE_MAXREXMTS		3

/* Arbitrary values */
#define TCP_SYNCACHE_HASHSIZE		512
#define TCP_SYNCACHE_BUCKETLIMIT	30

struct tcp_syncache {
	struct	syncache_head *hashbase;
	uma_zone_t zone;
	u_int	hashsize;
	u_int	hashmask;
	u_int	bucket_limit;
	u_int	cache_count;		/* XXX: unprotected */
	u_int	cache_limit;
	u_int	rexmt_limit;
	u_int	hash_secret;
};
static struct tcp_syncache tcp_syncache;

SYSCTL_NODE(_net_inet_tcp, OID_AUTO, syncache, CTLFLAG_RW, 0, "TCP SYN cache");

SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, bucketlimit, CTLFLAG_RDTUN,
     &tcp_syncache.bucket_limit, 0, "Per-bucket hash limit for syncache");

SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, cachelimit, CTLFLAG_RDTUN,
     &tcp_syncache.cache_limit, 0, "Overall entry limit for syncache");

SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, count, CTLFLAG_RD,
     &tcp_syncache.cache_count, 0, "Current number of entries in syncache");

SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, hashsize, CTLFLAG_RDTUN,
     &tcp_syncache.hashsize, 0, "Size of TCP syncache hashtable");

SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, rexmtlimit, CTLFLAG_RW,
     &tcp_syncache.rexmt_limit, 0, "Limit on SYN/ACK retransmissions");

int	tcp_sc_rst_sock_fail = 1;
SYSCTL_INT(_net_inet_tcp_syncache, OID_AUTO, rst_on_sock_fail, CTLFLAG_RW,
     &tcp_sc_rst_sock_fail, 0, "Send reset on socket allocation failure");

static MALLOC_DEFINE(M_SYNCACHE, "syncache", "TCP syncache");

#define SYNCACHE_HASH(inc, mask)					\
	((tcp_syncache.hash_secret ^					\
	  (inc)->inc_faddr.s_addr ^					\
	  ((inc)->inc_faddr.s_addr >> 16) ^				\
	  (inc)->inc_fport ^ (inc)->inc_lport) & mask)

#define SYNCACHE_HASH6(inc, mask)					\
	((tcp_syncache.hash_secret ^					\
	  (inc)->inc6_faddr.s6_addr32[0] ^				\
	  (inc)->inc6_faddr.s6_addr32[3] ^				\
	  (inc)->inc_fport ^ (inc)->inc_lport) & mask)

#define ENDPTS_EQ(a, b) (						\
	(a)->ie_fport == (b)->ie_fport &&				\
	(a)->ie_lport == (b)->ie_lport &&				\
	(a)->ie_faddr.s_addr == (b)->ie_faddr.s_addr &&			\
	(a)->ie_laddr.s_addr == (b)->ie_laddr.s_addr			\
)

#define ENDPTS6_EQ(a, b) (memcmp(a, b, sizeof(*a)) == 0)

#define	SCH_LOCK(sch)		mtx_lock(&(sch)->sch_mtx)
#define	SCH_UNLOCK(sch)		mtx_unlock(&(sch)->sch_mtx)
#define	SCH_LOCK_ASSERT(sch)	mtx_assert(&(sch)->sch_mtx, MA_OWNED)

/*
 * Requires the syncache entry to be already removed from the bucket list.
 */
static void
syncache_free(struct syncache *sc)
{
	if (sc->sc_ipopts)
		(void) m_free(sc->sc_ipopts);
#ifdef MAC
	mac_destroy_syncache(&sc->sc_label);
#endif

	uma_zfree(tcp_syncache.zone, sc);
}

void
syncache_init(void)
{
	int i;

	tcp_syncache.cache_count = 0;
	tcp_syncache.hashsize = TCP_SYNCACHE_HASHSIZE;
	tcp_syncache.bucket_limit = TCP_SYNCACHE_BUCKETLIMIT;
	tcp_syncache.rexmt_limit = SYNCACHE_MAXREXMTS;
	tcp_syncache.hash_secret = arc4random();

	TUNABLE_INT_FETCH("net.inet.tcp.syncache.hashsize",
	    &tcp_syncache.hashsize);
	TUNABLE_INT_FETCH("net.inet.tcp.syncache.bucketlimit",
	    &tcp_syncache.bucket_limit);
	if (!powerof2(tcp_syncache.hashsize) || tcp_syncache.hashsize == 0) {
		printf("WARNING: syncache hash size is not a power of 2.\n");
		tcp_syncache.hashsize = TCP_SYNCACHE_HASHSIZE;
	}
	tcp_syncache.hashmask = tcp_syncache.hashsize - 1;

	/* Set limits. */
	tcp_syncache.cache_limit =
	    tcp_syncache.hashsize * tcp_syncache.bucket_limit;
	TUNABLE_INT_FETCH("net.inet.tcp.syncache.cachelimit",
	    &tcp_syncache.cache_limit);

	/* Allocate the hash table. */
	MALLOC(tcp_syncache.hashbase, struct syncache_head *,
	    tcp_syncache.hashsize * sizeof(struct syncache_head),
	    M_SYNCACHE, M_WAITOK | M_ZERO);

	/* Initialize the hash buckets. */
	for (i = 0; i < tcp_syncache.hashsize; i++) {
		TAILQ_INIT(&tcp_syncache.hashbase[i].sch_bucket);
		mtx_init(&tcp_syncache.hashbase[i].sch_mtx, "tcp_sc_head",
			 NULL, MTX_DEF);
		callout_init_mtx(&tcp_syncache.hashbase[i].sch_timer,
			 &tcp_syncache.hashbase[i].sch_mtx, 0);
		tcp_syncache.hashbase[i].sch_length = 0;
	}

	/* Create the syncache entry zone. */
	tcp_syncache.zone = uma_zcreate("syncache", sizeof(struct syncache),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	uma_zone_set_max(tcp_syncache.zone, tcp_syncache.cache_limit);
}

/*
 * Inserts a syncache entry into the specified bucket row.
 * Locks and unlocks the syncache_head autonomously.
 */
static void
syncache_insert(struct syncache *sc, struct syncache_head *sch)
{
	struct syncache *sc2;

	SCH_LOCK(sch);

	/*
	 * Make sure that we don't overflow the per-bucket limit.
	 * If the bucket is full, toss the oldest element.
	 */
	if (sch->sch_length >= tcp_syncache.bucket_limit) {
		KASSERT(!TAILQ_EMPTY(&sch->sch_bucket),
			("sch->sch_length incorrect"));
		sc2 = TAILQ_LAST(&sch->sch_bucket, sch_head);
		syncache_drop(sc2, sch);
		tcpstat.tcps_sc_bucketoverflow++;
	}

	/* Put it into the bucket. */
	TAILQ_INSERT_HEAD(&sch->sch_bucket, sc, sc_hash);
	sch->sch_length++;

	/* Reinitialize the bucket row's timer. */
	if (sch->sch_length == 1)
		sch->sch_nextc = ticks + INT_MAX;
	syncache_timeout(sc, sch, 1);

	SCH_UNLOCK(sch);

	tcp_syncache.cache_count++;
	tcpstat.tcps_sc_added++;
	//printf("syncache_insert: cache_count=%d\n", tcp_syncache.cache_count);
}

/*
 * Remove and free entry from syncache bucket row.
 * Expects locked syncache head.
 */
static void
syncache_drop(struct syncache *sc, struct syncache_head *sch)
{

	SCH_LOCK_ASSERT(sch);

	TAILQ_REMOVE(&sch->sch_bucket, sc, sc_hash);
	sch->sch_length--;

	syncache_free(sc);
	tcp_syncache.cache_count--;
	//printf("syncache_drop: cache_count=%d\n", tcp_syncache.cache_count);
}

/*
 * Engage/reengage time on bucket row.
 */
static void
syncache_timeout(struct syncache *sc, struct syncache_head *sch, int docallout)
{
	sc->sc_rxttime = ticks +
		TCPTV_RTOBASE * (tcp_backoff[sc->sc_rxmits]);
	sc->sc_rxmits++;
	if (TSTMP_LT(sc->sc_rxttime, sch->sch_nextc)) {
		sch->sch_nextc = sc->sc_rxttime;
		if (docallout)
			callout_reset(&sch->sch_timer, sch->sch_nextc - ticks,
			    syncache_timer, (void *)sch);
	}
}

/*
 * Walk the timer queues, looking for SYN,ACKs that need to be retransmitted.
 * If we have retransmitted an entry the maximum number of times, expire it.
 * One separate timer for each bucket row.
 */
static void
syncache_timer(void *xsch)
{
	struct syncache_head *sch = (struct syncache_head *)xsch;
	struct syncache *sc, *nsc;
	int tick = ticks;
	char *s;

	/* NB: syncache_head has already been locked by the callout. */
	SCH_LOCK_ASSERT(sch);

	/*
	 * In the following cycle we may remove some entries and/or
	 * advance some timeouts, so re-initialize the bucket timer.
	 */
	sch->sch_nextc = tick + INT_MAX;

	TAILQ_FOREACH_SAFE(sc, &sch->sch_bucket, sc_hash, nsc) {
		/*
		 * We do not check if the listen socket still exists
		 * and accept the case where the listen socket may be
		 * gone by the time we resend the SYN/ACK.  We do
		 * not expect this to happens often. If it does,
		 * then the RST will be sent by the time the remote
		 * host does the SYN/ACK->ACK.
		 */
		if (TSTMP_GT(sc->sc_rxttime, tick)) {
			if (TSTMP_LT(sc->sc_rxttime, sch->sch_nextc))
				sch->sch_nextc = sc->sc_rxttime;
			continue;
		}

		if (sc->sc_rxmits > tcp_syncache.rexmt_limit) {
			if ((s = tcp_log_addrs(&sc->sc_inc, NULL, NULL, NULL))) {
				log(LOG_DEBUG, "%s; %s: Retransmits exhausted, "
				    "giving up and removing syncache entry\n",
				    s, __func__);
				free(s, M_TCPLOG);
			}
			syncache_drop(sc, sch);
			tcpstat.tcps_sc_stale++;
			continue;
		}
		if ((s = tcp_log_addrs(&sc->sc_inc, NULL, NULL, NULL))) {
			log(LOG_DEBUG, "%s; %s: Response timeout, "
			    "retransmitting (%u) SYN|ACK\n",
			    s, __func__, sc->sc_rxmits);
			free(s, M_TCPLOG);
		}

		(void) syncache_respond(sc);
		tcpstat.tcps_sc_retransmitted++;
		syncache_timeout(sc, sch, 0);
	}
	if (!TAILQ_EMPTY(&(sch)->sch_bucket))
		callout_reset(&(sch)->sch_timer, (sch)->sch_nextc - tick,
			syncache_timer, (void *)(sch));
}

/*
 * Find an entry in the syncache.
 * Returns always with locked syncache_head plus a matching entry or NULL.
 */
struct syncache *
syncache_lookup(struct in_conninfo *inc, struct syncache_head **schp)
{
	struct syncache *sc;
	struct syncache_head *sch;

#ifdef INET6
	if (inc->inc_isipv6) {
		sch = &tcp_syncache.hashbase[
		    SYNCACHE_HASH6(inc, tcp_syncache.hashmask)];
		*schp = sch;

		SCH_LOCK(sch);

		/* Circle through bucket row to find matching entry. */
		TAILQ_FOREACH(sc, &sch->sch_bucket, sc_hash) {
			if (ENDPTS6_EQ(&inc->inc_ie, &sc->sc_inc.inc_ie))
				return (sc);
		}
	} else
#endif
	{
		sch = &tcp_syncache.hashbase[
		    SYNCACHE_HASH(inc, tcp_syncache.hashmask)];
		*schp = sch;

		SCH_LOCK(sch);

		/* Circle through bucket row to find matching entry. */
		TAILQ_FOREACH(sc, &sch->sch_bucket, sc_hash) {
#ifdef INET6
			if (sc->sc_inc.inc_isipv6)
				continue;
#endif
			if (ENDPTS_EQ(&inc->inc_ie, &sc->sc_inc.inc_ie))
				return (sc);
		}
	}
	SCH_LOCK_ASSERT(*schp);
	return (NULL);			/* always returns with locked sch */
}

/*
 * This function is called when we get a RST for a
 * non-existent connection, so that we can see if the
 * connection is in the syn cache.  If it is, zap it.
 */
void
syncache_chkrst(struct in_conninfo *inc, struct tcphdr *th)
{
	struct syncache *sc;
	struct syncache_head *sch;
	char *s = NULL;

	sc = syncache_lookup(inc, &sch);	/* returns locked sch */
	SCH_LOCK_ASSERT(sch);

	/*
	 * Any RST to our SYN|ACK must not carry ACK, SYN or FIN flags.
	 * See RFC 793 page 65, section SEGMENT ARRIVES.
	 */
	if (th->th_flags & (TH_ACK|TH_SYN|TH_FIN)) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: Spurious RST with ACK, SYN or "
			    "FIN flag set, segment ignored\n", s, __func__);
		tcpstat.tcps_badrst++;
		goto done;
	}

	/*
	 * No corresponding connection was found in syncache.
	 * If syncookies are enabled and possibly exclusively
	 * used, or we are under memory pressure, a valid RST
	 * may not find a syncache entry.  In that case we're
	 * done and no SYN|ACK retransmissions will happen.
	 * Otherwise the the RST was misdirected or spoofed.
	 */
	if (sc == NULL) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: Spurious RST without matching "
			    "syncache entry (possibly syncookie only), "
			    "segment ignored\n", s, __func__);
		tcpstat.tcps_badrst++;
		goto done;
	}

	/*
	 * If the RST bit is set, check the sequence number to see
	 * if this is a valid reset segment.
	 * RFC 793 page 37:
	 *   In all states except SYN-SENT, all reset (RST) segments
	 *   are validated by checking their SEQ-fields.  A reset is
	 *   valid if its sequence number is in the window.
	 *
	 *   The sequence number in the reset segment is normally an
	 *   echo of our outgoing acknowlegement numbers, but some hosts
	 *   send a reset with the sequence number at the rightmost edge
	 *   of our receive window, and we have to handle this case.
	 */
	if (SEQ_GEQ(th->th_seq, sc->sc_irs) &&
	    SEQ_LEQ(th->th_seq, sc->sc_irs + sc->sc_wnd)) {
		syncache_drop(sc, sch);
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: Our SYN|ACK was rejected, "
			    "connection attempt aborted by remote endpoint\n",
			    s, __func__);
		tcpstat.tcps_sc_reset++;
	} else if ((s = tcp_log_addrs(inc, th, NULL, NULL))) {
		log(LOG_DEBUG, "%s; %s: RST with invalid SEQ %u != IRS %u "
		    "(+WND %u), segment ignored\n",
		    s, __func__, th->th_seq, sc->sc_irs, sc->sc_wnd);
		tcpstat.tcps_badrst++;
	}

done:
	if (s != NULL)
		free(s, M_TCPLOG);
	SCH_UNLOCK(sch);
}

void
syncache_badack(struct in_conninfo *inc)
{
	struct syncache *sc;
	struct syncache_head *sch;

	sc = syncache_lookup(inc, &sch);	/* returns locked sch */
	SCH_LOCK_ASSERT(sch);
	if (sc != NULL) {
		syncache_drop(sc, sch);
		tcpstat.tcps_sc_badack++;
	}
	SCH_UNLOCK(sch);
}

void
syncache_unreach(struct in_conninfo *inc, struct tcphdr *th)
{
	struct syncache *sc;
	struct syncache_head *sch;

	sc = syncache_lookup(inc, &sch);	/* returns locked sch */
	SCH_LOCK_ASSERT(sch);
	if (sc == NULL)
		goto done;

	/* If the sequence number != sc_iss, then it's a bogus ICMP msg */
	if (ntohl(th->th_seq) != sc->sc_iss)
		goto done;

	/*
	 * If we've rertransmitted 3 times and this is our second error,
	 * we remove the entry.  Otherwise, we allow it to continue on.
	 * This prevents us from incorrectly nuking an entry during a
	 * spurious network outage.
	 *
	 * See tcp_notify().
	 */
	if ((sc->sc_flags & SCF_UNREACH) == 0 || sc->sc_rxmits < 3 + 1) {
		sc->sc_flags |= SCF_UNREACH;
		goto done;
	}
	syncache_drop(sc, sch);
	tcpstat.tcps_sc_unreach++;
done:
	SCH_UNLOCK(sch);
}

/*
 * Build a new TCP socket structure from a syncache entry.
 */
static struct socket *
syncache_socket(struct syncache *sc, struct socket *lso, struct mbuf *m)
{
	struct inpcb *inp = NULL;
	struct socket *so;
	struct tcpcb *tp;
	char *s;

	INP_INFO_WLOCK_ASSERT(&tcbinfo);

	/*
	 * Ok, create the full blown connection, and set things up
	 * as they would have been set up if we had created the
	 * connection when the SYN arrived.  If we can't create
	 * the connection, abort it.
	 */
	so = sonewconn(lso, SS_ISCONNECTED);
	if (so == NULL) {
		/*
		 * Drop the connection; we will either send a RST or
		 * have the peer retransmit its SYN again after its
		 * RTO and try again.
		 */
		tcpstat.tcps_listendrop++;
		if ((s = tcp_log_addrs(&sc->sc_inc, NULL, NULL, NULL))) {
			log(LOG_DEBUG, "%s; %s: Socket create failed "
			    "due to limits or memory shortage\n",
			    s, __func__);
			free(s, M_TCPLOG);
		}
		goto abort2;
	}
#ifdef MAC
	SOCK_LOCK(so);
	mac_set_socket_peer_from_mbuf(m, so);
	SOCK_UNLOCK(so);
#endif

	inp = sotoinpcb(so);
	INP_LOCK(inp);

	/* Insert new socket into PCB hash list. */
	inp->inp_inc.inc_isipv6 = sc->sc_inc.inc_isipv6;
#ifdef INET6
	if (sc->sc_inc.inc_isipv6) {
		inp->in6p_laddr = sc->sc_inc.inc6_laddr;
	} else {
		inp->inp_vflag &= ~INP_IPV6;
		inp->inp_vflag |= INP_IPV4;
#endif
		inp->inp_laddr = sc->sc_inc.inc_laddr;
#ifdef INET6
	}
#endif
	inp->inp_lport = sc->sc_inc.inc_lport;
	if (in_pcbinshash(inp) != 0) {
		/*
		 * Undo the assignments above if we failed to
		 * put the PCB on the hash lists.
		 */
#ifdef INET6
		if (sc->sc_inc.inc_isipv6)
			inp->in6p_laddr = in6addr_any;
		else
#endif
			inp->inp_laddr.s_addr = INADDR_ANY;
		inp->inp_lport = 0;
		goto abort;
	}
#ifdef IPSEC
	/* Copy old policy into new socket's. */
	if (ipsec_copy_policy(sotoinpcb(lso)->inp_sp, inp->inp_sp))
		printf("syncache_socket: could not copy policy\n");
#endif
#ifdef INET6
	if (sc->sc_inc.inc_isipv6) {
		struct inpcb *oinp = sotoinpcb(lso);
		struct in6_addr laddr6;
		struct sockaddr_in6 sin6;
		/*
		 * Inherit socket options from the listening socket.
		 * Note that in6p_inputopts are not (and should not be)
		 * copied, since it stores previously received options and is
		 * used to detect if each new option is different than the
		 * previous one and hence should be passed to a user.
		 * If we copied in6p_inputopts, a user would not be able to
		 * receive options just after calling the accept system call.
		 */
		inp->inp_flags |= oinp->inp_flags & INP_CONTROLOPTS;
		if (oinp->in6p_outputopts)
			inp->in6p_outputopts =
			    ip6_copypktopts(oinp->in6p_outputopts, M_NOWAIT);

		sin6.sin6_family = AF_INET6;
		sin6.sin6_len = sizeof(sin6);
		sin6.sin6_addr = sc->sc_inc.inc6_faddr;
		sin6.sin6_port = sc->sc_inc.inc_fport;
		sin6.sin6_flowinfo = sin6.sin6_scope_id = 0;
		laddr6 = inp->in6p_laddr;
		if (IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr))
			inp->in6p_laddr = sc->sc_inc.inc6_laddr;
		if (in6_pcbconnect(inp, (struct sockaddr *)&sin6,
		    thread0.td_ucred)) {
			inp->in6p_laddr = laddr6;
			goto abort;
		}
		/* Override flowlabel from in6_pcbconnect. */
		inp->in6p_flowinfo &= ~IPV6_FLOWLABEL_MASK;
		inp->in6p_flowinfo |= sc->sc_flowlabel;
	} else
#endif
	{
		struct in_addr laddr;
		struct sockaddr_in sin;

		inp->inp_options = ip_srcroute(m);
		if (inp->inp_options == NULL) {
			inp->inp_options = sc->sc_ipopts;
			sc->sc_ipopts = NULL;
		}

		sin.sin_family = AF_INET;
		sin.sin_len = sizeof(sin);
		sin.sin_addr = sc->sc_inc.inc_faddr;
		sin.sin_port = sc->sc_inc.inc_fport;
		bzero((caddr_t)sin.sin_zero, sizeof(sin.sin_zero));
		laddr = inp->inp_laddr;
		if (inp->inp_laddr.s_addr == INADDR_ANY)
			inp->inp_laddr = sc->sc_inc.inc_laddr;
		if (in_pcbconnect(inp, (struct sockaddr *)&sin,
		    thread0.td_ucred)) {
			inp->inp_laddr = laddr;
			goto abort;
		}
	}
	tp = intotcpcb(inp);
	tp->t_state = TCPS_SYN_RECEIVED;
	tp->iss = sc->sc_iss;
	tp->irs = sc->sc_irs;
	tcp_rcvseqinit(tp);
	tcp_sendseqinit(tp);
	tp->snd_wl1 = sc->sc_irs;
	tp->snd_max = tp->iss + 1;
	tp->snd_nxt = tp->iss + 1;
	tp->rcv_up = sc->sc_irs + 1;
	tp->rcv_wnd = sc->sc_wnd;
	tp->rcv_adv += tp->rcv_wnd;
	tp->last_ack_sent = tp->rcv_nxt;
#ifndef MAXHE_TODO
        so->ini_seq = tp->snd_nxt;
        //printf("tcp_syncache: called\n");
#endif // MAXHE_TODO


	tp->t_flags = sototcpcb(lso)->t_flags & (TF_NOPUSH|TF_NODELAY);
	if (sc->sc_flags & SCF_NOOPT)
		tp->t_flags |= TF_NOOPT;
	else {
		if (sc->sc_flags & SCF_WINSCALE) {
			tp->t_flags |= TF_REQ_SCALE|TF_RCVD_SCALE;
			tp->snd_scale = sc->sc_requested_s_scale;
			tp->request_r_scale = sc->sc_requested_r_scale;
		}
		if (sc->sc_flags & SCF_TIMESTAMP) {
			tp->t_flags |= TF_REQ_TSTMP|TF_RCVD_TSTMP;
			tp->ts_recent = sc->sc_tsreflect;
			tp->ts_recent_age = ticks;
			tp->ts_offset = sc->sc_tsoff;
		}
#ifdef TCP_SIGNATURE
		if (sc->sc_flags & SCF_SIGNATURE)
			tp->t_flags |= TF_SIGNATURE;
#endif
		if (sc->sc_flags & SCF_SACK)
			tp->t_flags |= TF_SACK_PERMIT;
	}

	/*
	 * Set up MSS and get cached values from tcp_hostcache.
	 * This might overwrite some of the defaults we just set.
	 */
	tcp_mss(tp, sc->sc_peer_mss);

	/*
	 * If the SYN,ACK was retransmitted, reset cwnd to 1 segment.
	 */
	//printf("syncache_socket: (3) tp->snd_cwnd=%ld\n", tp->snd_cwnd);
	if (sc->sc_rxmits)
		tp->snd_cwnd = tp->t_maxseg;
	//printf("syncache_socket: (5)tp->snd_cwnd=%ld\n", tp->snd_cwnd);
	tcp_timer_activate(tp, TT_KEEP, tcp_keepinit);

	INP_UNLOCK(inp);

	tcpstat.tcps_accepts++;
	return (so);

abort:
	INP_UNLOCK(inp);
abort2:
	if (so != NULL)
		soabort(so);
	return (NULL);
}

/*
 * This function gets called when we receive an ACK for a
 * socket in the LISTEN state.  We look up the connection
 * in the syncache, and if its there, we pull it out of
 * the cache and turn it into a full-blown connection in
 * the SYN-RECEIVED state.
 */
int
syncache_expand(struct in_conninfo *inc, struct tcpopt *to, struct tcphdr *th,
    struct socket **lsop, struct mbuf *m)
{
	struct syncache *sc;
	struct syncache_head *sch;
	struct syncache scs;
	char *s;

	/*
	 * Global TCP locks are held because we manipulate the PCB lists
	 * and create a new socket.
	 */
	INP_INFO_WLOCK_ASSERT(&tcbinfo);
	KASSERT((th->th_flags & (TH_RST|TH_ACK|TH_SYN)) == TH_ACK,
	    ("%s: can handle only ACK", __func__));

	sc = syncache_lookup(inc, &sch);	/* returns locked sch */
	SCH_LOCK_ASSERT(sch);
	if (sc == NULL) {
		/*
		 * There is no syncache entry, so see if this ACK is
		 * a returning syncookie.  To do this, first:
		 *  A. See if this socket has had a syncache entry dropped in
		 *     the past.  We don't want to accept a bogus syncookie
		 *     if we've never received a SYN.
		 *  B. check that the syncookie is valid.  If it is, then
		 *     cobble up a fake syncache entry, and return.
		 */
		if (!tcp_syncookies) {
			SCH_UNLOCK(sch);
			if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
				log(LOG_DEBUG, "%s; %s: Spurious ACK, "
				    "segment rejected (syncookies disabled)\n",
				    s, __func__);
			goto failed;
		}
		bzero(&scs, sizeof(scs));
		sc = syncookie_lookup(inc, sch, &scs, to, th, *lsop);
		SCH_UNLOCK(sch);
		if (sc == NULL) {
			if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
				log(LOG_DEBUG, "%s; %s: Segment failed "
				    "SYNCOOKIE authentication, segment rejected "
				    "(probably spoofed)\n", s, __func__);
			goto failed;
		}
	} else {
		/* Pull out the entry to unlock the bucket row. */
		TAILQ_REMOVE(&sch->sch_bucket, sc, sc_hash);
		sch->sch_length--;
		tcp_syncache.cache_count--;
		//printf("syncache_expand: cache_count=%d\n", tcp_syncache.cache_count);
		SCH_UNLOCK(sch);
	}

	/*
	 * Segment validation:
	 * ACK must match our initial sequence number + 1 (the SYN|ACK).
	 */
	if (th->th_ack != sc->sc_iss + 1) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: ACK %u != ISS+1 %u, segment "
			    "rejected\n", s, __func__, th->th_ack, sc->sc_iss);
		goto failed;
	}
	/*
	 * The SEQ must match the received initial receive sequence
	 * number + 1 (the SYN) because we didn't ACK any data that
	 * may have come with the SYN.
	 */
	if (th->th_seq != sc->sc_irs + 1) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: SEQ %u != IRS+1 %u, segment "
			    "rejected\n", s, __func__, th->th_seq, sc->sc_irs);
		goto failed;
	}
#if 0
	/*
	 * If timestamps were present in the SYN and we accepted
	 * them in our SYN|ACK we require them to be present from
	 * now on.  And vice versa.
	 *
	 * Unfortunately, during testing of 7.0 some users found
	 * network devices that violate this constraint, so it must
	 * be disabled.
	 */
	if ((sc->sc_flags & SCF_TIMESTAMP) && !(to->to_flags & TOF_TS)) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: Timestamp missing, "
			    "segment rejected\n", s, __func__);
		goto failed;
	}
#endif
	if (!(sc->sc_flags & SCF_TIMESTAMP) && (to->to_flags & TOF_TS)) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: Timestamp not expected, "
			    "segment rejected\n", s, __func__);
		goto failed;
	}
	/*
	 * If timestamps were negotiated the reflected timestamp
	 * must be equal to what we actually sent in the SYN|ACK.
	 */
	if ((to->to_flags & TOF_TS) && to->to_tsecr != sc->sc_ts) {
		if ((s = tcp_log_addrs(inc, th, NULL, NULL)))
			log(LOG_DEBUG, "%s; %s: TSECR %u != TS %u, "
			    "segment rejected\n",
			    s, __func__, to->to_tsecr, sc->sc_ts);
		goto failed;
	}

	*lsop = syncache_socket(sc, *lsop, m);

	if (*lsop == NULL)
		tcpstat.tcps_sc_aborted++;
	else
		tcpstat.tcps_sc_completed++;

	if (sc != &scs)
		syncache_free(sc);
	return (1);
failed:
	if (sc != NULL && sc != &scs)
		syncache_free(sc);
	if (s != NULL)
		free(s, M_TCPLOG);
	*lsop = NULL;
	return (0);
}

/*
 * Given a LISTEN socket and an inbound SYN request, add
 * this to the syn cache, and send back a segment:
 *	<SEQ=ISS><ACK=RCV_NXT><CTL=SYN,ACK>
 * to the source.
 *
 * IMPORTANT NOTE: We do _NOT_ ACK data that might accompany the SYN.
 * Doing so would require that we hold onto the data and deliver it
 * to the application.  However, if we are the target of a SYN-flood
 * DoS attack, an attacker could send data which would eventually
 * consume all available buffer space if it were ACKed.  By not ACKing
 * the data, we avoid this DoS scenario.
 */
void
syncache_add(struct in_conninfo *inc, struct tcpopt *to, struct tcphdr *th,
    struct inpcb *inp, struct socket **lsop, struct mbuf *m)
{
	struct tcpcb *tp;
	struct socket *so;
	struct syncache *sc = NULL;
	struct syncache_head *sch;
	struct mbuf *ipopts = NULL;
	u_int32_t flowtmp;
	int win, sb_hiwat, ip_ttl, ip_tos, noopt;
	char *s;
#ifdef INET6
	int autoflowlabel = 0;
#endif
#ifdef MAC
	struct label *maclabel;
#endif
	struct syncache scs;

	//printf("syncache_add: called\n");
	INP_INFO_WLOCK_ASSERT(&tcbinfo);
	INP_LOCK_ASSERT(inp);			/* listen socket */
	KASSERT((th->th_flags & (TH_RST|TH_ACK|TH_SYN)) == TH_SYN,
	    ("%s: unexpected tcp flags", __func__));

	/*
	 * Combine all so/tp operations very early to drop the INP lock as
	 * soon as possible.
	 */
	so = *lsop;
	tp = sototcpcb(so);

#ifdef INET6
	if (inc->inc_isipv6 &&
	    (inp->in6p_flags & IN6P_AUTOFLOWLABEL))
		autoflowlabel = 1;
#endif
	ip_ttl = inp->inp_ip_ttl;
	ip_tos = inp->inp_ip_tos;
	win = sbspace(&so->so_rcv);
	sb_hiwat = so->so_rcv.sb_hiwat;
	noopt = (tp->t_flags & TF_NOOPT);

	//printf("syncache_add: called 2\n");
	so = NULL;
	tp = NULL;

#ifdef MAC
	if (mac_init_syncache(&maclabel) != 0) {
		INP_UNLOCK(inp);
		INP_INFO_WUNLOCK(&tcbinfo);
		goto done;
	} else
		mac_init_syncache_from_inpcb(maclabel, inp);
#endif
	INP_UNLOCK(inp);
	INP_INFO_WUNLOCK(&tcbinfo);

	//printf("syncache_add: called 4\n");
	/*
	 * Remember the IP options, if any.
	 */
#ifdef INET6
	if (!inc->inc_isipv6)
#endif
		ipopts = ip_srcroute(m);
	//printf("syncache_add: called 5\n");

	/*
	 * See if we already have an entry for this connection.
	 * If we do, resend the SYN,ACK, and reset the retransmit timer.
	 *
	 * XXX: should the syncache be re-initialized with the contents
	 * of the new SYN here (which may have different options?)
	 *
	 * XXX: We do not check the sequence number to see if this is a
	 * real retransmit or a new connection attempt.  The question is
	 * how to handle such a case; either ignore it as spoofed, or
	 * drop the current entry and create a new one?
	 */
	sc = syncache_lookup(inc, &sch);	/* returns locked entry */
	//printf("syncache_add: syncache_lookup returns 0x%x\n", (long)sc);
	SCH_LOCK_ASSERT(sch);
	if (sc != NULL) {
		tcpstat.tcps_sc_dupsyn++;
		if (ipopts) {
			/*
			 * If we were remembering a previous source route,
			 * forget it and use the new one we've been given.
			 */
			if (sc->sc_ipopts)
				(void) m_free(sc->sc_ipopts);
			sc->sc_ipopts = ipopts;
		}
		/*
		 * Update timestamp if present.
		 */
		if ((sc->sc_flags & SCF_TIMESTAMP) && (to->to_flags & TOF_TS))
			sc->sc_tsreflect = to->to_tsval;
		else
			sc->sc_flags &= ~SCF_TIMESTAMP;
#ifdef MAC
		/*
		 * Since we have already unconditionally allocated label
		 * storage, free it up.  The syncache entry will already
		 * have an initialized label we can use.
		 */
		mac_destroy_syncache(&maclabel);
		KASSERT(sc->sc_label != NULL,
		    ("%s: label not initialized", __func__));
#endif
		/* Retransmit SYN|ACK and reset retransmit count. */
		if ((s = tcp_log_addrs(&sc->sc_inc, th, NULL, NULL))) {
			log(LOG_DEBUG, "%s; %s: Received duplicate SYN, "
			    "resetting timer and retransmitting SYN|ACK\n",
			    s, __func__);
			free(s, M_TCPLOG);
		}
		if (syncache_respond(sc) == 0) {
			sc->sc_rxmits = 0;
			syncache_timeout(sc, sch, 1);
			tcpstat.tcps_sndacks++;
			tcpstat.tcps_sndtotal++;
		}
		SCH_UNLOCK(sch);
		goto done;
	}

	sc = uma_zalloc(tcp_syncache.zone, M_NOWAIT | M_ZERO);
	//printf("%s: uma_zlloc returns 0x%x\n", __FUNCTION__, (long)sc);
	if (sc == NULL) {
		/*
		 * The zone allocator couldn't provide more entries.
		 * Treat this as if the cache was full; drop the oldest
		 * entry and insert the new one.
		 */
		tcpstat.tcps_sc_zonefail++;
		if ((sc = TAILQ_LAST(&sch->sch_bucket, sch_head)) != NULL)
			syncache_drop(sc, sch);
		sc = uma_zalloc(tcp_syncache.zone, M_NOWAIT | M_ZERO);
		if (sc == NULL) {
			if (tcp_syncookies) {
				bzero(&scs, sizeof(scs));
				sc = &scs;
			} else {
				SCH_UNLOCK(sch);
				if (ipopts)
					(void) m_free(ipopts);
				goto done;
			}
		}
	}

	//printf("%s: here 4\n", __FUNCTION__);
	/*
	 * Fill in the syncache values.
	 */
#ifdef MAC
	sc->sc_label = maclabel;
#endif
	sc->sc_ipopts = ipopts;
	bcopy(inc, &sc->sc_inc, sizeof(struct in_conninfo));
#ifdef INET6
	if (!inc->inc_isipv6)
#endif
	{
		sc->sc_ip_tos = ip_tos;
		sc->sc_ip_ttl = ip_ttl;
	}

	sc->sc_irs = th->th_seq;
	sc->sc_iss = arc4random();
	sc->sc_flags = 0;
	sc->sc_flowlabel = 0;

	/*
	 * Initial receive window: clip sbspace to [0 .. TCP_MAXWIN].
	 * win was derived from socket earlier in the function.
	 */
	win = imax(win, 0);
	win = imin(win, TCP_MAXWIN);
	sc->sc_wnd = win;

	//printf("%s: here 7\n", __FUNCTION__);
	if (tcp_do_rfc1323) {
		/*
		 * A timestamp received in a SYN makes
		 * it ok to send timestamp requests and replies.
		 */
		if (to->to_flags & TOF_TS) {
			sc->sc_tsreflect = to->to_tsval;
			sc->sc_ts = ticks;
			sc->sc_flags |= SCF_TIMESTAMP;
		}
		if (to->to_flags & TOF_SCALE) {
			int wscale = 0;

			/*
			 * Pick the smallest possible scaling factor that
			 * will still allow us to scale up to sb_max, aka
			 * kern.ipc.maxsockbuf.
			 *
			 * We do this because there are broken firewalls that
			 * will corrupt the window scale option, leading to
			 * the other endpoint believing that our advertised
			 * window is unscaled.  At scale factors larger than
			 * 5 the unscaled window will drop below 1500 bytes,
			 * leading to serious problems when traversing these
			 * broken firewalls.
			 *
			 * With the default maxsockbuf of 256K, a scale factor
			 * of 3 will be chosen by this algorithm.  Those who
			 * choose a larger maxsockbuf should watch out
			 * for the compatiblity problems mentioned above.
			 *
			 * RFC1323: The Window field in a SYN (i.e., a <SYN>
			 * or <SYN,ACK>) segment itself is never scaled.
			 */
			while (wscale < TCP_MAX_WINSHIFT &&
			    (TCP_MAXWIN << wscale) < sb_max)
				wscale++;
			sc->sc_requested_r_scale = wscale;
			sc->sc_requested_s_scale = to->to_wscale;
			sc->sc_flags |= SCF_WINSCALE;
		}
	}
#ifdef TCP_SIGNATURE
	/*
	 * If listening socket requested TCP digests, and received SYN
	 * contains the option, flag this in the syncache so that
	 * syncache_respond() will do the right thing with the SYN+ACK.
	 * XXX: Currently we always record the option by default and will
	 * attempt to use it in syncache_respond().
	 */
	if (to->to_flags & TOF_SIGNATURE)
		sc->sc_flags |= SCF_SIGNATURE;
#endif
	if (to->to_flags & TOF_SACKPERM)
		sc->sc_flags |= SCF_SACK;
	if (to->to_flags & TOF_MSS)
		sc->sc_peer_mss = to->to_mss;	/* peer mss may be zero */
	if (noopt)
		sc->sc_flags |= SCF_NOOPT;

	//printf("%s: here 10\n", __FUNCTION__);
	if (tcp_syncookies) {
		syncookie_generate(sch, sc, &flowtmp);
#ifdef INET6
		if (autoflowlabel)
			sc->sc_flowlabel = flowtmp;
#endif
	} else {
#ifdef INET6
		if (autoflowlabel)
			sc->sc_flowlabel =
			    (htonl(ip6_randomflowlabel()) & IPV6_FLOWLABEL_MASK);
#endif
	}
	SCH_UNLOCK(sch);

	//printf("%s: here 14\n", __FUNCTION__);
	/*
	 * Do a standard 3-way handshake.
	 */
	if (syncache_respond(sc) == 0) {
		if (tcp_syncookies && tcp_syncookiesonly && sc != &scs)
			syncache_free(sc);
		else if (sc != &scs)
			syncache_insert(sc, sch);   /* locks and unlocks sch */
		tcpstat.tcps_sndacks++;
		tcpstat.tcps_sndtotal++;
	} else {
		if (sc != &scs)
			syncache_free(sc);
		tcpstat.tcps_sc_dropped++;
	}

done:
	//printf("%s: here done\n", __FUNCTION__);
#ifdef MAC
	if (sc == &scs)
		mac_destroy_syncache(&maclabel);
#endif
	*lsop = NULL;
	m_freem(m);
	return;
}

static int
syncache_respond(struct syncache *sc)
{
	struct ip *ip = NULL;
	struct mbuf *m;
	struct tcphdr *th;
	int optlen, error;
	u_int16_t hlen, tlen, mssopt;
	struct tcpopt to;
#ifdef INET6
	struct ip6_hdr *ip6 = NULL;
#endif

	hlen =
#ifdef INET6
	       (sc->sc_inc.inc_isipv6) ? sizeof(struct ip6_hdr) :
#endif
		sizeof(struct ip);
	tlen = hlen + sizeof(struct tcphdr);

	/* Determine MSS we advertize to other end of connection. */
	mssopt = tcp_mssopt(&sc->sc_inc);
	if (sc->sc_peer_mss)
		mssopt = max( min(sc->sc_peer_mss, mssopt), tcp_minmss);

	/* XXX: Assume that the entire packet will fit in a header mbuf. */
	KASSERT(max_linkhdr + tlen + TCP_MAXOLEN <= MHLEN,
	    ("syncache: mbuf too small"));

	/* Create the IP+TCP header from scratch. */
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return (ENOBUFS);
#ifdef MAC
	mac_create_mbuf_from_syncache(sc->sc_label, m);
#endif
	m->m_data += max_linkhdr;
	m->m_len = tlen;
	m->m_pkthdr.len = tlen;
	m->m_pkthdr.rcvif = NULL;

#ifdef INET6
	if (sc->sc_inc.inc_isipv6) {
		ip6 = mtod(m, struct ip6_hdr *);
		ip6->ip6_vfc = IPV6_VERSION;
		ip6->ip6_nxt = IPPROTO_TCP;
		ip6->ip6_src = sc->sc_inc.inc6_laddr;
		ip6->ip6_dst = sc->sc_inc.inc6_faddr;
		ip6->ip6_plen = htons(tlen - hlen);
		/* ip6_hlim is set after checksum */
		ip6->ip6_flow &= ~IPV6_FLOWLABEL_MASK;
		ip6->ip6_flow |= sc->sc_flowlabel;

		th = (struct tcphdr *)(ip6 + 1);
	} else
#endif
	{
		ip = mtod(m, struct ip *);
		ip->ip_v = IPVERSION;
		ip->ip_hl = sizeof(struct ip) >> 2;
		ip->ip_len = tlen;
		ip->ip_id = 0;
		ip->ip_off = 0;
		ip->ip_sum = 0;
		ip->ip_p = IPPROTO_TCP;
		ip->ip_src = sc->sc_inc.inc_laddr;
		ip->ip_dst = sc->sc_inc.inc_faddr;
		ip->ip_ttl = sc->sc_ip_ttl;
		ip->ip_tos = sc->sc_ip_tos;

		/*
		 * See if we should do MTU discovery.  Route lookups are
		 * expensive, so we will only unset the DF bit if:
		 *
		 *	1) path_mtu_discovery is disabled
		 *	2) the SCF_UNREACH flag has been set
		 */
		if (path_mtu_discovery && ((sc->sc_flags & SCF_UNREACH) == 0))
		       ip->ip_off |= IP_DF;

		th = (struct tcphdr *)(ip + 1);
	}
	th->th_sport = sc->sc_inc.inc_lport;
	th->th_dport = sc->sc_inc.inc_fport;

	th->th_seq = htonl(sc->sc_iss);
	th->th_ack = htonl(sc->sc_irs + 1);
	th->th_off = sizeof(struct tcphdr) >> 2;
	th->th_x2 = 0;
	th->th_flags = TH_SYN|TH_ACK;
	th->th_win = htons(sc->sc_wnd);
	th->th_urp = 0;

	/* Tack on the TCP options. */
	if ((sc->sc_flags & SCF_NOOPT) == 0) {
		to.to_flags = 0;

		to.to_mss = mssopt;
		to.to_flags = TOF_MSS;
		if (sc->sc_flags & SCF_WINSCALE) {
			to.to_wscale = sc->sc_requested_r_scale;
			to.to_flags |= TOF_SCALE;
		}
		if (sc->sc_flags & SCF_TIMESTAMP) {
			/* Virgin timestamp or TCP cookie enhanced one. */
			to.to_tsval = sc->sc_ts;
			to.to_tsecr = sc->sc_tsreflect;
			to.to_flags |= TOF_TS;
		}
		if (sc->sc_flags & SCF_SACK)
			to.to_flags |= TOF_SACKPERM;
#ifdef TCP_SIGNATURE
		if (sc->sc_flags & SCF_SIGNATURE)
			to.to_flags |= TOF_SIGNATURE;
#endif
		optlen = tcp_addoptions(&to, (u_char *)(th + 1));

		/* Adjust headers by option size. */
		th->th_off = (sizeof(struct tcphdr) + optlen) >> 2;
		m->m_len += optlen;
		m->m_pkthdr.len += optlen;

#ifdef TCP_SIGNATURE
		if (sc->sc_flags & SCF_SIGNATURE)
			tcp_signature_compute(m, sizeof(struct ip), 0, optlen,
			    to.to_signature, IPSEC_DIR_OUTBOUND);
#endif
#ifdef INET6
		if (sc->sc_inc.inc_isipv6)
			ip6->ip6_plen = htons(ntohs(ip6->ip6_plen) + optlen);
		else
#endif
			ip->ip_len += optlen;
	} else
		optlen = 0;

#ifdef INET6
	if (sc->sc_inc.inc_isipv6) {
		th->th_sum = 0;
		th->th_sum = in6_cksum(m, IPPROTO_TCP, hlen,
				       tlen + optlen - hlen);
		ip6->ip6_hlim = in6_selecthlim(NULL, NULL);
		error = ip6_output(m, NULL, NULL, 0, NULL, NULL, NULL);
	} else
#endif
	{
#ifdef MAXHE_TODO
		th->th_sum = in_pseudo(ip->ip_src.s_addr, ip->ip_dst.s_addr,
		    htons(tlen + optlen - hlen + IPPROTO_TCP));
#else
		th->th_sum = 0;
#endif // MAXHE_TODO
		//printf("TCP checksum=0x%x\n", th->th_sum);
		m->m_pkthdr.csum_flags = CSUM_TCP;
		m->m_pkthdr.csum_data = offsetof(struct tcphdr, th_sum);
		error = ip_output(m, sc->sc_ipopts, NULL, 0, NULL, NULL);
	}
	return (error);
}

/*
 * The purpose of SYN cookies is to avoid keeping track of all SYN's we
 * receive and to be able to handle SYN floods from bogus source addresses
 * (where we will never receive any reply).  SYN floods try to exhaust all
 * our memory and available slots in the SYN cache table to cause a denial
 * of service to legitimate users of the local host.
 *
 * The idea of SYN cookies is to encode and include all necessary information
 * about the connection setup state within the SYN-ACK we send back and thus
 * to get along without keeping any local state until the ACK to the SYN-ACK
 * arrives (if ever).  Everything we need to know should be available from
 * the information we encoded in the SYN-ACK.
 *
 * More information about the theory behind SYN cookies and its first
 * discussion and specification can be found at:
 *  http://cr.yp.to/syncookies.html    (overview)
 *  http://cr.yp.to/syncookies/archive (gory details)
 *
 * This implementation extends the orginal idea and first implementation
 * of FreeBSD by using not only the initial sequence number field to store
 * information but also the timestamp field if present.  This way we can
 * keep track of the entire state we need to know to recreate the session in
 * its original form.  Almost all TCP speakers implement RFC1323 timestamps
 * these days.  For those that do not we still have to live with the known
 * shortcomings of the ISN only SYN cookies.
 *
 * Cookie layers:
 *
 * Initial sequence number we send:
 * 31|................................|0
 *    DDDDDDDDDDDDDDDDDDDDDDDDDMMMRRRP
 *    D = MD5 Digest (first dword)
 *    M = MSS index
 *    R = Rotation of secret
 *    P = Odd or Even secret
 *
 * The MD5 Digest is computed with over following parameters:
 *  a) randomly rotated secret
 *  b) struct in_conninfo containing the remote/local ip/port (IPv4&IPv6)
 *  c) the received initial sequence number from remote host
 *  d) the rotation offset and odd/even bit
 *
 * Timestamp we send:
 * 31|................................|0
 *    DDDDDDDDDDDDDDDDDDDDDDSSSSRRRRA5
 *    D = MD5 Digest (third dword) (only as filler)
 *    S = Requested send window scale
 *    R = Requested receive window scale
 *    A = SACK allowed
 *    5 = TCP-MD5 enabled (not implemented yet)
 *    XORed with MD5 Digest (forth dword)
 *
 * The timestamp isn't cryptographically secure and doesn't need to be.
 * The double use of the MD5 digest dwords ties it to a specific remote/
 * local host/port, remote initial sequence number and our local time
 * limited secret.  A received timestamp is reverted (XORed) and then
 * the contained MD5 dword is compared to the computed one to ensure the
 * timestamp belongs to the SYN-ACK we sent.  The other parameters may
 * have been tampered with but this isn't different from supplying bogus
 * values in the SYN in the first place.
 *
 * Some problems with SYN cookies remain however:
 * Consider the problem of a recreated (and retransmitted) cookie.  If the
 * original SYN was accepted, the connection is established.  The second
 * SYN is inflight, and if it arrives with an ISN that falls within the
 * receive window, the connection is killed.
 *
 * Notes:
 * A heuristic to determine when to accept syn cookies is not necessary.
 * An ACK flood would cause the syncookie verification to be attempted,
 * but a SYN flood causes syncookies to be generated.  Both are of equal
 * cost, so there's no point in trying to optimize the ACK flood case.
 * Also, if you don't process certain ACKs for some reason, then all someone
 * would have to do is launch a SYN and ACK flood at the same time, which
 * would stop cookie verification and defeat the entire purpose of syncookies.
 */
static int tcp_sc_msstab[] = { 0, 256, 468, 536, 996, 1452, 1460, 8960 };

static void
syncookie_generate(struct syncache_head *sch, struct syncache *sc,
    u_int32_t *flowlabel)
{
	MD5_CTX ctx;
	u_int32_t md5_buffer[MD5_DIGEST_LENGTH / sizeof(u_int32_t)];
	u_int32_t data;
	u_int32_t *secbits;
	u_int off, pmss, mss;
	int i;

	SCH_LOCK_ASSERT(sch);

	/* Which of the two secrets to use. */
	secbits = sch->sch_oddeven ?
			sch->sch_secbits_odd : sch->sch_secbits_even;

	/* Reseed secret if too old. */
	if (sch->sch_reseed < time_uptime) {
		sch->sch_oddeven = sch->sch_oddeven ? 0 : 1;	/* toggle */
		secbits = sch->sch_oddeven ?
				sch->sch_secbits_odd : sch->sch_secbits_even;
		for (i = 0; i < SYNCOOKIE_SECRET_SIZE; i++)
			secbits[i] = arc4random();
		sch->sch_reseed = time_uptime + SYNCOOKIE_LIFETIME;
	}

	/* Secret rotation offset. */
	off = sc->sc_iss & 0x7;			/* iss was randomized before */

	/* Maximum segment size calculation. */
	pmss = max( min(sc->sc_peer_mss, tcp_mssopt(&sc->sc_inc)), tcp_minmss);
	for (mss = sizeof(tcp_sc_msstab) / sizeof(int) - 1; mss > 0; mss--)
		if (tcp_sc_msstab[mss] <= pmss)
			break;

	/* Fold parameters and MD5 digest into the ISN we will send. */
	data = sch->sch_oddeven;/* odd or even secret, 1 bit */
	data |= off << 1;	/* secret offset, derived from iss, 3 bits */
	data |= mss << 4;	/* mss, 3 bits */

	MD5Init(&ctx);
	MD5Update(&ctx, ((u_int8_t *)secbits) + off,
	    SYNCOOKIE_SECRET_SIZE * sizeof(*secbits) - off);
	MD5Update(&ctx, secbits, off);
	MD5Update(&ctx, &sc->sc_inc, sizeof(sc->sc_inc));
	MD5Update(&ctx, &sc->sc_irs, sizeof(sc->sc_irs));
	MD5Update(&ctx, &data, sizeof(data));
	MD5Final((u_int8_t *)&md5_buffer, &ctx);

	data |= (md5_buffer[0] << 7);
	sc->sc_iss = data;

#ifdef INET6
	*flowlabel = md5_buffer[1] & IPV6_FLOWLABEL_MASK;
#endif

	/* Additional parameters are stored in the timestamp if present. */
	if (sc->sc_flags & SCF_TIMESTAMP) {
		data =  ((sc->sc_flags & SCF_SIGNATURE) ? 1 : 0); /* TCP-MD5, 1 bit */
		data |= ((sc->sc_flags & SCF_SACK) ? 1 : 0) << 1; /* SACK, 1 bit */
		data |= sc->sc_requested_s_scale << 2;  /* SWIN scale, 4 bits */
		data |= sc->sc_requested_r_scale << 6;  /* RWIN scale, 4 bits */
		data |= md5_buffer[2] << 10;		/* more digest bits */
		data ^= md5_buffer[3];
		sc->sc_ts = data;
		sc->sc_tsoff = data - ticks;		/* after XOR */
	}

	tcpstat.tcps_sc_sendcookie++;
	return;
}

static struct syncache *
syncookie_lookup(struct in_conninfo *inc, struct syncache_head *sch, 
    struct syncache *sc, struct tcpopt *to, struct tcphdr *th,
    struct socket *so)
{
	MD5_CTX ctx;
	u_int32_t md5_buffer[MD5_DIGEST_LENGTH / sizeof(u_int32_t)];
	u_int32_t data = 0;
	u_int32_t *secbits;
	tcp_seq ack, seq;
	int off, mss, wnd, flags;

	SCH_LOCK_ASSERT(sch);

	/*
	 * Pull information out of SYN-ACK/ACK and
	 * revert sequence number advances.
	 */
	ack = th->th_ack - 1;
	seq = th->th_seq - 1;
	off = (ack >> 1) & 0x7;
	mss = (ack >> 4) & 0x7;
	flags = ack & 0x7f;

	/* Which of the two secrets to use. */
	secbits = (flags & 0x1) ? sch->sch_secbits_odd : sch->sch_secbits_even;

	/*
	 * The secret wasn't updated for the lifetime of a syncookie,
	 * so this SYN-ACK/ACK is either too old (replay) or totally bogus.
	 */
	if (sch->sch_reseed < time_uptime) {
		return (NULL);
	}

	/* Recompute the digest so we can compare it. */
	MD5Init(&ctx);
	MD5Update(&ctx, ((u_int8_t *)secbits) + off,
	    SYNCOOKIE_SECRET_SIZE * sizeof(*secbits) - off);
	MD5Update(&ctx, secbits, off);
	MD5Update(&ctx, inc, sizeof(*inc));
	MD5Update(&ctx, &seq, sizeof(seq));
	MD5Update(&ctx, &flags, sizeof(flags));
	MD5Final((u_int8_t *)&md5_buffer, &ctx);

	/* Does the digest part of or ACK'ed ISS match? */
	if ((ack & (~0x7f)) != (md5_buffer[0] << 7))
		return (NULL);

	/* Does the digest part of our reflected timestamp match? */
	if (to->to_flags & TOF_TS) {
		data = md5_buffer[3] ^ to->to_tsecr;
		if ((data & (~0x3ff)) != (md5_buffer[2] << 10))
			return (NULL);
	}

	/* Fill in the syncache values. */
	bcopy(inc, &sc->sc_inc, sizeof(struct in_conninfo));
	sc->sc_ipopts = NULL;
	
	sc->sc_irs = seq;
	sc->sc_iss = ack;

#ifdef INET6
	if (inc->inc_isipv6) {
		if (sotoinpcb(so)->in6p_flags & IN6P_AUTOFLOWLABEL)
			sc->sc_flowlabel = md5_buffer[1] & IPV6_FLOWLABEL_MASK;
	} else
#endif
	{
		sc->sc_ip_ttl = sotoinpcb(so)->inp_ip_ttl;
		sc->sc_ip_tos = sotoinpcb(so)->inp_ip_tos;
	}

	/* Additional parameters that were encoded in the timestamp. */
	if (data) {
		sc->sc_flags |= SCF_TIMESTAMP;
		sc->sc_tsreflect = to->to_tsval;
		sc->sc_ts = to->to_tsecr;
		sc->sc_tsoff = to->to_tsecr - ticks;
		sc->sc_flags |= (data & 0x1) ? SCF_SIGNATURE : 0;
		sc->sc_flags |= ((data >> 1) & 0x1) ? SCF_SACK : 0;
		sc->sc_requested_s_scale = min((data >> 2) & 0xf,
		    TCP_MAX_WINSHIFT);
		sc->sc_requested_r_scale = min((data >> 6) & 0xf,
		    TCP_MAX_WINSHIFT);
		if (sc->sc_requested_s_scale || sc->sc_requested_r_scale)
			sc->sc_flags |= SCF_WINSCALE;
	} else
		sc->sc_flags |= SCF_NOOPT;

	wnd = sbspace(&so->so_rcv);
	wnd = imax(wnd, 0);
	wnd = imin(wnd, TCP_MAXWIN);
	sc->sc_wnd = wnd;

	sc->sc_rxmits = 0;
	sc->sc_peer_mss = tcp_sc_msstab[mss];

	tcpstat.tcps_sc_recvcookie++;
	return (sc);
}

/*
 * Returns the current number of syncache entries.  This number
 * will probably change before you get around to calling 
 * syncache_pcblist.
 */

int
syncache_pcbcount(void)
{
	struct syncache_head *sch;
	int count, i;

	for (count = 0, i = 0; i < tcp_syncache.hashsize; i++) {
		/* No need to lock for a read. */
		sch = &tcp_syncache.hashbase[i];
		count += sch->sch_length;
	}
	return count;
}

/*
 * Exports the syncache entries to userland so that netstat can display
 * them alongside the other sockets.  This function is intended to be
 * called only from tcp_pcblist.
 *
 * Due to concurrency on an active system, the number of pcbs exported
 * may have no relation to max_pcbs.  max_pcbs merely indicates the
 * amount of space the caller allocated for this function to use.
 */
int
syncache_pcblist(struct sysctl_req *req, int max_pcbs, int *pcbs_exported)
{
	struct xtcpcb xt;
	struct syncache *sc;
	struct syncache_head *sch;
	int count, error, i;

	for (count = 0, error = 0, i = 0; i < tcp_syncache.hashsize; i++) {
		sch = &tcp_syncache.hashbase[i];
		SCH_LOCK(sch);
		TAILQ_FOREACH(sc, &sch->sch_bucket, sc_hash) {
			if (count >= max_pcbs) {
				SCH_UNLOCK(sch);
				goto exit;
			}
			bzero(&xt, sizeof(xt));
			xt.xt_len = sizeof(xt);
			if (sc->sc_inc.inc_isipv6)
				xt.xt_inp.inp_vflag = INP_IPV6;
			else
				xt.xt_inp.inp_vflag = INP_IPV4;
			bcopy(&sc->sc_inc, &xt.xt_inp.inp_inc, sizeof (struct in_conninfo));
			xt.xt_tp.t_inpcb = &xt.xt_inp;
			xt.xt_tp.t_state = TCPS_SYN_RECEIVED;
			xt.xt_socket.xso_protocol = IPPROTO_TCP;
			xt.xt_socket.xso_len = sizeof (struct xsocket);
			xt.xt_socket.so_type = SOCK_STREAM;
			xt.xt_socket.so_state = SS_ISCONNECTING;
			error = SYSCTL_OUT(req, &xt, sizeof xt);
			if (error) {
				SCH_UNLOCK(sch);
				goto exit;
			}
			count++;
		}
		SCH_UNLOCK(sch);
	}
exit:
	*pcbs_exported = count;
	return error;
}

