/*---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 *
 * Redistributions of source code must retain the above
 * copyright notice, this list of conditions and
 * the following disclaimers.
 *
 *
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 *
 * Neither the names of the University of Illinois, NCSA,
 * nor the names of its contributors may be used to endorse
 * or promote products derived from this Software without
 * specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ________________________________________________________________
 * National Laboratory for Applied Network Research
 * National Center for Supercomputing Applications
 * University of Illinois at Urbana-Champaign
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________
 *
 * Listener.cpp
 * by Mark Gates <mgates@nlanr.net>
 * &  Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * rewritten by Robert McMahon
 * -------------------------------------------------------------------
 * Listener sets up a socket listening on the server host. For each
 * connected socket that accept() returns, this creates a Server
 * socket and spawns a thread for it.
 *
 * Changes to the latest version. Listener will run as a daemon
 * Multicast Server is now Multi-threaded
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <stdio.h>
 *   <string.h>
 *   <errno.h>
 *
 *   <sys/types.h>
 *   <unistd.h>
 *
 *   <netdb.h>
 *   <netinet/in.h>
 *   <sys/socket.h>
 * ------------------------------------------------------------------- */
#define HEADERS()

#include "headers.h"
#include "Listener.hpp"
#include "SocketAddr.h"
#include "PerfSocket.hpp"
#include "active_hosts.h"
#include "util.h"
#include "version.h"
#include "Locale.h"
#include "SocketAddr.h"
#include "payloads.h"
#include "delay.h"
#if (defined HAVE_SSM_MULTICAST) && (defined HAVE_NET_IF_H)
#include <net/if.h>
#endif

#if HAVE_DECL_MSG_WAITALL
#define PEEK_FLAGS (MSG_PEEK | MSG_WAITALL)
#else
#define PEEK_FLAGS (MSG_PEEK)
#endif

/* -------------------------------------------------------------------

 * Stores local hostname and socket info.
 * ------------------------------------------------------------------- */

Listener::Listener (thread_Settings *inSettings) {
    mClients = inSettings->mThreads;
    ListenSocket = INVALID_SOCKET;
    /*
     * These thread settings are stored in three places
     *
     * 1) Listener thread
     * 2) Reporter Thread (per the ReportSettings())
     * 3) Server thread
     */
    mSettings = inSettings;
    // alloc and initialize the buffer (mBuf) used for test messages in the payload
    mBufLen = (mSettings->mBufLen > MINMBUFALLOCSIZE) ? mSettings->mBufLen : MINMBUFALLOCSIZE;
    mBuf = new char[mBufLen]; // defined in payloads.h
    FAIL_errno(mBuf == NULL, "No memory for buffer\n", mSettings);
} // end Listener

/* -------------------------------------------------------------------
 * Delete memory (buffer).
 * ------------------------------------------------------------------- */
Listener::~Listener () {
#if HAVE_THREAD_DEBUG
    thread_debug("Listener destructor close sock=%d", ListenSocket);
#endif
    if (ListenSocket != INVALID_SOCKET) {
        int rc = close(ListenSocket);
        WARN_errno(rc == SOCKET_ERROR, "listener close");
    }
    DELETE_ARRAY(mBuf);
} // end ~Listener

/* -------------------------------------------------------------------
 * This is the main Listener thread loop, listens and accept new
 * connections and starts traffic threads
 *
 * Flow is
 * o) suspend on traffic done for single client case
 * o) hang a select() then accept() on the listener socket
 * o) read or, more accurately, peak the socket for initial messages
 * o) determine and set server's settings flags
 * o) instantiate new settings for listener's clients if needed
 * o) instantiate and bind sum and bidir report objects as needed
 * o) start the threads needed
 *
 * ------------------------------------------------------------------- */
