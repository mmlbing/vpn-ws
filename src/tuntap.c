#include "vpn-ws.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <net/if_dl.h>
#include <sys/sysctl.h>
#endif

#if defined(__linux__)

#include <linux/if_tun.h>
#define TUNTAP_DEVICE "/dev/net/tun"

int vpn_ws_tuntap(char *name) {
	struct ifreq ifr;
        int fd = open(TUNTAP_DEVICE, O_RDWR);
	if (fd < 0) {
		vpn_ws_error("vpn_ws_tuntap()/open()");
		return -1;
	}

	memset(&ifr, 0, sizeof(struct ifreq));

        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, name, IFNAMSIZ);

	if (ioctl(fd, TUNSETIFF, (void *) &ifr) < 0) {
		vpn_ws_error("vpn_ws_tuntap()/ioctl()");
                return -1;
        }

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		vpn_ws_error("vpn_ws_tuntap()/ioctl()");
                return -1;
	}

	// copy MAC address
	memcpy(vpn_ws_conf.tuntap_mac, ifr.ifr_hwaddr.sa_data, 6);
	//printf("%x %x\n", vpn_ws_conf.tuntap_mac[0], vpn_ws_conf.tuntap_mac[1]);

	return fd;
}

int vpn_ws_update_tuntap_mac(uint8_t *mac_updated) {
	struct ifreq ifr;
	if (mac_updated == NULL) {
		return -1;
	}
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		vpn_ws_error("vpn_ws_update_tuntap_mac()/socket()");
		return -1;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strncpy(ifr.ifr_name, vpn_ws_conf.tuntap_name, IFNAMSIZ);

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		vpn_ws_error("vpn_ws_update_tuntap_mac()/ioctl()");
		close(fd);
		return -1;
	}

	// copy MAC address
	memcpy(vpn_ws_conf.tuntap_mac, ifr.ifr_hwaddr.sa_data, 6);
	memcpy(mac_updated, ifr.ifr_hwaddr.sa_data, 6);

	close(fd);
	return 0;
}

#elif defined(__WIN32__)

#include <winioctl.h>
#include <malloc.h>

#define NETWORK_CONNECTIONS_KEY "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}"

#define TAP_WIN_CONTROL_CODE(request,method) \
  CTL_CODE (FILE_DEVICE_UNKNOWN, request, method, FILE_ANY_ACCESS)

#define TAP_WIN_IOCTL_GET_MAC               TAP_WIN_CONTROL_CODE (1, METHOD_BUFFERED)

#define TAP_WIN_IOCTL_SET_MEDIA_STATUS      TAP_WIN_CONTROL_CODE (6, METHOD_BUFFERED)


HANDLE vpn_ws_tuntap(char *name) {
	HANDLE handle;
	HKEY adapter_key;
	HKEY unit_key;
	DWORD data_type;
	LONG status = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
		NETWORK_CONNECTIONS_KEY, 0,
		KEY_READ, &adapter_key);

	if (status != ERROR_SUCCESS) {
		vpn_ws_error("vpn_ws_tuntap()/RegOpenKeyEx()");
		return NULL;
	}

	int i = 0;
	for(;;) {
		char enum_name[256];
		char unit_name[256];
		DWORD len = sizeof(enum_name);

		status = RegEnumKeyEx(adapter_key, i, enum_name, &len,
			NULL, NULL, NULL, NULL);

		if (status != ERROR_SUCCESS) goto end;
		
		snprintf(unit_name, sizeof(unit_name), "%s\\%s\\Connection",
			NETWORK_CONNECTIONS_KEY, enum_name);	

		status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, unit_name, 0, KEY_READ, &unit_key);

		if (status != ERROR_SUCCESS) goto next;

		char *netname_str = "Name";
		char netname[256];

		len = sizeof(netname);
		status = RegQueryValueEx(unit_key,
			netname_str,
			NULL, &data_type,
			(LPBYTE)netname, &len);
	
		if (status != ERROR_SUCCESS || data_type != REG_SZ) {
			RegCloseKey(unit_key);
			goto next;	
		}

		RegCloseKey(unit_key);

		if (!strcmp(netname, name)) {
			char dev[256];
			snprintf(dev, 256, "\\\\.\\Global\\%s.tap", enum_name); 
			handle = CreateFile(dev,
				GENERIC_READ|GENERIC_WRITE,
				0, 0, OPEN_EXISTING,
				FILE_ATTRIBUTE_SYSTEM|FILE_FLAG_OVERLAPPED, 0);
			if (handle == INVALID_HANDLE_VALUE) {
				vpn_ws_error("vpn_ws_tuntap()/CreateFile()");
				break;
			}

			if (!DeviceIoControl(handle, TAP_WIN_IOCTL_GET_MAC, vpn_ws_conf.tuntap_mac, 6, vpn_ws_conf.tuntap_mac, 6, &len, NULL)) {
				vpn_ws_error("vpn_ws_tuntap()/DeviceIoControl()");
				CloseHandle(handle);
				break;
			}

			ULONG status = TRUE;
			if (!DeviceIoControl(handle, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status), &status, sizeof(status), &len, NULL)) {
				vpn_ws_error("vpn_ws_tuntap()/DeviceIoControl()");
				CloseHandle(handle);
				break;
			}
			return handle;
		}

	
				
