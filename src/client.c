#include "vpn-ws.h"

#ifndef __WIN32__
#include <netdb.h>
#include <resolv.h>
#endif

struct vpn_ws_config vpn_ws_conf;

static struct option vpn_ws_options[] = {
        {"exec", required_argument, NULL, 1 },
        {"key", required_argument, NULL, 2 },
        {"crt", required_argument, NULL, 3 },
        {"no-verify", no_argument, &vpn_ws_conf.ssl_no_verify, 1 },
	{"bridge", no_argument, &vpn_ws_conf.bridge, 1 },
        {NULL, 0, 0, 0}
};

#ifdef __WIN32__
/*
	The amount of code here for opening a socket is astonishing....
*/
static HANDLE _vpn_ws_win32_socket(int family, int type, int protocol) {
	unsigned long pblen = 0;
	SOCKET ret;
	WSAPROTOCOL_INFOW *pbuff;
	WSAPROTOCOL_INFOA pinfo;
	int nprotos, i, err;

	if (WSCEnumProtocols(NULL, NULL, &pblen, &err) != SOCKET_ERROR) {
		vpn_ws_log("no socket protocols available");
		return NULL;
	}

	if (err != WSAENOBUFS) {
		vpn_ws_error("WSCEnumProtocols()");
		return NULL;
	}

	pbuff = vpn_ws_malloc(pblen);
	if ((nprotos = WSCEnumProtocols(NULL, pbuff, &pblen, &err)) == SOCKET_ERROR) {
		vpn_ws_error("WSCEnumProtocols()");
		return NULL;
	}

	for (i = 0; i < nprotos; i++) {
		if (pbuff[i].iAddressFamily != family) continue;
		if (pbuff[i].iSocketType != type) continue;
		if (!(pbuff[i].dwServiceFlags1 & XP1_IFS_HANDLES))
			continue;

		memcpy(&pinfo, pbuff + i, sizeof(pinfo));
		wcstombs(pinfo.szProtocol, pbuff[i].szProtocol, sizeof(pinfo.szProtocol));
		free(pbuff);
		if ((ret = WSASocket(family, type, protocol, &pinfo, 0, 0)) == INVALID_SOCKET) {
			vpn_ws_error("WSASocket()");
			return NULL;
		}
		return (HANDLE) ret;
	}
	free(pbuff);
	return NULL;
}
#endif

void vpn_ws_client_destroy(vpn_ws_peer *peer) {
	if (vpn_ws_conf.ssl_ctx) {
		vpn_ws_ssl_close(vpn_ws_conf.ssl_ctx);
	}
	vpn_ws_peer_destroy(peer);
}

int vpn_ws_client_read(vpn_ws_peer *peer, uint64_t amount) {
        uint64_t available = peer->len - peer->pos;
        if (available < amount) {
                peer->len += amount;
                void *tmp = realloc(peer->buf, peer->len);
                if (!tmp) {
                        vpn_ws_error("vpn_ws_client_read()/realloc()");
                        return -1;
                }
                peer->buf = tmp;
        }

	if (vpn_ws_conf.ssl_ctx) {
		ssize_t rlen = vpn_ws_ssl_read(vpn_ws_conf.ssl_ctx, peer->buf + peer->pos, amount);
		if (rlen == 0) {
			return -1;
		}	
		if (rlen > 0) {
        		peer->pos += rlen;
			return 0;
		}
		return rlen;
	}

	vpn_ws_recv(peer->fd, peer->buf + peer->pos, amount, rlen);
        if (rlen < 0) {
		if (rlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) return 0;
		vpn_ws_error("vpn_ws_client_read()/read()");
		return -1;
	}
	else if (rlen == 0) {
		return -1;
	}
        peer->pos += rlen;

        return 0;
}


int vpn_ws_rnrn(char *buf, size_t len) {
	if (len < 17) return 0;
	uint8_t status = 0;
	size_t i;
	for(i=0;i<len;i++) {
		if (status == 0) {
			if (buf[i] == '\r') {
				status = 1;
				continue;
			}
		}
		else if (status == 1) {
			if (buf[i] == '\n') {
				status = 2;
				continue;
			}
		}
		else if (status == 2) {
			if (buf[i] == '\r') {
				status = 3;
				continue;
			}
		}
		else if (status == 3) {
			if (buf[i] == '\n') {
                                status = 4;
				break;
                        }
		}
                status = 0;
	}
	if (status != 4) return 0;
	return vpn_ws_str_to_uint(buf+9, 3);
}