void Listener::Run () {
    // mCount is set True if -P was passed to the server
    int mCount = ((mSettings->mThreads != 0) ?  mSettings->mThreads : -1);

    // This is a listener launched by the client per -r or -d
    if (mSettings->clientListener) {
	SockAddr_remoteAddr(mSettings);
    }
    if (!isUDP(mSettings)) {
	// TCP needs just one listen
	my_listen(); // This will set ListenSocket to a new sock fd
    }
    bool mMode_Time = isServerModeTime(mSettings) && !isDaemon(mSettings);
    if (mMode_Time) {
	mEndTime.setnow();
	mEndTime.add(mSettings->mAmount / 100.0);
    } else if (isPermitKey(mSettings) && (mSettings->mListenerTimeout > 0)) {
	mEndTime.setnow();
	mEndTime.add(mSettings->mListenerTimeout);
    }
    Timestamp now;
#define SINGLECLIENTDELAY_DURATION 50000 // units is microseconds
    while (!sInterupted && mCount) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Listener main loop port %d ", mSettings->mPort);
#endif
	now.setnow();
	if ((mMode_Time || (mSettings->mListenerTimeout > 0)) && mEndTime.before(now)) {
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Listener port %d (loop timer expired)", mSettings->mPort);
#endif
	    break;
	}
	// Serialize in the event the -1 option or --singleclient is set
	int tc;
	if ((isSingleClient(mSettings) || isMulticast(mSettings)) && \
	    mCount && (tc = (thread_numtrafficthreads()) > 0)) {
	    // Start with a delay in the event some traffic
	    // threads are pending to be scheduled and haven't
	    // had a chance to update the traffic thread count.
	    // An event system between listener thread and traffic threads
	    // might better but also more complex. This delay
	    // really should be good enough unless the os scheduler sucks
	    delay_loop(SINGLECLIENTDELAY_DURATION);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Listener single client loop mc/t/mcast/sc %d/%d/%d/%d",mCount, tc, isMulticast(mSettings), isSingleClient(mSettings));
#endif
	    continue;
	}
	if (isUDP(mSettings)) {
	    // UDP needs a new listen per every new socket
	    my_listen(); // This will set ListenSocket to a new sock fd
	}
	// Use a select() with a timeout if -t is set or if this is a v1 -r or -d test
	fd_set set;
	if ((mMode_Time) || isCompat(mSettings) || isPermitKey(mSettings)) {
	    // Hang a select w/timeout on the listener socket
	    struct timeval timeout;
	    if (!isPermitKey(mSettings)) {
		timeout.tv_sec = mSettings->mAmount / 100;
		timeout.tv_usec = (mSettings->mAmount % 100) * 10000;
	    } else {
		timeout.tv_sec = static_cast<long>(mSettings->mListenerTimeout);
		timeout.tv_usec = (static_cast<long>(mSettings->mListenerTimeout) * 1000000) % 1000000;
	    }
	    if (isTxStartTime(mSettings)) {
		now.setnow();
		long adjsecs = (mSettings->txstart_epoch.tv_sec - now.getSecs());
		if (adjsecs > 0)
		    timeout.tv_sec += adjsecs + 1;
	    }
	    FD_ZERO(&set);
	    FD_SET(ListenSocket, &set);
	    if (!(select(ListenSocket + 1, &set, NULL, NULL, &timeout) > 0)) {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("Listener select timeout");
#endif
		if (isCompat(mSettings)) {
		    fprintf(stderr, "ERROR: expected reverse connect did not occur\n");
		    break;
		} else
		    continue;
	    }
	}
	if (!setsock_blocking(mSettings->mSock, true)) {
	    WARN(1, "Failed setting socket to blocking mode");
	}
	// Instantiate another settings object to be used by the server thread
	Settings_Copy(mSettings, &server, 1);
	FAIL(!server, "Failed memory allocation for server settings", mSettings);
	server->mThreadMode = kMode_Server;
	if (!isDataReport(mSettings))
	    setNoDataReport(server);

	// accept a new socket and assign it to the server thread
	int accept_sock = my_accept(server);
	if (!(accept_sock > 0)) {
	    assert(server != mSettings);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Listener thread accept fail %d", accept_sock);
#endif
	    Settings_Destroy(server);
	    continue;
	}

#ifdef HAVE_THREAD_DEBUG
	thread_debug("Listener thread accepted server sock=%d transferID", server->mSock, server->mTransferID);
#endif
	// Decrement the -P counter, commonly usd to kill the listener
	// after one test, i.e. -s -P 1
	if (mCount > 0) {
	    mCount--;
	}
	// These are some exception cases where the accepted socket shouldn't have been
	// accepted but the accept() was first required to figure this out
	//
	// 1) When a client started the listener per -d or -r (but not --reverse.)
	//    What's done here is to see if the server peer opening the
	//    socket matches the expected peer per a compare of the ip addresses
	//    For the case of a *client Listener* the server and  host must match
	//    Note: it's a good idea to prefer --reverse and full duplex socket vs this
	//    -d,-r legacy approach. Still support it though in the name of legacy usage
	//
	// 2) The peer is using a V6 address but the listener/server didn't get -V (for v6) on
	//    it's command line
	//
	if ((mSettings->clientListener && SockAddr_Hostare_Equal(&mSettings->peer, &server->peer)) || \
	    (!isIPV6(mSettings) && SockAddr_isIPv6(&server->peer))) {
	    // Not allowed, reset things and restart the loop
	    // Don't forget to delete the UDP entry (inserted in my_accept)
	    Iperf_remove_host(server);
	    if (DecrSumReportRefCounter(server->mSumReport) <= 0) {
		FreeSumReport(server->mSumReport);
	    }
	    if (!isUDP(server))
	        close(server->mSock);
	    assert(server != mSettings);
	    Settings_Destroy(server);
	    continue;
	}
	// isCompat is a version 1.7 test, basically it indicates there is nothing
	// in the first messages so don't try to process them. Later iperf versions use
	// the first message to convey test request and test settings information.  This flag
	// is also used for threads that are children so-to-speak, e.g. a -d or -r client,
	// which cannot have test flags otherwise there would be "test setup recursion"
	// Time to read the very first packet received (per UDP) or the test flags (TCP)
	// to get the client's requested test information.
	// Note 1: It's important to know that this will also populate mBuf with
	// enough information for the listener to perform test info exchange later in the code
	// Note 2: The mBuf read is a peek so the server's traffic thread started later
	// will also process the first message from an accounting perspective.
	// This is required for accurate traffic statistics
	if (!isCompat(server) && !apply_client_settings(server)) {
	    if (isConnectionReport(server) && !isSumOnly(server)) {
		struct ReportHeader *reporthdr = InitConnectionReport(server, 0);
		struct ConnectionInfo *cr = static_cast<struct ConnectionInfo *>(reporthdr->this_report);
		cr->connect_timestamp.tv_sec = server->accept_time.tv_sec;
		cr->connect_timestamp.tv_usec = server->accept_time.tv_usec;
		assert(reporthdr);
		PostReport(reporthdr);
	    }
	    Iperf_remove_host(server);
	    if (DecrSumReportRefCounter(server->mSumReport) <= 0) {
		FreeSumReport(server->mSumReport);
	    }
	    close(server->mSock);
	    assert(server != mSettings);
	    Settings_Destroy(server);
	    continue;
	}
	// server settings flags should now be set per the client's first message exchange
	// so the server setting's flags per the client can now be checked
	if (isUDP(server) && !isCompat(mSettings)  && (isL2LengthCheck(mSettings) || isL2LengthCheck(server))) {
	    if (!L2_setup(server, server->mSock)) {
		// Requested L2 testing but L2 setup failed
		Iperf_remove_host(server);
		if (DecrSumReportRefCounter(server->mSumReport) <= 0) {
		    FreeSumReport(server->mSumReport);
		}
		assert(server != mSettings);
		Settings_Destroy(server);
		continue;
	    }
	}
	// Force compat mode to use 64 bit seq numbers
	if (isUDP(server) && isCompat(mSettings)) {
	    setSeqNo64b(server);
	}

	// Read any more test settings and test values (not just the flags) and instantiate
	// any settings objects for client threads (e.g. bidir or full duplex)
	// This will set the listener_client_settings to NULL if
	// there is no need for the Listener to start a client
	//
	// Note: the packet payload pointer for this information has different
	// offsets per TCP or UDP. Basically, TCP starts at byte 0 but UDP
	// has to skip over the UDP seq no, etc.
	//
	if (!isCompat(server) && !isCompat(mSettings) && \
	    (isFullDuplex(server) || isServerReverse(server) || (server->mMode != kTest_Normal))) {
	    thread_Settings *listener_client_settings = NULL;
	    // read client header for reverse settings
	    Settings_GenerateClientSettings(server, &listener_client_settings, mBuf);
	    if (listener_client_settings) {
		if (server->mMode != kTest_Normal)
		    listener_client_settings->mTransferID = 0;
		setTransferID(listener_client_settings, 1);
		if (isFullDuplex(listener_client_settings) || isReverse(listener_client_settings))
		    Iperf_push_host(listener_client_settings);
		if (isFullDuplex(server)) {
		    assert(server->mSumReport != NULL);
		    if (!server->mSumReport->sum_fd_set) {
			// Reset the sum output routine for the server sum report
			// now that it's know to be full duplex. This wasn't known
			// during accept()
			SetSumHandlers(server, server->mSumReport);
			server->mSumReport->sum_fd_set = 1;
		    }
		    server->mFullDuplexReport = InitSumReport(server, server->mSock, 1);
		    listener_client_settings->mFullDuplexReport = server->mFullDuplexReport;
#if HAVE_THREAD_DEBUG
		    thread_debug("FullDuplex report client=%p/%p server=%p/%p", (void *) listener_client_settings, (void *) listener_client_settings->mFullDuplexReport, (void *) server, (void *) server->mFullDuplexReport);
#endif
		    server->runNow =  listener_client_settings;
		} else if (server->mMode != kTest_Normal) {
#if HAVE_THREAD_DEBUG
		    thread_debug("V1 test (-d or -r) sum report client=%p/%p server=%p/%p", (void *) listener_client_settings, (void *) listener_client_settings->mFullDuplexReport, (void *) server, (void *) server->mFullDuplexReport);
#endif
		    if (listener_client_settings->mMode == kTest_DualTest) {
#ifdef HAVE_THREAD
			server->runNow =  listener_client_settings;
#else
			server->runNext = listener_client_settings;
#endif
		    } else {
			server->runNext =  listener_client_settings;
		    }
		}
	    }
	}
	setTransferID(server, 0);
	if (isConnectionReport(server) && !isSumOnly(server)) {
	    struct ReportHeader *reporthdr = InitConnectionReport(server, 0);
	    struct ConnectionInfo *cr = static_cast<struct ConnectionInfo *>(reporthdr->this_report);
	    cr->connect_timestamp.tv_sec = server->accept_time.tv_sec;
	    cr->connect_timestamp.tv_usec = server->accept_time.tv_usec;
	    assert(reporthdr);
	    PostReport(reporthdr);
	}
	// Now start the server side traffic threads
	thread_start_all(server);
    }
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Listener exiting port/sig/threads %d/%d/%d", mSettings->mPort, sInterupted, mCount);
#endif
} // end Run