next:
		i++;
		
	}
end:
	RegCloseKey(adapter_key);
	return NULL;
}

int vpn_ws_update_tuntap_mac(uint8_t *mac_updated) {
	return 0;
}

#else

#if defined(__APPLE__)
#include <sys/sys_domain.h>
#include <sys/kern_control.h>
// like linux
#define SIOCGIFHWADDR   0x8927
#endif

int vpn_ws_tuntap(char *name) {
	int fd = -1;
#if defined(__APPLE__)
	// is it it.unbit.utap ?
	if (vpn_ws_is_a_number(name)) {
		fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
		if (fd < 0) {
			vpn_ws_error("vpn_ws_tuntap()/socket()");
			return -1;
		}
		struct ctl_info info;
        	memset(&info, 0, sizeof(info));
        	strncpy(info.ctl_name, "it.unbit.utap", sizeof(info.ctl_name));
        	if (ioctl(fd, CTLIOCGINFO, &info)) {
			vpn_ws_error("vpn_ws_tuntap()/ioctl()");
			close(fd);
			return -1;
		}
		struct sockaddr_ctl       addr;
        	memset(&addr, 0, sizeof(addr));
        	addr.sc_len = sizeof(addr);
        	addr.sc_family = AF_SYSTEM;
        	addr.ss_sysaddr = AF_SYS_CONTROL;
        	addr.sc_id = info.ctl_id;
        	addr.sc_unit = atoi(name);

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
			vpn_ws_error("vpn_ws_tuntap()/connect()");
			close(fd);
			return -1;
		}

		socklen_t mac_len = 6;
		if (getsockopt(fd, SYSPROTO_CONTROL, SIOCGIFHWADDR, vpn_ws_conf.tuntap_mac, &mac_len)) {
			vpn_ws_error("vpn_ws_tuntap()/getsockopt()");
                        close(fd);
                        return -1;
		}

		return fd;
	}
	else {
#endif
	fd = open(name, O_RDWR);
	if (fd < 0) {
		vpn_ws_error("vpn_ws_tuntap()/open()");
		return -1;
	}
#if defined(__APPLE__)
	}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	int mib[6];
	mib[0] = CTL_NET;
	mib[1] = AF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_LINK;
	mib[4] = NET_RT_IFLIST;
	mib[5] = if_nametoindex(name+5);
	if (!mib[5]) {
		vpn_ws_error("vpn_ws_tuntap()/if_nametoindex()");
		close(fd);
		return -1;
	}

	size_t len;
	if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0) {
		vpn_ws_error("vpn_ws_tuntap()/sysctl()");
                close(fd);
                return -1;
	}

	char *buf = vpn_ws_malloc(len);
	if (!buf) {
		close(fd);
		return -1;
	}

	if (sysctl(mib, 6, buf, &len, NULL, 0) < 0) {
		vpn_ws_error("vpn_ws_tuntap()/sysctl()");
                close(fd);
                return -1;
	}

	struct if_msghdr *ifm = (struct if_msghdr *)buf;
	struct sockaddr_dl *sdl = (struct sockaddr_dl *)(ifm + 1);
	uint8_t *ptr = (uint8_t *)LLADDR(sdl);
        // copy MAC address
        memcpy(vpn_ws_conf.tuntap_mac, ptr, 6);
#endif

	return fd;
}

int vpn_ws_update_tuntap_mac(uint8_t *mac_updated) {
	return 0;
}

#endif