// here the socket is still in blocking state
int vpn_ws_wait_101(vpn_ws_fd fd, void *ssl) {
	char buf[8192];
	size_t remains = 8192;

	for(;;) {
		if (!ssl) {
			vpn_ws_recv(fd, buf + (8192-remains), remains, rlen);
			if (rlen <= 0) {
				vpn_ws_error("vpn_ws_wait_101()/read()");
				return -1;
			}
			remains -= rlen;
		}
		else {
			ssize_t rlen = vpn_ws_ssl_read(ssl, (uint8_t *) buf + (8192-remains), remains);
			if (rlen <= 0) {
				vpn_ws_error("vpn_ws_wait_101()/vpn_ws_ssl_read()");
                                return -1;
			}
			remains -= rlen;
		}

		int code = vpn_ws_rnrn(buf, 8192-remains);
		if (code) return code;
	}
}

int vpn_ws_full_write(vpn_ws_fd fd, char *buf, size_t len) {
	size_t remains = len;
	char *ptr = buf;
	while(remains > 0) {
		vpn_ws_send(fd, ptr, remains, wlen);
		if (wlen <= 0) {
			if (wlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) {
#ifndef __WIN32__
				fd_set wset;
				FD_ZERO(&wset);
				FD_SET(fd, &wset);
				if (select(fd+1, NULL, &wset, NULL, NULL) < 0) {
					vpn_ws_error("vpn_ws_full_write()/select()");
					return -1;
				}
#else
#endif
				continue;
			}
			vpn_ws_error("vpn_ws_full_write()/write()");
			return -1;
		}
		ptr += wlen;
		remains -= wlen;
	}
	return 0;
}

int vpn_ws_client_write(vpn_ws_peer *peer, uint8_t *buf, uint64_t len) {
	if (vpn_ws_conf.ssl_ctx) {
		return vpn_ws_ssl_write(vpn_ws_conf.ssl_ctx, buf, len);
	}
	return vpn_ws_full_write(peer->fd, (char *)buf, len);
}