/* -------------------------------------------------------------------
 * Setup a socket listening on a port.
 * For TCP, this calls bind() and listen().
 * For UDP, this just calls bind().
 * If inLocalhost is not null, bind to that address rather than the
 * wildcard server address, specifying what incoming interface to
 * accept connections on.
 * ------------------------------------------------------------------- */
void Listener::my_listen () {
    int rc;

    SockAddr_localAddr(mSettings);

    // create an AF_INET socket for the accepts
    // for the case of L2 testing and UDP, a new AF_PACKET
    // will be created to supercede this one
    int type = (isUDP(mSettings)  ?  SOCK_DGRAM  :  SOCK_STREAM);
    int domain = (SockAddr_isIPv6(&mSettings->local) ?
#ifdef HAVE_IPV6
		  AF_INET6
#else
		  AF_INET
#endif
		  : AF_INET);

#ifdef WIN32
    if (SockAddr_isMulticast(&mSettings->local)) {
	// Multicast on Win32 requires special handling
	ListenSocket = WSASocket(domain, type, 0, 0, 0, WSA_FLAG_MULTIPOINT_C_LEAF | WSA_FLAG_MULTIPOINT_D_LEAF);
	WARN_errno(ListenSocket == INVALID_SOCKET, "socket");

    } else
#endif
	{
	    ListenSocket = socket(domain, type, 0);
	    WARN_errno(ListenSocket == INVALID_SOCKET, "socket");
	}
    mSettings->mSock = ListenSocket;

    SetSocketOptions(mSettings);

    // reuse the address, so we can run if a former server was killed off
    int boolean = 1;
    Socklen_t len = sizeof(boolean);
    rc = setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&boolean), len);
    // bind socket to server address
#ifdef WIN32
    if (SockAddr_isMulticast(&mSettings->local)) {
	// Multicast on Win32 requires special handling
	rc = WSAJoinLeaf(ListenSocket, (sockaddr*) &mSettings->local, mSettings->size_local,0,0,0,0,JL_BOTH);
	WARN_errno(rc == SOCKET_ERROR, "WSAJoinLeaf (aka bind)");
    } else
#endif
	{
	    rc = bind(ListenSocket, reinterpret_cast<sockaddr*>(&mSettings->local), mSettings->size_local);
	    FAIL_errno(rc == SOCKET_ERROR, "bind", mSettings);
	}

    // update the reporter thread
    if (isReport(mSettings) && isSettingsReport(mSettings)) {
        struct ReportHeader *report_settings = InitSettingsReport(mSettings);
	assert(report_settings != NULL);
	// disable future settings reports, listener should only do it once
	unsetReport(mSettings);
	PostReport(report_settings);
    }

    // listen for connections (TCP only).
    // use large (INT_MAX) backlog allowing multiple simultaneous connections
    if (!isUDP(mSettings)) {
	if (isSingleClient(mSettings) || isPermitKey(mSettings)) {
	    rc = listen(ListenSocket, 0 + mSettings->mThreads);
	} else {
	    rc = listen(ListenSocket, INT_MAX);
	}
	WARN_errno(rc == SOCKET_ERROR, "listen");
    } else {
#ifndef WIN32
	// if UDP and multicast, join the group
	if (SockAddr_isMulticast(&mSettings->local)) {
#ifdef HAVE_MULTICAST
	    my_multicast_join();
#else
	    fprintf(stderr, "Multicast not supported");
#endif // HAVE_MULTICAST
	}
#endif
    }
} // end my_listen()

/* -------------------------------------------------------------------
 * Joins the multicast group or source and group (SSM S,G)
 *
 * taken from: https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.1.0/com.ibm.zos.v2r1.hale001/ipv6d0141001708.htm
 *
 * Multicast function	                                        IPv4	                   IPv6	                Protocol-independent
 * ==================                                           ====                       ====                 ====================
 * Level of specified option on setsockopt()/getsockopt()	IPPROTO_IP	           IPPROTO_IPV6	IPPROTO_IP or IPPROTO_IPV6
 * Join a multicast group	                                IP_ADD_MEMBERSHIP          IPV6_JOIN_GROUP	MCAST_JOIN_GROUP
 * Leave a multicast group or leave all sources of that
 *   multicast group	                                        IP_DROP_MEMBERSHIP	   IPV6_LEAVE_GROUP	MCAST_LEAVE_GROUP
 * Select outbound interface for sending multicast datagrams	IP_MULTICAST_IF	IPV6_MULTICAST_IF	NA
 * Set maximum hop count	                                IP_MULTICAST_TTL	   IPV6_MULTICAST_HOPS	NA
 * Enable multicast loopback	                                IP_MULTICAST_LOOP	   IPV6_MULTICAST_LOOP	NA
 * Join a source multicast group	                        IP_ADD_SOURCE_MEMBERSHIP   NA	                MCAST_JOIN_SOURCE_GROUP
 * Leave a source multicast group	                        IP_DROP_SOURCE_MEMBERSHIP  NA	                MCAST_LEAVE_SOURCE_GROUP
 * Block data from a source to a multicast group	        IP_BLOCK_SOURCE   	   NA	                MCAST_BLOCK_SOURCE
 * Unblock a previously blocked source for a multicast group	IP_UNBLOCK_SOURCE	   NA	                MCAST_UNBLOCK_SOURCE
 *
 *
 * Reminder:  The os will decide which version of IGMP or MLD to use.   This may be controlled by system settings, e.g.:
 *
 * [rmcmahon@lvnvdb0987:~/Code/ssm/iperf2-code] $ sysctl -a | grep mld | grep force
 * net.ipv6.conf.all.force_mld_version = 0
 * net.ipv6.conf.default.force_mld_version = 0
 * net.ipv6.conf.lo.force_mld_version = 0
 * net.ipv6.conf.eth0.force_mld_version = 0
 *
 * [rmcmahon@lvnvdb0987:~/Code/ssm/iperf2-code] $ sysctl -a | grep igmp | grep force
 * net.ipv4.conf.all.force_igmp_version = 0
 * net.ipv4.conf.default.force_igmp_version = 0
 * net.ipv4.conf.lo.force_igmp_version = 0
 * net.ipv4.conf.eth0.force_igmp_version = 0
 *
 * ------------------------------------------------------------------- */
void Listener::my_multicast_join () {
    // This is the older mulitcast join code.  Both SSM and binding the
    // an interface requires the newer socket options.  Using the older
    // code here will maintain compatiblity with previous iperf versions
    if (!isSSMMulticast(mSettings) && !mSettings->mIfrname) {
	if (!SockAddr_isIPv6(&mSettings->local)) {
	    struct ip_mreq mreq;
	    memcpy(&mreq.imr_multiaddr, SockAddr_get_in_addr(&mSettings->local), \
		    sizeof(mreq.imr_multiaddr));
	    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	    int rc = setsockopt(ListenSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				 reinterpret_cast<char*>(&mreq), sizeof(mreq));
	    WARN_errno(rc == SOCKET_ERROR, "multicast join");
#if HAVE_DECL_IP_MULTICAST_ALL
	    int mc_all = 0;
	    rc = setsockopt(ListenSocket, IPPROTO_IP, IP_MULTICAST_ALL, (void*) &mc_all, sizeof(mc_all));
	    WARN_errno(rc == SOCKET_ERROR, "ip_multicast_all");
#endif
	} else {
#if (HAVE_IPV6_MULTICAST && (HAVE_DECL_IPV6_JOIN_GROUP || HAVE_DECL_IPV6_ADD_MEMBERSHIP))
	    struct ipv6_mreq mreq;
	    memcpy(&mreq.ipv6mr_multiaddr, SockAddr_get_in6_addr(&mSettings->local), sizeof(mreq.ipv6mr_multiaddr));
	    mreq.ipv6mr_interface = 0;
#if HAVE_DECL_IPV6_JOIN_GROUP
	    int rc = setsockopt(ListenSocket, IPPROTO_IPV6, IPV6_JOIN_GROUP, \
				reinterpret_cast<char*>(&mreq), sizeof(mreq));
#else
	    int rc = setsockopt(ListenSocket, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, \
				reinterpret_cast<char*>(&mreq), sizeof(mreq));
#endif
	    FAIL_errno(rc == SOCKET_ERROR, "multicast v6 join", mSettings);
#else
	    fprintf(stderr, "IPv6 multicast is not supported on this platform\n");
#endif
	}
    } else {
	int rc;
#ifdef HAVE_SSM_MULTICAST
	// Here it's either an SSM S,G multicast join or a *,G with an interface specifier
	// Use the newer socket options when these are specified
	socklen_t socklen = sizeof(struct sockaddr_storage);
	int iface=0;
#ifdef HAVE_NET_IF_H
	/* Set the interface or any */
	if (mSettings->mIfrname) {
	    iface = if_nametoindex(mSettings->mIfrname);
	    FAIL_errno(!iface, "mcast if_nametoindex",mSettings);
	} else {
	    iface = 0;
	}
#endif

        if (isIPV6(mSettings)) {
#ifdef HAVE_IPV6_MULTICAST
	    if (mSettings->mSSMMulticastStr) {
		struct group_source_req group_source_req;
		struct sockaddr_in6 *group;
		struct sockaddr_in6 *source;

		memset(&group_source_req, 0, sizeof(struct group_source_req));

		group_source_req.gsr_interface = iface;
		group=reinterpret_cast<struct sockaddr_in6*>(&group_source_req.gsr_group);
		source=reinterpret_cast<struct sockaddr_in6*>(&group_source_req.gsr_source);
		source->sin6_family = AF_INET6;
		group->sin6_family = AF_INET6;
		/* Set the group */
		rc=getsockname(ListenSocket,reinterpret_cast<struct sockaddr *>(group), &socklen);
		FAIL_errno(rc == SOCKET_ERROR, "mcast join source group getsockname",mSettings);
		group->sin6_port = 0;    /* Ignored */

		/* Set the source, apply the S,G */
		rc=inet_pton(AF_INET6, mSettings->mSSMMulticastStr,&source->sin6_addr);
		FAIL_errno(rc != 1, "mcast v6 join source group pton",mSettings);
		source->sin6_port = 0;    /* Ignored */
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
		source->sin6_len = group->sin6_len;
#endif
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_SOURCE_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IPV6,MCAST_JOIN_SOURCE_GROUP, &group_source_req,
				sizeof(group_source_req));
#endif
		FAIL_errno(rc == SOCKET_ERROR, "mcast v6 join source group",mSettings);
	    } else {
		struct group_req group_req;
		struct sockaddr_in6 *group;

		memset(&group_req, 0, sizeof(struct group_req));

		group_req.gr_interface = iface;
		group=reinterpret_cast<struct sockaddr_in6*>(&group_req.gr_group);
		group->sin6_family = AF_INET6;
		/* Set the group */
		rc=getsockname(ListenSocket,reinterpret_cast<struct sockaddr *>(group), &socklen);
		FAIL_errno(rc == SOCKET_ERROR, "mcast v6 join group getsockname",mSettings);
		group->sin6_port = 0;    /* Ignored */
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IPV6,MCAST_JOIN_GROUP, &group_req,
				sizeof(group_source_req));
#endif
		FAIL_errno(rc == SOCKET_ERROR, "mcast v6 join group",mSettings);
	    }
#else
	    fprintf(stderr, "Unfortunately, IPv6 multicast is not supported on this platform\n");
#endif
	} else {
	    if (mSettings->mSSMMulticastStr) {
		struct sockaddr_in *group;
		struct sockaddr_in *source;

		// Fill out both structures because we don't which one will succeed
		// and both may need to be tried
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE
		struct ip_mreq_source imr;
		memset (&imr, 0, sizeof (imr));
#endif
#ifdef HAVE_STRUCT_GROUP_SOURCE_REQ
		struct group_source_req group_source_req;
		memset(&group_source_req, 0, sizeof(struct group_source_req));
		group_source_req.gsr_interface = iface;
		group=reinterpret_cast<struct sockaddr_in*>(&group_source_req.gsr_group);
		source=reinterpret_cast<struct sockaddr_in*>(&group_source_req.gsr_source);
#else
		struct sockaddr_in imrgroup;
		struct sockaddr_in imrsource;
		group = &imrgroup;
		source = &imrsource;
#endif
		source->sin_family = AF_INET;
		group->sin_family = AF_INET;
		/* Set the group */
		rc=getsockname(ListenSocket,reinterpret_cast<struct sockaddr *>(group), &socklen);
		FAIL_errno(rc == SOCKET_ERROR, "mcast join source group getsockname",mSettings);
		group->sin_port = 0;    /* Ignored */

		/* Set the source, apply the S,G */
		rc=inet_pton(AF_INET,mSettings->mSSMMulticastStr,&source->sin_addr);
		FAIL_errno(rc != 1, "mcast join source pton",mSettings);
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
		source->sin_len = group->sin_len;
#endif
		source->sin_port = 0;    /* Ignored */
		rc = -1;

#if HAVE_DECL_MCAST_JOIN_SOURCE_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IP,MCAST_JOIN_SOURCE_GROUP, &group_source_req,
				sizeof(group_source_req));
#endif

#if HAVE_DECL_IP_ADD_SOURCE_MEMBERSHIP
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE
		// Some operating systems will have MCAST_JOIN_SOURCE_GROUP but still fail
		// In those cases try the IP_ADD_SOURCE_MEMBERSHIP
		if (rc < 0) {
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE_IMR_MULTIADDR_S_ADDR
		    imr.imr_multiaddr = ((const struct sockaddr_in *)group)->sin_addr;
		    imr.imr_sourceaddr = ((const struct sockaddr_in *)source)->sin_addr;
#else
		    // Some Android versions declare mreq_source without an s_addr
		    imr.imr_multiaddr = ((const struct sockaddr_in *)group)->sin_addr.s_addr;
		    imr.imr_sourceaddr = ((const struct sockaddr_in *)source)->sin_addr.s_addr;
#endif
		    rc = setsockopt (ListenSocket, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, reinterpret_cast<char*>(&imr), sizeof (imr));
		}
#endif
#endif
		FAIL_errno(rc == SOCKET_ERROR, "mcast join source group",mSettings);
	    } else {
		struct group_req group_req;
		struct sockaddr_in *group;

		memset(&group_req, 0, sizeof(struct group_req));

		group_req.gr_interface = iface;
		group=reinterpret_cast<struct sockaddr_in*>(&group_req.gr_group);
		group->sin_family = AF_INET;
		/* Set the group */
		rc=getsockname(ListenSocket,reinterpret_cast<struct sockaddr *>(group), &socklen);
		FAIL_errno(rc == SOCKET_ERROR, "mcast join group getsockname",mSettings);
		group->sin_port = 0;    /* Ignored */
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IP,MCAST_JOIN_GROUP, &group_req,
				sizeof(group_source_req));