int vpn_ws_connect(vpn_ws_peer *peer, char *name) {
	static char *cpy = NULL;

	if (cpy) free(cpy);
	cpy = strdup(name);
	
	int ssl = 0;
	uint16_t port = 80;
	if (strlen(cpy) < 6) {
		vpn_ws_warning("invalid websocket url: %s", cpy);
		return -1;
	}

	if (!strncmp(cpy, "wss://", 6)) {
		ssl = 1;
		port = 443;
	}
	else if (!strncmp(cpy, "ws://", 5)) {
		ssl = 0;
		port = 80;
	}
	else {
		vpn_ws_warning("invalid websocket url: %s (requires ws:// or wss://)", cpy);
		return -1;
	}

	char *path = NULL;

	// now get the domain part
	char *domain = cpy + 5 + ssl;
	size_t domain_len = strlen(domain);
	char *slash = strchr(domain, '/');
	if (slash) {
		domain_len = slash - domain;
		domain[domain_len] = 0;
		path = slash + 1;
	}

	// check for basic auth
	char *at = strchr(domain, '@');
	if (at) {
		*at = 0;
		domain = at+1;
		domain_len = strlen(domain);
	}

	// check for port
	char *port_str = strchr(domain, ':');
	if (port_str) {		
		*port_str = 0;
		domain_len = strlen(domain);
		port = atoi(port_str+1);
	}

	vpn_ws_notice("connecting to %s port %u (transport: %s)", domain, port, ssl ? "wss": "ws");

	// resolve the domain
#ifndef __WIN32__
	res_init();
#endif
	struct hostent *he = gethostbyname(domain);
	if (!he) {
		vpn_ws_warning("vpn_ws_connect()/gethostbyname(): unable to resolve name");
		return -1;
	}

#ifndef __WIN32__
	peer->fd = socket(AF_INET, SOCK_STREAM, 0);
#else
	peer->fd = _vpn_ws_win32_socket(AF_INET, SOCK_STREAM, 0);
#endif
	if (vpn_ws_is_invalid_fd(peer->fd)) {
		vpn_ws_error("vpn_ws_connect()/socket()");
		return -1;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr = *((struct in_addr *) he->h_addr);

	if (connect(vpn_ws_socket_cast(peer->fd), (struct sockaddr *) &sin, sizeof(struct sockaddr_in))) {
		vpn_ws_error("vpn_ws_connect()/connect()");
		return -1;
	}

	char *auth = NULL;

	if (at) {
		char *crd = cpy + 5 + ssl;
		auth = vpn_ws_calloc(23 + (strlen(crd) * 2));
		if (!auth) {
			return -1;
		}
		memcpy(auth, "Authorization: Basic ", 21);
		uint16_t auth_len = vpn_ws_base64_encode((uint8_t *)crd, strlen(crd), (uint8_t *)auth + 21);
		memcpy(auth + 21 + auth_len, "\r\n", 2); 
	}

	uint8_t *mac = vpn_ws_conf.tuntap_mac;
	uint8_t key[32];
	uint8_t secret[10];
	int i;
#ifdef __OpenBSD__
	for(i=0;i<10;i++) secret[i] = arc4random();
#else
	for(i=0;i<10;i++) secret[i] = rand();
#endif
	uint16_t key_len = vpn_ws_base64_encode(secret, 10, key);
	// now build and send the request
	char buf[8192];
	int ret = snprintf(buf, 8192, "GET /%s HTTP/1.1\r\nHost: %s%s%s\r\n%sUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %.*s\r\nX-vpn-ws-MAC: %02x:%02x:%02x:%02x:%02x:%02x%s\r\n\r\n",
		path ? path : "",
		domain,
		port_str ? ":" : "",
		port_str ? port_str+1 : "",
		auth ? auth : "",
		key_len,
		key,
		mac[0],	
		mac[1],	
		mac[2],	
		mac[3],	
		mac[4],
		mac[5],
		vpn_ws_conf.bridge ? "\r\nX-vpn-ws-bridge: on" : ""
	);

	if (auth) free(auth);

	if (ret == 0 || ret > 8192) {
		vpn_ws_log("vpn_ws_connect()/snprintf()");
		return -1;
	}

	if (ssl) {
		vpn_ws_conf.ssl_ctx = vpn_ws_ssl_handshake(peer, domain, vpn_ws_conf.ssl_key, vpn_ws_conf.ssl_crt);
		if (!vpn_ws_conf.ssl_ctx) {
			return -1;
		}
		if (vpn_ws_ssl_write(vpn_ws_conf.ssl_ctx, (uint8_t *)buf, ret)) {
			return -1;
		}
	}
	else {
		if (vpn_ws_full_write(peer->fd, buf, ret)) {
			return -1;
		}		
	}

	int http_code = vpn_ws_wait_101(peer->fd, vpn_ws_conf.ssl_ctx);
	if (http_code != 101) {
		vpn_ws_warning("error, websocket handshake returned code: %d", http_code);
		return -1;
	}

	vpn_ws_notice("connected to %s port %u (transport: %s)", domain, port, ssl ? "wss": "ws");
	return 0;
}

int main(int argc, char *argv[]) {

#ifndef __WIN32__
	sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, SIGPIPE);
        sigprocmask(SIG_BLOCK, &sset, NULL);
#else
	// initialize winsock2
	WSADATA wsaData;
	WSAStartup(MAKEWORD(1, 1), &wsaData);
#endif

	int option_index = 0;
	for(;;) {
                int c = getopt_long(argc, argv, "", vpn_ws_options, &option_index);
                if (c < 0) break;
                switch(c) {
			case 0:
				break;	
                        case 1:
                                vpn_ws_conf.exec = optarg;
                                break;
                        case 2:
                                vpn_ws_conf.ssl_key = optarg;
                                break;
                        case 3:
                                vpn_ws_conf.ssl_crt = optarg;
                                break;
                        case '?':
                                break;
                        default:
                                vpn_ws_warning("error parsing arguments");
                                vpn_ws_exit(1);
                }
        }

	if (optind + 1 >= argc) {
		vpn_ws_log("syntax: %s <tap> <ws>", argv[0]);
		vpn_ws_exit(1);
	}

	vpn_ws_conf.tuntap_name = argv[optind];
	vpn_ws_conf.server_addr = argv[optind+1];

	struct timeval tv;
#ifndef __OpenBSD__
	// initialize rnd engine
	gettimeofday(&tv, NULL);
	srand((unsigned int) (tv.tv_usec * tv.tv_sec));
#endif


	vpn_ws_fd tuntap_fd = vpn_ws_tuntap(vpn_ws_conf.tuntap_name);
	if (vpn_ws_is_invalid_fd(tuntap_fd)) {
		vpn_ws_exit(1);
	}

	if (vpn_ws_nb(tuntap_fd)) {
		vpn_ws_exit(1);
	}

	if (vpn_ws_conf.exec) {
		if (vpn_ws_exec(vpn_ws_conf.exec)) {
			vpn_ws_exit(1);
		}
		else {
			// Update the interface MAC after interface up
			uint8_t mac_updated[6];
			if (vpn_ws_update_tuntap_mac(mac_updated) < 0) {
				vpn_ws_exit(1);
			}
			memcpy(vpn_ws_conf.tuntap_mac, mac_updated, 6);
			vpn_ws_log("Interface MAC address updated [%02X:%02X:%02X:%02X:%02X:%02X]",
				vpn_ws_conf.tuntap_mac[0], vpn_ws_conf.tuntap_mac[1], vpn_ws_conf.tuntap_mac[2], vpn_ws_conf.tuntap_mac[3],vpn_ws_conf.tuntap_mac[4], vpn_ws_conf.tuntap_mac[5]); 
		}
	}

	vpn_ws_peer *peer = NULL;

	int throttle = -1;
	// back here whenever the server disconnect
reconnect:
	if (throttle > -1) {
		vpn_ws_log("disconnected");
	}
	if (throttle >= 30) throttle = 0;
	throttle++;
	if (throttle) sleep(throttle);

	peer = vpn_ws_calloc(sizeof(vpn_ws_peer));
        if (!peer) {
		goto reconnect;
        }
	memcpy(peer->mac, vpn_ws_conf.tuntap_mac, 6);

	if (vpn_ws_connect(peer, vpn_ws_conf.server_addr)) {
		vpn_ws_client_destroy(peer);
		goto reconnect;
	}

	// we set the socket in non blocking mode, albeit the code paths are all blocking
	// it is only a secuity measure to avoid dead-blocking the process (as an example select() on Linux is a bit flacky)
	if (vpn_ws_nb(peer->fd)) {
		vpn_ws_client_destroy(peer);
                goto reconnect;
	}

	uint8_t mask[4];
#ifdef __OpenBSD__
	mask[0] = arc4random();
	mask[1] = arc4random();
	mask[2] = arc4random();
	mask[3] = arc4random();
#else
	mask[0] = rand();
	mask[1] = rand();
	mask[2] = rand();
	mask[3] = rand();
#endif

#ifndef __WIN32__
	fd_set rset;
	// find the highest fd
	int max_fd = peer->fd;
	if (tuntap_fd > max_fd) max_fd = tuntap_fd;
	max_fd++;
#else
	WSAEVENT ev = WSACreateEvent();
	WSAEventSelect((SOCKET)peer->fd, ev, FD_READ);
	OVERLAPPED overlapped_read;
	memset(&overlapped_read, 0, sizeof(OVERLAPPED));
	OVERLAPPED overlapped_write;
	memset(&overlapped_write, 0, sizeof(OVERLAPPED));
	overlapped_read.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!overlapped_read.hEvent) {
		vpn_ws_error("main()/CreateEvent()");
		vpn_ws_exit(1);
	}
	HANDLE waiting_objects[2];
	waiting_objects[0] = ev;
	waiting_objects[1] = overlapped_read.hEvent;
	// flag to signal if we need to call RadFile on the tuntap device
	int tuntap_is_reading = 0;