#endif
		FAIL_errno(rc == SOCKET_ERROR, "mcast join group",mSettings);
	    }
	}

#else
	fprintf(stderr, "Unfortunately, SSM is not supported on this platform\n");
	exit(-1);
#endif
    }
}
// end my_multicast_join()

bool Listener::L2_setup (thread_Settings *server, int sockfd) {
#if defined(HAVE_LINUX_FILTER_H) && defined(HAVE_AF_PACKET)
    //
    //  Supporting parallel L2 UDP threads is a bit tricky.  Below are some notes as to why and the approach used.
    //
    //  The primary issues for UDP are:
    //
    //  1) We want the listener thread to hand off the flow to a server thread and not be burdened by that flow
    //  2) For -P support, the listener thread neads to detect new flows which will share the same UDP port
    //     and UDP is stateless
    //
    //  The listener thread needs to detect new traffic flows and hand them to a new server thread, and then
    //  rehang a listen/accept.  For standard iperf the "flow routing" is done using connect() per the ip quintuple.
    //  The OS will then route established connected flows to the socket descriptor handled by a server thread and won't
    //  burden the listener thread with these packets.
    //
    //  For L2 verification, we have to create a two sockets that will exist for the life of the flow.  A
    //  new packet socket (AF_PACKET) will receive L2 frames and bypasses
    //  the OS network stack.  The original AF_INET socket will still send up packets
    //  to the network stack.
    //
    //  When using packet sockets there is inherent packet duplication, the hand off to a server
    //  thread is not so straight forward as packets will continue being sent up to the listener thread
    //  (technical problem is that packet sockets do not support connect() which binds the IP quintuple as the
    //  forwarding key) Since the Listener uses recvfrom(), there is no OS mechanism to detect new flows nor
    //  to drop packets.  The listener can't listen on quintuple based connected flows because the client's source
    //  port is unknown.  Therefore the Listener thread will continue to receive packets from all established
    //  flows sharing the same dst port which will impact CPU utilization and hence performance.
    //
    //  The technique used to address this is to open an AF_PACKET socket and leave the AF_INET socket open.
    //  (This also aligns with BSD based systems)  The original AF_INET socket will remain in the (connected)
    //  state so the network stack has it's connected state.  A cBPF is then used to cause the kernel to fast drop
    //  those packets.  A cBPF is set up to drop such packets.  The test traffic will then only come over the
    //  packet (raw) socket and not the  AF_INET socket. If we were to try to close the original AF_INET socket
    //  (vs leave it open w/the fast drop cBPF) then the existing traffic will be sent up by the network stack
    //  to he Listener thread, flooding it with packets, again something we want to avoid.
    //
    //  On the packet (raw) socket itself, we do two more things to better handle performance
    //
    //  1)  Use a full quintuple cBPF allowing the kernel to filter packets (allow) per the quintuple
    //  2)  Use the packet fanout option to assign a CBPF to a socket and hence to a single server thread minimizing
    //      duplication (reduce all cBPF's filtering load)
    //
    struct sockaddr *p = reinterpret_cast<sockaddr *>(&server->peer);
    struct sockaddr *l = reinterpret_cast<sockaddr *>(&server->local);
    int rc = 0;

    //
    // Establish a packet (raw) socket to be used by the server thread giving it full L2 packets
    //
    struct sockaddr s;
    socklen_t len = sizeof(s);
    getpeername(sockfd, &s, &len);
    if (isIPV6(server)) {
	server->mSock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IPV6));
	WARN_errno(server->mSock == INVALID_SOCKET, "ip6 packet socket (AF_PACKET)");
	server->l4offset = IPV6HDRLEN + sizeof(struct ether_header);
    } else {
	server->mSock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	WARN_errno(server->mSock == INVALID_SOCKET, "ip packet socket (AF_PACKET)");
	unsetIPV6(server);
	server->l4offset = sizeof(struct iphdr) + sizeof(struct ether_header);
    }
    // Didn't get a valid socket, return now
    if (server->mSock < 0) {
	return false;
    }
    // More per thread settings based on using a packet socket
    server->l4payloadoffset = server->l4offset + sizeof(struct udphdr);
    server->recvflags = MSG_TRUNC;
    // The original AF_INET socket only exists to keep the connected state
    // in the OS for this flow. Fast drop packets there as
    // now packets will use the AF_PACKET (raw) socket
    // Also, store the original AF_INET socket descriptor so it can be
    // closed in the Server's destructor.  (Note: closing the
    // socket descriptors will also free the cBPF.)
    //
    server->mSockDrop = sockfd;
    rc = SockAddr_Drop_All_BPF(sockfd);
    WARN_errno(rc == SOCKET_ERROR, "l2 all drop bpf");

    // Now optimize packet flow up the raw socket
    // Establish the flow BPF to forward up only "connected" packets to this raw socket
    if (l->sa_family == AF_INET6) {
#ifdef HAVE_IPV6
	struct in6_addr *v6peer = SockAddr_get_in6_addr(&server->peer);
	struct in6_addr *v6local = SockAddr_get_in6_addr(&server->local);
	if (isIPV6(server)) {
	    rc = SockAddr_v6_Connect_BPF(server->mSock, v6local, v6peer, (reinterpret_cast<struct sockaddr_in6 *>(l))->sin6_port, (reinterpret_cast<struct sockaddr_in6 *>(p))->sin6_port);
	    WARN_errno(rc == SOCKET_ERROR, "l2 connect ipv6 bpf");
	} else {
	    // This is an ipv4 address in a v6 family (structure), just pull the lower 32 bits for the v4 addr
	    rc = SockAddr_v4_Connect_BPF(server->mSock, v6local->s6_addr32[3], v6peer->s6_addr32[3], (reinterpret_cast<struct sockaddr_in6 *>(l))->sin6_port, (reinterpret_cast<struct sockaddr_in6 *>(p))->sin6_port);
	    WARN_errno(rc == SOCKET_ERROR, "l2 v4in6 connect ip bpf");
	}
#else
	fprintf(stderr, "Unfortunately, IPv6 is not supported on this platform\n");
	return false;
#endif /* HAVE_IPV6 */
    } else {
	rc = SockAddr_v4_Connect_BPF(server->mSock, (reinterpret_cast<struct sockaddr_in *>(l))->sin_addr.s_addr, (reinterpret_cast<struct sockaddr_in *>(p))->sin_addr.s_addr, (reinterpret_cast<struct sockaddr_in *>(l))->sin_port, (reinterpret_cast<struct sockaddr_in *>(p))->sin_port);
	WARN_errno(rc == SOCKET_ERROR, "l2 connect ip bpf");
    }
    return rc >= 0;
#else
    fprintf(stderr, "Client requested --l2checks but not supported on this platform\n");
    return false;
#endif
}


/* ------------------------------------------------------------------------
 * Do the equivalent of an accept() call for UDP sockets. This checks
 * a listening UDP socket for new or first received datagram
 * ------------------------------------------------------------------- ----*/
int Listener::udp_accept (thread_Settings *server) {
    assert(server != NULL);
    int rc;
    assert(ListenSocket > 0);
    // Preset the server socket to INVALID, hang recvfrom on the Listener's socket
    // The INVALID socket is used to keep the while loop going
    server->mSock = INVALID_SOCKET;
    rc = recvfrom(ListenSocket, mBuf, mBufLen, MSG_PEEK, \
		  reinterpret_cast<struct sockaddr*>(&server->peer), &server->size_peer);
#if HAVE_THREAD_DEBUG
    {
	char tmpaddr[200];
	size_t len=200;
	unsigned short port = SockAddr_getPort(&server->peer);
	SockAddr_getHostAddress(&server->peer, tmpaddr, len);
	thread_debug("rcvfrom peer: %s port %d len=%d", tmpaddr, port, rc);
    }
#endif
    FAIL_errno(rc == SOCKET_ERROR, "recvfrom", mSettings);
    if (!(rc < 0) && !sInterupted) {
	// Handle connection for UDP sockets
	int gid = Iperf_push_host_port_conditional(server);
#if HAVE_THREAD_DEBUG
	if (gid < 0)
	    thread_debug("rcvfrom peer: drop duplicate");
#endif
	if (gid > 0) {
	    // We have a new UDP flow (based upon key of quintuple)
	    // so let's hand off this socket
	    // to the server and create a new listener socket
	    server->mSock = ListenSocket;
	    ListenSocket = INVALID_SOCKET;
	    // This connect() will allow the OS to only
	    // send packets with the ip quintuple up to the server
	    // socket and, hence, to the server thread (yet to be created)
	    // This connect() routing is only supported with AF_INET or AF_INET6 sockets,
	    // e.g. AF_PACKET sockets can't do this.  We'll handle packet sockets later
	    // All UDP accepts here will use AF_INET.  This is intentional and needed
	    rc = connect(server->mSock, reinterpret_cast<struct sockaddr*>(&server->peer), server->size_peer);
	    FAIL_errno(rc == SOCKET_ERROR, "connect UDP", mSettings);
	    server->size_local = sizeof(iperf_sockaddr);
	    getsockname(server->mSock, reinterpret_cast<sockaddr*>(&server->local), &server->size_local);
	    SockAddr_Ifrname(server);
	}
    }
    return server->mSock;
}
/* -------------------------------------------------------------------
 * This is called by the Listener thread main loop, return a socket or error
 * ------------------------------------------------------------------- */
int Listener::my_accept (thread_Settings *server) {
    assert(server != NULL);
#ifdef HAVE_THREAD_DEBUG
    if (isUDP(server)) {
	thread_debug("Listener thread listening for UDP (sock=%d)", ListenSocket);
    } else {
	thread_debug("Listener thread listening for TCP (sock=%d)", ListenSocket);
    }
#endif
    SockAddr_zeroAddress(&server->peer);
    server->size_peer = sizeof(iperf_sockaddr);
    server->accept_time.tv_sec = 0;
    server->accept_time.tv_usec = 0;
    if (isUDP(server)) {
	server->mSock = udp_accept(server);
	// note udp_accept will update the active host table
    } else {
	// accept a TCP  connection
	server->mSock = accept(ListenSocket, reinterpret_cast<sockaddr*>(&server->peer), &server->size_peer);
	if (server->mSock > 0) {
	    server->size_local = sizeof(iperf_sockaddr);
	    getsockname(server->mSock, reinterpret_cast<sockaddr*>(&server->local), &server->size_local);
	    SockAddr_Ifrname(server);
	    Iperf_push_host(server);
	}
    }
    if (server->mSock > 0) {
	Timestamp now;
	server->accept_time.tv_sec = now.getSecs();
	server->accept_time.tv_usec = now.getUsecs();
    }
    return server->mSock;
} // end my_accept