#endif

	for(;;) {
#ifndef __WIN32__
		FD_ZERO(&rset);
		FD_SET(peer->fd, &rset);
		FD_SET(tuntap_fd, &rset);
		tv.tv_sec = 17;
		tv.tv_usec = 0;
		// we send a websocket ping every 17 seconds (if inactive, should be enough
		// for every proxy out there)
		int ret = select(max_fd, &rset, NULL, NULL, &tv);
		if (ret < 0) {
			// the process manager will save us here
			vpn_ws_error("main()/select()");
			vpn_ws_exit(1);
		}
		if (ret == 0) {
#else
		DWORD ret = WaitForMultipleObjects(2, waiting_objects, FALSE, 17000);
		if (ret == WAIT_FAILED) {
			vpn_ws_error("main()/WaitForMultipleObjects()");
			vpn_ws_exit(1);
		}
		if (ret == WAIT_TIMEOUT) {
#endif

		// too much inactivity, send a ping
			if (vpn_ws_client_write(peer, (uint8_t *) "\x89\x00", 2)) {
				vpn_ws_client_destroy(peer);
                		goto reconnect;
			}			
			continue;
		}


#ifndef __WIN32__
		if (FD_ISSET(peer->fd, &rset)) {
#else
		if (ret == WAIT_OBJECT_0) {
#endif
			if (vpn_ws_client_read(peer, 8192)) {
				vpn_ws_client_destroy(peer);
                		goto reconnect;
			}
			
#ifdef __WIN32__
			WSAResetEvent(ev);
#endif
			// start getting websocket packets
			for(;;) {
				uint16_t ws_header = 0;
				int64_t rlen = vpn_ws_websocket_parse(peer, &ws_header);
				if (rlen < 0) {
					vpn_ws_client_destroy(peer);
                                	goto reconnect;
				}
				if (rlen == 0) break;
				// ignore packet ?
				if (ws_header == 0) goto decapitate;
				// is it a masked packet ?
				uint8_t *ws = peer->buf + ws_header;
				uint64_t ws_len = rlen - ws_header;
				if (peer->has_mask) {
                			uint16_t i;
                			for (i=0;i<ws_len;i++) {
                         			ws[i] = ws[i] ^ peer->mask[i % 4];
                			}
				}

#ifndef __WIN32__
				if (vpn_ws_full_write(tuntap_fd, (char *)ws, ws_len)) {
					// being not able to write on tuntap is really bad...
					vpn_ws_exit(1);
				}
#else
				ssize_t wlen = -1;
				if (!WriteFile(tuntap_fd, ws, ws_len, (LPDWORD) &wlen, &overlapped_write)) {
					if (GetLastError() != ERROR_IO_PENDING) {
						vpn_ws_error("main()/WriteFile()");
						vpn_ws_exit(1);
					}
					if (!GetOverlappedResult(tuntap_fd, &overlapped_write, (LPDWORD) &wlen, TRUE)) {
						vpn_ws_error("main()/GetOverlappedResult()");
                                        	vpn_ws_exit(1);
					}	
				}	
#endif

decapitate:
				memmove(peer->buf, peer->buf + rlen, peer->pos - rlen);
        			peer->pos -= rlen;
			}
		}

		
#ifndef __WIN32__
		if (FD_ISSET(tuntap_fd, &rset)) {
			// we use this buffer for the websocket packet too
			// 2 byte header + 2 byte size + 4 bytes masking + mtu
			uint8_t mtu[8+1500];
			vpn_ws_recv(tuntap_fd, mtu+8, 1500, rlen);
			if (rlen <= 0) {
				if (rlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) continue;
				vpn_ws_error("main()/read()");
                        	vpn_ws_exit(1);
			}
#else
		if (ret == WAIT_OBJECT_0+1 || WaitForSingleObject(overlapped_read.hEvent, 0) == WAIT_OBJECT_0) {
			uint8_t mtu[8+1500];
			ssize_t rlen = -1;
			// the tuntap is not reading, call ReadFile
			if (!tuntap_is_reading) {
				if (!ReadFile(tuntap_fd, mtu+8, 1500, (LPDWORD) &rlen, &overlapped_read)) {
					if (GetLastError() != ERROR_IO_PENDING) {
						vpn_ws_error("main()/ReadFile()");
						vpn_ws_exit(1);
					}
					ResetEvent(overlapped_read.hEvent);
					tuntap_is_reading = 1;
					continue;
				}
				tuntap_is_reading = 0;
				SetEvent(overlapped_read.hEvent);
			}
			else {
				if (!GetOverlappedResult(tuntap_fd, &overlapped_read, (LPDWORD)&rlen, TRUE)) {
					vpn_ws_error("main()/GetOverlappedResult()");
					vpn_ws_exit(1);
				}
				tuntap_is_reading = 0;
				SetEvent(overlapped_read.hEvent);
			}
#endif


			// mask packet
			ssize_t i;
			for (i=0;i<rlen;i++) {
                        	mtu[8+i] = mtu[8+i] ^ mask[i % 4];
			}

			mtu[4] = mask[0];
                        mtu[5] = mask[1];
                        mtu[6] = mask[2];
                        mtu[7] = mask[3];

			if (rlen < 126) {
				mtu[2] = 0x82;
				mtu[3] = rlen | 0x80;
				if (vpn_ws_client_write(peer, mtu + 2, rlen + 6)) {
					vpn_ws_client_destroy(peer);
					goto reconnect;
				}
			}
			else {
				mtu[0] = 0x82;
				mtu[1] = 126 | 0x80;
				mtu[2] = (uint8_t) ((rlen >> 8) & 0xff);
				mtu[3] = (uint8_t) (rlen & 0xff);
				if (vpn_ws_client_write(peer, mtu, rlen + 8)) {
					vpn_ws_client_destroy(peer);
					goto reconnect;
				}
			}
		}

	}

	return 0;
}