// Read deep enough into the packet to get the client settings
// Read the headers but don't pull them from the queue in order to
// preserve server thread accounting, i.e. these exchanges will
// be part of traffic accounting. Return false if it's determined
// this traffic shouldn't be accepted for a test run
// Description of bits and fields is in include/payloads.h
bool Listener::apply_client_settings (thread_Settings *server) {
    assert(server != NULL);
    assert(mBuf != NULL);
    bool rc;

    // Set the receive timeout for the very first read
    int sorcvtimer = TESTEXCHANGETIMEOUT; // 4 sec in usecs
    SetSocketOptionsReceiveTimeout(server, sorcvtimer);
    server->peer_version_u = 0;
    server->peer_version_l = 0;
    server->mMode = kTest_Normal;

    if (isUDP(server)) {
	rc = apply_client_settings_udp(server);
    } else {
	rc = apply_client_settings_tcp(server);
    }
    return rc;
}

inline bool Listener::test_permit_key(uint32_t flags, thread_Settings *server, int keyoffset) {
    if (!(flags & HEADER_KEYCHECK)) {
	server->mKeyCheck= false;
	return false;
    }
    struct permitKey *thiskey = reinterpret_cast<struct permitKey *>(mBuf + (keyoffset - sizeof(thiskey->length)));
    int keylen = ntohs(thiskey->length);
    if ((keylen < MIN_PERMITKEY_LEN) || (keylen > MAX_PERMITKEY_LEN)) {
	server->mKeyCheck= false;
//	fprintf(stderr, "REJECT: key length bounds error (%d)\n", keylen);
	return false;
    }
    if (keylen != static_cast<int>(strlen(server->mPermitKey))) {
	server->mKeyCheck= false;
//	fprintf(stderr, "REJECT: key length mismatch (%d!=%d)\n", keylen, (int) strlen(server->mPermitKey));
	return false;
    }
    if (!isUDP(server)) {
	int n = recvn(server->mSock, mBuf, keyoffset + keylen, PEEK_FLAGS);
	FAIL_errno((n < (keyoffset + keylen)), "read key", server);
	server->skip = n;
    }
    strncpy(server->mPermitKey, thiskey->value, MAX_PERMITKEY_LEN + 1);
    if (strncmp(server->mPermitKey, mSettings->mPermitKey, keylen) != 0) {
	server->mKeyCheck= false;
//	fprintf(stderr, "REJECT: key value mismatch per %s\n", thiskey->value);
	return false;
    }
    server->mKeyCheck= true;
    return true;
}

bool Listener::apply_client_settings_udp (thread_Settings *server) {
    struct client_udp_testhdr *hdr = reinterpret_cast<struct client_udp_testhdr *>(mBuf);
    uint32_t flags = ntohl(hdr->base.flags);
    uint16_t upperflags = 0;
    if (flags & HEADER_SEQNO64B) {
	setSeqNo64b(server);
    }
#if HAVE_THREAD_DEBUG
    thread_debug("UDP test flags = %X", flags);
#endif
    if (flags & HEADER32_SMALL_TRIPTIMES) {
#if HAVE_THREAD_DEBUG
        thread_debug("UDP small header");
#endif
	server->accept_time.tv_sec = ntohl(hdr->seqno_ts.tv_sec);
	server->accept_time.tv_usec = ntohl(hdr->seqno_ts.tv_usec);
	uint32_t seqno = ntohl(hdr->seqno_ts.id);
	if (seqno != 1) {
	    fprintf(stderr, "WARN: first received packet (id=%d) was not first sent packet, report start time will be off\n", seqno);
	}
	setTripTime(server);
	setEnhanced(server);
    } else if ((flags & HEADER_VERSION1) || (flags & HEADER_VERSION2) || (flags & HEADER_EXTEND)) {
	if ((flags & HEADER_VERSION1) && !(flags & HEADER_VERSION2)) {
	    if (flags & RUN_NOW)
		server->mMode = kTest_DualTest;
	    else
		server->mMode = kTest_TradeOff;
	}
	if (flags & HEADER_EXTEND) {
	    upperflags = htons(hdr->extend.upperflags);
	    server->mTOS = ntohs(hdr->extend.tos);
	    server->peer_version_u = ntohl(hdr->extend.version_u);
	    server->peer_version_l = ntohl(hdr->extend.version_l);
	    if (flags & HEADER_UDPTESTS) {
		// Handle stateless flags
		if (upperflags & HEADER_ISOCH) {
		    setIsochronous(server);
		}
		if (upperflags & HEADER_L2ETHPIPV6) {
		    setIPV6(server);
		} else {
		    unsetIPV6(server);
		}
		if (upperflags & HEADER_L2LENCHECK) {
		    setL2LengthCheck(server);
		}
		if (upperflags & HEADER_NOUDPFIN) {
		    setNoUDPfin(server);
		}
	    }
	    if (upperflags & HEADER_EPOCH_START) {
		server->txstart_epoch.tv_sec = ntohl(hdr->start_fq.start_tv_sec);
		server->txstart_epoch.tv_usec = ntohl(hdr->start_fq.start_tv_usec);
		Timestamp now;
		if ((abs(now.getSecs() - server->txstart_epoch.tv_sec)) > (MAXDIFFTXSTART + 1)) {
		    fprintf(stdout,"WARN: ignore --txstart-time because client didn't provide valid start timestamp within %d seconds of now\n", MAXDIFFTXSTART);
		    unsetTxStartTime(server);
		} else {
		    setTxStartTime(server);
		}
	    }
	    if (upperflags & HEADER_TRIPTIME) {
		server->accept_time.tv_sec = ntohl(hdr->start_fq.start_tv_sec);
		server->accept_time.tv_usec = ntohl(hdr->start_fq.start_tv_usec);
		Timestamp now;
		if (!isTxStartTime(server) && ((abs(now.getSecs() - server->accept_time.tv_sec)) > (MAXDIFFTIMESTAMPSECS + 1))) {
		    fprintf(stdout,"WARN: ignore --trip-times because client didn't provide valid start timestamp within %d seconds of now\n", MAXDIFFTIMESTAMPSECS);
		} else {
		    setTripTime(server);
		    setEnhanced(server);
		}
	    }
	}
	if (flags & HEADER_VERSION2) {
	    upperflags = htons(hdr->extend.upperflags);
	    if (upperflags & HEADER_FULLDUPLEX) {
		setFullDuplex(server);
		setServerReverse(server);
	    }
	    if (upperflags & HEADER_REVERSE)  {
		server->mThreadMode=kMode_Client;
		setServerReverse(server);
		setNoUDPfin(server);
		unsetReport(server);
	    }
	}
    }
    return true;
}
bool Listener::apply_client_settings_tcp (thread_Settings *server) {
    bool rc = false;
    int n = recvn(server->mSock, mBuf, sizeof(uint32_t), PEEK_FLAGS);
    if (n == 0) {
	//peer closed the socket, with no writes e.g. a connect-only test
	WARN(1, "read tcp flags (peer close)");
	goto DONE;
    }
    if (n < (int) sizeof(uint32_t)) {
	WARN(1, "read tcp flags (runt)");
	goto DONE;
    } else {
	rc = true;
	struct client_tcp_testhdr *hdr = reinterpret_cast<struct client_tcp_testhdr *>(mBuf);
	uint32_t flags = ntohl(hdr->base.flags);
	uint16_t upperflags = 0;
	int peeklen;
	if ((flags & HEADER_VERSION1) || (flags & HEADER_VERSION2) || (flags & HEADER_EXTEND) || isPermitKey(mSettings)) {
	    // figure out the length of the test header
	    if ((peeklen = Settings_ClientHdrPeekLen(flags)) > 0) {
		// read the test settings passed to the server by the client
		int n = recvn(server->mSock, mBuf, peeklen, PEEK_FLAGS);
		FAIL_errno((n < peeklen), "read tcp test info", server);
		server->skip = n;
		if (isPermitKey(mSettings)) {
		    if (!test_permit_key(flags, server, peeklen)) {
			rc = false;
			goto DONE;
		    }
		} else if (flags & HEADER_KEYCHECK) {
		    rc = false;
		    server->mKeyCheck = false;
		    goto DONE;
		}
		struct client_tcp_testhdr *hdr = reinterpret_cast<struct client_tcp_testhdr*>(mBuf);
		if ((flags & HEADER_VERSION1) && !(flags & HEADER_VERSION2)) {
		    if (flags & RUN_NOW)
			server->mMode = kTest_DualTest;
		    else
			server->mMode = kTest_TradeOff;
		}
		if (flags & HEADER_EXTEND) {
		    upperflags = htons(hdr->extend.upperflags);
		    server->mTOS = ntohs(hdr->extend.tos);
		    server->peer_version_u = ntohl(hdr->extend.version_u);
		    server->peer_version_l = ntohl(hdr->extend.version_l);
		    if (upperflags & HEADER_ISOCH) {
			setIsochronous(server);
		    }
		    if (upperflags & HEADER_EPOCH_START) {
			server->txstart_epoch.tv_sec = ntohl(hdr->start_fq.start_tv_sec);
			server->txstart_epoch.tv_usec = ntohl(hdr->start_fq.start_tv_usec);
			Timestamp now;
			if ((abs(now.getSecs() - server->txstart_epoch.tv_sec)) > (MAXDIFFTXSTART + 1)) {
			    fprintf(stdout,"WARN: ignore --txstart-time because client didn't provide valid start timestamp within %d seconds of now\n", MAXDIFFTXSTART);
			    unsetTxStartTime(server);
			} else {
			    setTxStartTime(server);
			}
		    }
		    if (upperflags & HEADER_TRIPTIME) {
			Timestamp now;
			if (!isTxStartTime(server) && ((abs(now.getSecs() - server->accept_time.tv_sec)) > (MAXDIFFTIMESTAMPSECS + 1))) {
			    fprintf(stdout,"WARN: ignore --trip-times because client didn't provide valid start timestamp within %d seconds of now\n", MAXDIFFTIMESTAMPSECS);
			} else {
			    setTripTime(server);
			    setEnhanced(server);
			}
		    }
		    if (upperflags & HEADER_PERIODICBURST) {
			setEnhanced(server);
			setFrameInterval(server);
			setPeriodicBurst(server);
			{
			    struct client_tcp_testhdr *hdr = reinterpret_cast<struct client_tcp_testhdr *>(mBuf);
			    server->mFPS = ntohl(hdr->isoch_settings.FPSl);
			    server->mFPS += ntohl(hdr->isoch_settings.FPSu) / static_cast<double>(rMillion);
			}
			if (!server->mFPS) {
			    server->mFPS = 1.0;
			}
		    }
		    if (flags & HEADER_VERSION2) {
			if (upperflags & HEADER_FULLDUPLEX) {
			    setFullDuplex(server);
			    setServerReverse(server);
			}
			if (upperflags & HEADER_REVERSE) {
			    server->mThreadMode=kMode_Client;
			    setServerReverse(server);
			}
		    }
		}
	    }
	    // Handle case that requires an ack back to the client
	    // Signaled by not UDP (only supported by TCP)
	    // and either 2.0.13 flags or the newer 2.0.14 flag of
	    // V2PEERDETECT
	    if (!isUDP(server) && !isCompat(mSettings) && \
		((!(flags & HEADER_VERSION2) && (flags & HEADER_EXTEND)) || \
		 (flags & HEADER_V2PEERDETECT))) {
		client_test_ack(server);
	    }
	}
    }
  DONE:
    return rc;
}

int Listener::client_test_ack(thread_Settings *server) {
    client_hdr_ack ack;
    int sotimer = 0;
    int optflag;
    ack.typelen.type  = htonl(CLIENTHDRACK);
    ack.typelen.length = htonl(sizeof(client_hdr_ack));
    ack.flags = 0;
    ack.reserved1 = 0;
    ack.reserved2 = 0;
    ack.version_u = htonl(IPERF_VERSION_MAJORHEX);
    ack.version_l = htonl(IPERF_VERSION_MINORHEX);
    int rc = 1;
    // This is a version 2.0.10 or greater client
    // write back to the client so it knows the server
    // version
    if (!isUDP(server)) {
	// sotimer units microseconds convert
	if (server->mInterval) {
	    sotimer = static_cast<int>((server->mInterval) / 4);
	} else if (isModeTime(server)) {
	    sotimer = static_cast<int>((server->mAmount * 10000) / 4);
	}
	if (sotimer > HDRXACKMAX) {
	    sotimer = HDRXACKMAX;
	} else if (sotimer < HDRXACKMIN) {
	    sotimer = HDRXACKMIN;
	}
	SetSocketOptionsSendTimeout(server, sotimer);
#ifdef TCP_NODELAY
	optflag=1;
	// Disable Nagle to reduce latency of this intial message
	if ((rc = setsockopt(server->mSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&optflag), sizeof(int))) < 0) {
	    WARN_errno(rc < 0, "tcpnodelay");
	}
#endif
    }
    if ((rc = send(server->mSock, reinterpret_cast<const char*>(&ack), sizeof(client_hdr_ack),0)) < 0) {
	WARN_errno(rc <= 0, "send_ack");
	rc = 0;
    }
    // Re-nable Nagle
    optflag=0;
    if (!isUDP(server) && (rc = setsockopt(server->mSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&optflag), sizeof(int))) < 0) {
	WARN_errno(rc < 0, "tcpnodelay");
    }
    return rc;
}
