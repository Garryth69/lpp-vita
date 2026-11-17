/*----------------------------------------------------------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#------  This File is Part Of : ----------------------------------------------------------------------------------------#
#------- _  -------------------  ______   _   --------------------------------------------------------------------------#
#------ | | ------------------- (_____ \ | |  --------------------------------------------------------------------------#
#------ | | ---  _   _   ____    _____) )| |  ____  _   _   ____   ____   ----------------------------------------------#
#------ | | --- | | | | / _  |  |  ____/ | | / _  || | | | / _  ) / ___)  ----------------------------------------------#
#------ | |_____| |_| |( ( | |  | |      | |( ( | || |_| |( (/ / | |  --------------------------------------------------#
#------ |_______)\____| \_||_|  |_|      |_| \_||_| \__  | \____)|_|  --------------------------------------------------#
#------------------------------------------------- (____/  -------------------------------------------------------------#
#------------------------   ______   _   -------------------------------------------------------------------------------#
#------------------------  (_____ \ | |  -------------------------------------------------------------------------------#
#------------------------   _____) )| | _   _   ___   ------------------------------------------------------------------#
#------------------------  |  ____/ | || | | | /___)  ------------------------------------------------------------------#
#------------------------  | |      | || |_| ||___ |  ------------------------------------------------------------------#
#------------------------  |_|      |_| \____|(___/   ------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#- Licensed under the GPL License --------------------------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#- Copyright (c) Nanni <lpp.nanni@gmail.com> ---------------------------------------------------------------------------#
#- Copyright (c) Rinnegatamante <rinnegatamante@gmail.com> -------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#- Credits : -----------------------------------------------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------#
#- All the devs involved in Rejuvenate and vita-toolchain --------------------------------------------------------------#
#- xerpi for drawing libs and for FTP server code ----------------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------*/
#define NET_INIT_SIZE 1*1024*1024
#define _GNU_SOURCE
#include <vitasdk.h>
#include <curl/curl.h>
#include <stdio.h>
#include "include/luaplayer.h"
extern "C"{
	#include "include/ftp/ftp.h"
}

#define stringify(str) #str
#define VariableRegister(lua, value) do { lua_pushinteger(lua, value); lua_setglobal (lua, stringify(value)); } while(0)

static void* net_memory = NULL;
static char vita_ip[16];
static bool isNet = false;
static CURL *curl_handle = NULL;
static uint64_t total_bytes = 0xFFFFFFFF;
static uint64_t downloaded_bytes = 0;

typedef struct
{
	unsigned char *ptr = (unsigned char*)malloc(1);
	size_t length = 1;
	void NetString() { ptr[0] = 0; }
	void concat(void* data, size_t size)
	{
		ptr = (unsigned char*)realloc(ptr, length + size);
		memcpy(ptr + length - 1, data, size);
		length += size;
		ptr[length - 1] = 0;
	};
} NetString;

typedef struct
{
	uint32_t magic;
	int sock;
	bool serverSocket;
} Socket;

#define MAX_NAME 512
struct hostent{
  char  *h_name;         /* official (cannonical) name of host               */
  char **h_aliases;      /* pointer to array of pointers of alias names      */
  int    h_addrtype;     /* host address type: AF_INET                       */
  int    h_length;       /* length of address: 4                             */
  char **h_addr_list;    /* pointer to array of pointers with IPv4 addresses */
};
#define h_addr h_addr_list[0]

// Copy-pasted from xyz code
static struct hostent *gethostbyname(const char *name)
{
	static hostent ent;
	static char sname[MAX_NAME] = "";
	static SceNetInAddr saddr = { 0 };
	static char *addrlist[2] = { (char *) &saddr, NULL };

	int rid = sceNetResolverCreate("resolver", NULL, 0);
	if(rid < 0) return NULL;

	int err = sceNetResolverStartNtoa(rid, name, &saddr, 0, 0, 0);
	sceNetResolverDestroy(rid);
	if(err < 0) return NULL;

	ent.h_name = sname;
	ent.h_aliases = 0;
	ent.h_addrtype = SCE_NET_AF_INET;
	ent.h_length = sizeof(struct SceNetInAddr);
	ent.h_addr_list = addrlist;
	ent.h_addr = addrlist[0];

	return &ent;
}

// ---------------------------------------------------------------------------------------------------------------------
// Otakio request core
// Every request (sync or async, string or file) goes through one setup: arguments are copied as plain strings (never
// used as printf formats), the transfer is compressed (gzip/deflate), DNS answers, TLS sessions, open connections and
// cookies are shared between requests, and the outcome (HTTP status, final URL, content type, curl error) is kept for
// Network.getLastResponse().
// ---------------------------------------------------------------------------------------------------------------------

#define NET_BUFFER_SIZE (64 * 1024)
#define OTAKIO_ENGINE_VERSION 2

static CURLSH *curl_share = NULL;
static CURL *sync_handle = NULL;
static SceUID share_locks[CURL_LOCK_DATA_LAST];

static void share_lock(CURL *handle, curl_lock_data data, curl_lock_access access, void *userptr)
{
	if (data >= 0 && data < CURL_LOCK_DATA_LAST && share_locks[data] > 0)
		sceKernelLockMutex(share_locks[data], 1, NULL);
}

static void share_unlock(CURL *handle, curl_lock_data data, void *userptr)
{
	if (data >= 0 && data < CURL_LOCK_DATA_LAST && share_locks[data] > 0)
		sceKernelUnlockMutex(share_locks[data], 1);
}

static void share_init()
{
	if (curl_share != NULL) return;
	for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
		share_locks[i] = sceKernelCreateMutex("curl share", 0, 0, NULL);
	curl_share = curl_share_init();
	curl_share_setopt(curl_share, CURLSHOPT_LOCKFUNC, share_lock);
	curl_share_setopt(curl_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
	curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
	curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
}

static void share_term()
{
	if (curl_share == NULL) return;
	curl_share_cleanup(curl_share);
	curl_share = NULL;
	for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
	{
		if (share_locks[i] > 0) sceKernelDeleteMutex(share_locks[i]);
		share_locks[i] = 0;
	}
}

typedef struct
{
	char *url;
	char *dest;
	char *useragent;
	char *postdata;
	size_t postsize;
	char *cookie;
	char *headers[4];
	char *proxy;
	char *proxyAuth;
	int method;
	int contentType;
	long lowSpeedTime;
} NetRequest;

typedef struct
{
	long status;
	int code;
	char url[1024];
	char contentType[128];
	char error[CURL_ERROR_SIZE];
	double seconds;
	curl_off_t bytes;
	bool decoded; // the body came compressed and was inflated
} NetResponse;

static NetResponse lastAsync;
static NetResponse lastSync;

static const char *DEFAULT_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

static char *dupStr(const char *s)
{
	return strdup(s != NULL ? s : "");
}

// The app doubles every '%' of a link: the old engine copied links with sprintf(dst, link), which turned "%%" back
// into "%". Kept so links built for either engine work.
static char *dupUrl(const char *s)
{
	if (s == NULL) return strdup("");
	size_t n = strlen(s);
	char *out = (char*)malloc(n + 1);
	size_t j = 0;
	for (size_t i = 0; i < n; i++)
	{
		out[j++] = s[i];
		if (s[i] == '%' && s[i + 1] == '%') i++;
	}
	out[j] = 0;
	return out;
}

static void freeRequest(NetRequest *r)
{
	free(r->url);
	free(r->dest);
	free(r->useragent);
	free(r->postdata);
	free(r->cookie);
	for (int i = 0; i < 4; i++) free(r->headers[i]);
	free(r->proxy);
	free(r->proxyAuth);
	memset(r, 0, sizeof(NetRequest));
}

// Lua arguments: url, [dest,] useragent, method, postdata, contentType, cookie, header1..4, proxy, proxyAuth
static void readRequest(lua_State *L, NetRequest *r, bool withDest, int argc)
{
	int i = 1;
	memset(r, 0, sizeof(NetRequest));
	r->url = dupUrl(luaL_checkstring(L, i++));
	if (withDest) r->dest = dupStr(luaL_checkstring(L, i++));
	r->useragent = dupStr(argc >= i ? luaL_checkstring(L, i) : DEFAULT_UA); i++;
	r->method = argc >= i ? luaL_checkinteger(L, i) : SCE_HTTP_METHOD_GET; i++;
	if (argc >= i)
	{
		size_t len = 0;
		const char *data = luaL_checklstring(L, i, &len);
		r->postdata = (char*)malloc(len + 1);
		memcpy(r->postdata, data, len);
		r->postdata[len] = 0;
		r->postsize = len;
	}
	i++;
	r->contentType = argc >= i ? luaL_checkinteger(L, i) : 1; i++;
	r->cookie = dupStr(argc >= i ? luaL_checkstring(L, i) : ""); i++;
	for (int h = 0; h < 4; h++, i++) r->headers[h] = dupStr(argc >= i ? luaL_checkstring(L, i) : "");
	r->proxy = dupStr(argc >= i ? luaL_checkstring(L, i) : ""); i++;
	r->proxyAuth = dupStr(argc >= i ? luaL_checkstring(L, i) : "");
	r->lowSpeedTime = 10L;
}

static volatile bool stopAsyncDownload = false;
static volatile bool asyncProgress = false;

static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	if (clientp != NULL)
	{
		// bytes on the wire (compressed when the body is): the same unit as Content-Length
		downloaded_bytes = dlnow;
		if (dltotal > 0) total_bytes = dltotal;
		return stopAsyncDownload ? 1 : 0;
	}
	return 0;
}

static size_t write_file(void *ptr, size_t size, size_t nmemb, void *stream)
{
	SceUID *fd = (SceUID*)stream;
	int written = sceIoWrite(*fd, ptr, size * nmemb);
	return written < 0 ? 0 : (size_t)written;
}

typedef struct
{
	unsigned char *ptr;
	size_t length;
	size_t capacity;
} NetBuffer;

static size_t write_buffer(void *ptr, size_t size, size_t nmemb, void *stream)
{
	NetBuffer *b = (NetBuffer*)stream;
	size_t add = size * nmemb;
	if (b->length + add + 1 > b->capacity)
	{
		size_t cap = b->capacity ? b->capacity : 16384;
		while (cap < b->length + add + 1) cap *= 2;
		unsigned char *grown = (unsigned char*)realloc(b->ptr, cap);
		if (grown == NULL) return 0;
		b->ptr = grown;
		b->capacity = cap;
	}
	memcpy(b->ptr + b->length, ptr, add);
	b->length += add;
	b->ptr[b->length] = 0;
	return add;
}

static struct curl_slist *setupRequest(CURL *h, NetRequest *r, bool compressed, bool progress, char *errbuf)
{
	curl_easy_reset(h);
	curl_easy_setopt(h, CURLOPT_SHARE, curl_share);
	curl_easy_setopt(h, CURLOPT_URL, r->url);
	curl_easy_setopt(h, CURLOPT_USERAGENT, r->useragent);
	// the cookie engine: cookies a site sets are sent back to it (also across redirects), like a browser session
	curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");
	if (r->cookie[0]) curl_easy_setopt(h, CURLOPT_COOKIE, r->cookie);
	if (strlen(r->proxy) > 2) curl_easy_setopt(h, CURLOPT_PROXY, r->proxy);
	if (strlen(r->proxyAuth) > 2) curl_easy_setopt(h, CURLOPT_PROXYUSERPWD, r->proxyAuth);
	switch (r->method)
	{
	case SCE_HTTP_METHOD_POST:
		curl_easy_setopt(h, CURLOPT_POST, 1L);
		curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)r->postsize);
		curl_easy_setopt(h, CURLOPT_POSTFIELDS, r->postdata ? r->postdata : "");
		break;
	case SCE_HTTP_METHOD_HEAD:
		curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
		break;
	default:
		curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
		break;
	}
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(h, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_0);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, r->lowSpeedTime);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
	if (compressed) curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(h, CURLOPT_BUFFERSIZE, (long)NET_BUFFER_SIZE);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 30L);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 15L);
	curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
	errbuf[0] = 0;
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, progress ? 0L : 1L);
	if (progress)
	{
		curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
		curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void*)1);
	}
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: */*");
	if (r->method == SCE_HTTP_METHOD_POST)
		headers = curl_slist_append(headers, r->contentType == 0 ? "Content-Type: application/json" : "Content-Type: application/x-www-form-urlencoded");
	for (int i = 0; i < 4; i++)
		if (r->headers[i] && r->headers[i][0]) headers = curl_slist_append(headers, r->headers[i]);
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
	return headers;
}

static void keepResponse(CURL *h, int res, const char *errbuf, NetResponse *out, bool decoded)
{
	memset(out, 0, sizeof(NetResponse));
	out->code = res;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
	char *s = NULL;
	if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) strncpy(out->url, s, sizeof(out->url) - 1);
	s = NULL;
	if (curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &s) == CURLE_OK && s) strncpy(out->contentType, s, sizeof(out->contentType) - 1);
	curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &out->seconds);
	curl_easy_getinfo(h, CURLINFO_SIZE_DOWNLOAD_T, &out->bytes);
	if (res != CURLE_OK)
		strncpy(out->error, errbuf[0] ? errbuf : curl_easy_strerror((CURLcode)res), sizeof(out->error) - 1);
	out->decoded = decoded;
}

// Runs a request into a file (dest) or a buffer. A body the server compressed wrongly is asked again uncompressed.
static int performRequest(CURL *h, NetRequest *r, NetBuffer *buffer, bool progress, NetResponse *out)
{
	static char errbuf_async[CURL_ERROR_SIZE];
	static char errbuf_sync[CURL_ERROR_SIZE];
	char *errbuf = progress ? errbuf_async : errbuf_sync;
	int res = CURLE_OK;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		bool compressed = attempt == 0;
		struct curl_slist *headers = setupRequest(h, r, compressed, progress, errbuf);
		SceUID fd = -1;
		if (r->dest)
		{
			fd = sceIoOpen(r->dest, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
			if (fd < 0)
			{
				curl_slist_free_all(headers);
				memset(out, 0, sizeof(NetResponse));
				out->code = CURLE_WRITE_ERROR;
				strncpy(out->error, "can't open the destination file", sizeof(out->error) - 1);
				return CURLE_WRITE_ERROR;
			}
			curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file);
			curl_easy_setopt(h, CURLOPT_WRITEDATA, &fd);
		}
		else
		{
			buffer->length = 0;
			curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_buffer);
			curl_easy_setopt(h, CURLOPT_WRITEDATA, buffer);
		}
		res = curl_easy_perform(h);
		curl_slist_free_all(headers);
		if (fd >= 0)
		{
			sceIoClose(fd);
			if (res != CURLE_OK) sceIoRemove(r->dest);
		}
		keepResponse(h, res, errbuf, out, compressed);
		if (res != CURLE_BAD_CONTENT_ENCODING || stopAsyncDownload) break;
	}
	return res;
}

#define FILE_DOWNLOAD   0
#define STRING_DOWNLOAD 1
#define DOWNLOAD_END    2
static volatile uint8_t asyncMode = DOWNLOAD_END;
static NetRequest asyncReq;
static long asyncConTime = 10L;
static const uint8_t JSON = 0;
static const uint8_t XWWW = 1;

static int downloadThread(unsigned int args, void* arg)
{
	NetBuffer buffer = {NULL, 0, 0};
	asyncReq.lowSpeedTime = asyncConTime;
	int res = performRequest(curl_handle, &asyncReq, asyncMode == STRING_DOWNLOAD ? &buffer : NULL, true, &lastAsync);
	if (asyncMode == STRING_DOWNLOAD)
	{
		if (res != CURLE_OK || buffer.ptr == NULL)
		{
			free(buffer.ptr);
			asyncStrRes = NULL;
			asyncResSize = 0;
		}
		else
		{
			asyncStrRes = buffer.ptr;
			asyncResSize = buffer.length;
		}
	}
	freeRequest(&asyncReq);
	asyncMode = DOWNLOAD_END;
	async_task_num--;
	asyncResult = 1;
	sceKernelExitDeleteThread(0);
	return 0;
}
static int lua_initFTP(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	char vita_ip[16];
	unsigned short int vita_port = 0;
	if (ftpvita_init(vita_ip, &vita_port) < 0) return luaL_error(L, "cannot start FTP server (WiFi off?)");
	ftpvita_add_device("app0:");
	ftpvita_add_device("ux0:");
	ftpvita_add_device("ur0:");
	ftpvita_add_device("music0:");
	ftpvita_add_device("photo0:");
	lua_pushstring(L, vita_ip);
	lua_pushinteger(L, vita_port);
	return 2;
}

static int lua_termFTP(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	ftpvita_fini();
	return 0;
}

static int lua_getip(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	SceNetCtlInfo info;
	if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) strcpy(vita_ip, "127.0.0.1");
	else strcpy(vita_ip, info.ip_address);
	lua_pushstring(L, vita_ip);
	return 1;
}

static int lua_getmac(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	SceNetEtherAddr mac;
	char macAddress[32];
	sceNetGetMacAddress(&mac, 0);	
	sprintf(macAddress, "%02X:%02X:%02X:%02X:%02X:%02X", mac.data[0], mac.data[1], mac.data[2], mac.data[3], mac.data[4], mac.data[5]);
	lua_pushstring(L, macAddress);
	return 1;
}

static int lua_init(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (isNet) return luaL_error(L, "Network is already inited");
	#endif
	int ret = sceNetShowNetstat();
	SceNetInitParam initparam;
	if (ret == SCE_NET_ERROR_ENOTINIT) {
		net_memory = malloc(NET_INIT_SIZE);
		initparam.memory = net_memory;
		initparam.size = NET_INIT_SIZE;
		initparam.flags = 0;
		ret = sceNetInit(&initparam);
		if (ret < 0) return luaL_error(L, "an error occurred while starting network.");
	}
	sceNetCtlInit();
	sceHttpInit(1*1024*1024);
	share_init();
	curl_handle = curl_easy_init();
	sync_handle = curl_easy_init();
	isNet = 1;
	return 0;
}

static int lua_wifi(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	int state;
	sceNetCtlInetGetState(&state);
	lua_pushboolean(L, state);
	return 1;
}

static int lua_wifilv(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	int state;
	SceNetCtlInfo info;
	sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info);
	lua_pushinteger(L, info.rssi_percentage);
	return 1;
}

static int lua_term(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	curl_easy_cleanup(curl_handle);
	curl_easy_cleanup(sync_handle);
	curl_handle = NULL;
	sync_handle = NULL;
	share_term();
	sceHttpTerm();
	sceNetCtlTerm();
	sceNetTerm();
	if (net_memory != NULL) free(net_memory);
	net_memory = NULL;
	isNet = 0;
	return 0;
}

static int lua_createserver(lua_State *L) {
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 1 && argc != 2) return luaL_error(L, "Socket.createServerSocket(port) takes one argument.");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	int port = luaL_checkinteger(L, 1);
	int type = SCE_NET_IPPROTO_TCP;
	if (argc == 2) type = luaL_checkinteger(L, 2);
	
	Socket* my_socket = (Socket*) malloc(sizeof(Socket));
	my_socket->serverSocket = true;

	if (type == SCE_NET_IPPROTO_TCP) my_socket->sock = sceNetSocket("Server Socket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	else my_socket->sock = sceNetSocket("Server Socket", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->sock <= 0) return luaL_error(L, "invalid socket.");
	#endif

	int _true = 1;
	SceNetSockaddrIn addrTo;
	addrTo.sin_family = SCE_NET_AF_INET;
	addrTo.sin_port = sceNetHtons(port);
	if (type == SCE_NET_IPPROTO_TCP) addrTo.sin_addr.s_addr = 0;
	else{
		SceNetCtlInfo info;
		sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
		sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &addrTo.sin_port);
	}
	
	int err = sceNetBind(my_socket->sock, (SceNetSockaddr*)&addrTo, sizeof(addrTo));
	#ifndef SKIP_ERROR_HANDLING
	if (err != 0) return luaL_error(L, "bind error.");
	#endif

	sceNetSetsockopt(my_socket->sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &_true, sizeof(_true));
	
	if (type == SCE_NET_IPPROTO_TCP){
		err = sceNetListen(my_socket->sock, 1);
		#ifndef SKIP_ERROR_HANDLING
		if (err != 0) return luaL_error(L, "listen error.");
		#endif
	}
	
	my_socket->magic = 0xDEADDEAD;
	lua_pushinteger(L,(uint32_t)my_socket);
	return 1;
}

static int lua_send(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING	
	if (argc != 2) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	
	Socket* my_socket = (Socket*)luaL_checkinteger(L, 1);
	size_t size;
	char* text = (char*)luaL_checklstring(L, 2, &size);

	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->magic != 0xDEADDEAD) return luaL_error(L, "attempt to access wrong memory block type");
	if (my_socket->serverSocket) return luaL_error(L, "send not allowed for server sockets.");
	if (!text) return luaL_error(L, "Socket.send() expected a string.");
	#endif
	
	int result = sceNetSend(my_socket->sock, text, size, 0);
	lua_pushinteger(L, result);
	return 1;
}

static int lua_recv(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 2) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	
	Socket* my_socket = (Socket*)luaL_checkinteger(L, 1);
	uint32_t size = luaL_checkinteger(L, 2);
	
	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->magic != 0xDEADDEAD) return luaL_error(L, "attempt to access wrong memory block type");			
	if (my_socket->serverSocket) return luaL_error(L, "recv not allowed for server sockets.");
	#endif

	char* data = (char*)malloc(size);
	int count = sceNetRecv(my_socket->sock, data, size, 0);
	if (count > 0) lua_pushlstring(L, data, count);
	else lua_pushstring(L, "");
	free(data);
	return 1;
}

static int lua_accept(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 1) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	
	Socket* my_socket = (Socket*)luaL_checkinteger(L, 1);
	
	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->magic != 0xDEADDEAD) return luaL_error(L, "attempt to access wrong memory block type");
	if (!my_socket->serverSocket) return luaL_error(L, "accept allowed for server sockets only.");
	#endif

	SceNetSockaddrIn addrAccept;
	unsigned int cbAddrAccept = sizeof(addrAccept);
	int sockClient = sceNetAccept(my_socket->sock, (SceNetSockaddr*)&addrAccept, &cbAddrAccept);
	if (sockClient <= 0) return 0;

	Socket* incomingSocket = (Socket*) malloc(sizeof(Socket));
	incomingSocket->serverSocket = 0;
	incomingSocket->sock = sockClient;
	incomingSocket->magic = 0xDEADDEAD;
	int rcvbuf = 32768;
	int _true = 1;
	sceNetSetsockopt(my_socket->sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &_true, sizeof(_true));
	lua_pushinteger(L, (uint32_t)incomingSocket);
	return 1;
}

static int lua_closeSock(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 1) return luaL_error(L, "Socket.close() takes one argument.");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	
	Socket* my_socket = (Socket*)luaL_checkinteger(L, 1);
	
	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->magic != 0xDEADDEAD) return luaL_error(L, "attempt to access wrong memory block type");
	#endif
	
	sceNetSocketClose(my_socket->sock);
	free(my_socket);
	return 0;
}

static int lua_connect(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 2 && argc != 3)  return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	
	// Getting arguments
	char *host = (char*)luaL_checkstring(L, 1);
	int port = luaL_checkinteger(L, 2);
	int type = SCE_NET_IPPROTO_TCP;
	if (argc == 3) type = luaL_checkinteger(L, 3);
	char port_str[8];
	sprintf(port_str,"%i",port);
	
	// Allocating Socket memblock
	Socket* my_socket = (Socket*) malloc(sizeof(Socket));
	my_socket->serverSocket = false;
	my_socket->magic = 0xDEADDEAD;
	
	// Creating socket
	if (type == SCE_NET_IPPROTO_TCP) my_socket->sock = sceNetSocket("Socket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	else my_socket->sock = sceNetSocket("Socket", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
	#ifndef SKIP_ERROR_HANDLING
	if (my_socket->sock < 0){
		free(my_socket);
		return luaL_error(L, "Failed creating socket.");
	}
	#endif
	
	// Resolving host
	hostent* hostentry = gethostbyname(host);
	SceNetSockaddrIn addrTo;
	addrTo.sin_family = SCE_NET_AF_INET;
	addrTo.sin_port = sceNetHtons(port);
	addrTo.sin_addr.s_addr = *(int *)hostentry->h_addr_list[0];
	
	// Connecting to the server if TCP socket
	if (type == SCE_NET_IPPROTO_TCP){
		int res = sceNetConnect(my_socket->sock, (SceNetSockaddr*)&addrTo, sizeof(SceNetSockaddrIn));
		#ifndef SKIP_ERROR_HANDLING
		if(res < 0){
			sceNetSocketClose(my_socket->sock);
			free(my_socket);
			return luaL_error(L, "Failed connecting to the server.");
		}
		#endif
	}
	
	// Setting socket options
	int _true = 1;
	sceNetSetsockopt(my_socket->sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &_true, sizeof(_true));	
	
	lua_pushinteger(L, (uint32_t)my_socket);
	return 1;
}

static int lua_download(lua_State *L){
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc < 2 || argc > 13) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	NetRequest r;
	readRequest(L, &r, true, argc);
	performRequest(sync_handle, &r, NULL, false, &lastSync);
	freeRequest(&r);
	return 0;
}

static int startAsync(lua_State *L, bool file)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc < 1 || argc > 13) return luaL_error(L, "wrong number of arguments");
	if (async_task_num == ASYNC_TASKS_MAX) return luaL_error(L, "cannot start more async tasks.");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	freeRequest(&asyncReq);
	readRequest(L, &asyncReq, file, argc);
	async_task_num++;
	asyncMode = file ? FILE_DOWNLOAD : STRING_DOWNLOAD;
	total_bytes = 0xFFFFFFFF;
	downloaded_bytes = 0;
	stopAsyncDownload = false;
	SceUID thd = sceKernelCreateThread("Net Downloader Thread", &downloadThread, 0x10000100, 0x100000, 0, 0, NULL);
	if (thd < 0)
	{
		freeRequest(&asyncReq);
		async_task_num--;
		asyncMode = DOWNLOAD_END;
		asyncResult = -1;
		return 0;
	}
	asyncResult = 0;
	sceKernelStartThread(thd, 0, NULL);
	return 0;
}

static int lua_downloadasync(lua_State *L){
	return startAsync(L, true);
}

static int lua_string(lua_State *L){
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc < 1 || argc > 12) return luaL_error(L, "wrong number of arguments");
	if (!isNet) return luaL_error(L, "Network is not inited");
	#endif
	NetRequest r;
	readRequest(L, &r, false, argc);
	NetBuffer buffer = {NULL, 0, 0};
	performRequest(sync_handle, &r, &buffer, false, &lastSync);
	freeRequest(&r);
	if (buffer.ptr) lua_pushlstring(L, (const char*)buffer.ptr, buffer.length);
	else lua_pushstring(L, "");
	free(buffer.ptr);
	return 1;
}

static int lua_stringasync(lua_State *L){
	return startAsync(L, false);
}

// Network.getLastResponse([sync]): what the last finished async request (or the last sync one) got
// -> {status = 200, code = 0, url = final URL, contentType = "...", error = "...", seconds = 0.4, bytes = 1234, compressed = true}
static int lua_lastresponse(lua_State *L)
{
	NetResponse *r = lua_toboolean(L, 1) ? &lastSync : &lastAsync;
	lua_newtable(L);
	lua_pushinteger(L, r->status); lua_setfield(L, -2, "status");
	lua_pushinteger(L, r->code); lua_setfield(L, -2, "code");
	lua_pushstring(L, r->url); lua_setfield(L, -2, "url");
	lua_pushstring(L, r->contentType); lua_setfield(L, -2, "contentType");
	lua_pushstring(L, r->error); lua_setfield(L, -2, "error");
	lua_pushnumber(L, r->seconds); lua_setfield(L, -2, "seconds");
	lua_pushnumber(L, (double)r->bytes); lua_setfield(L, -2, "bytes");
	lua_pushboolean(L, r->decoded); lua_setfield(L, -2, "compressed");
	return 1;
}

// Network.clearCookies(): forgets every cookie the sites set during this session
static int lua_clearcookies(lua_State *L)
{
	if (isNet && sync_handle)
	{
		curl_easy_setopt(sync_handle, CURLOPT_SHARE, curl_share);
		curl_easy_setopt(sync_handle, CURLOPT_COOKIEFILE, "");
		curl_easy_setopt(sync_handle, CURLOPT_COOKIELIST, "ALL");
	}
	return 0;
}

static int lua_getbytes1(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	#endif
	lua_pushnumber(L, downloaded_bytes);
	return 1;
}

static int lua_getbytes2(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	#endif
	lua_pushnumber(L, total_bytes);
	return 1;
}

static int lua_setcontime(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 1) return luaL_error(L, "wrong number of arguments");
	#endif
	asyncConTime = luaL_checkint(L,1);
	return 0;
}
static int lua_getcontime(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	#endif
	lua_pushnumber(L, asyncConTime);
	return 1;
}

static int lua_stopdownload(lua_State *L)
{
	int argc = lua_gettop(L);
	#ifndef SKIP_ERROR_HANDLING
	if (argc != 0) return luaL_error(L, "wrong number of arguments");
	#endif
	stopAsyncDownload = true;
	return 0;
}

//Register our Network Functions
static const luaL_Reg Network_functions[] = {
  {"init",                lua_init},
  {"term",                lua_term},
  {"initFTP",             lua_initFTP},
  {"termFTP",             lua_termFTP},
  {"getIPAddress",        lua_getip},
  {"getMacAddress",       lua_getmac},
  {"isWifiEnabled",       lua_wifi},
  {"getWifiLevel",        lua_wifilv},
  {"downloadFile",        lua_download},
  {"downloadFileAsync",   lua_downloadasync},
  {"requestString",       lua_string},
  {"requestStringAsync",  lua_stringasync},
  {"getDownloadedBytes",  lua_getbytes1},
  {"getTotalBytes",		  lua_getbytes2},
  {"setConnectionTime",   lua_setcontime},
  {"getConnectionTime",   lua_getcontime},
  {"stopCurrentDownload", lua_stopdownload},
  {"getLastResponse",     lua_lastresponse},
  {"clearCookies",        lua_clearcookies},
  {0, 0}
};

//Register our Socket Functions
static const luaL_Reg Socket_functions[] = {
  {"createServerSocket", lua_createserver},
  {"send",               lua_send},
  {"receive",            lua_recv},
  {"accept",             lua_accept},
  {"close",              lua_closeSock},
  {"connect",            lua_connect},
  {0, 0}
};

void luaNetwork_init(lua_State *L) {
	uint8_t GET_METHOD = SCE_HTTP_METHOD_GET;
	uint8_t POST_METHOD = SCE_HTTP_METHOD_POST;
	uint8_t HEAD_METHOD = SCE_HTTP_METHOD_HEAD;
	uint8_t OPTIONS_METHOD = SCE_HTTP_METHOD_OPTIONS;
	uint8_t PUT_METHOD = SCE_HTTP_METHOD_PUT;
	uint8_t DELETE_METHOD = SCE_HTTP_METHOD_DELETE;
	uint8_t TRACE_METHOD = SCE_HTTP_METHOD_TRACE;
	uint8_t CONNECT_METHOD = SCE_HTTP_METHOD_CONNECT;
	uint8_t UDP_SOCKET = SCE_NET_IPPROTO_UDP;
	uint8_t TCP_SOCKET = SCE_NET_IPPROTO_TCP;
	uint8_t JSON = 0;
	uint8_t XWWW = 1;
	if (isNet)
	{
		curl_easy_cleanup(curl_handle);
		curl_easy_cleanup(sync_handle);
		curl_handle = NULL;
		sync_handle = NULL;
		share_term();
		sceHttpTerm();
		sceNetCtlTerm();
		sceNetTerm();
		if (net_memory != NULL) 
			free(net_memory);
		net_memory = NULL;
		isNet = 0;
	}
	VariableRegister(L,UDP_SOCKET);
	VariableRegister(L,TCP_SOCKET);
	VariableRegister(L,GET_METHOD);
	VariableRegister(L,POST_METHOD);
	VariableRegister(L,HEAD_METHOD);
	VariableRegister(L,OPTIONS_METHOD);
	VariableRegister(L,PUT_METHOD);
	VariableRegister(L,DELETE_METHOD);
	VariableRegister(L,TRACE_METHOD);
	VariableRegister(L,CONNECT_METHOD);
	VariableRegister(L,JSON);
	VariableRegister(L,XWWW);
	lua_newtable(L);
	luaL_setfuncs(L, Network_functions, 0);
	lua_setglobal(L, "Network");
	// OTAKIO_ENGINE: what this engine can do, for scripts that must also run on older engines
	lua_newtable(L);
	lua_pushinteger(L, OTAKIO_ENGINE_VERSION); lua_setfield(L, -2, "version");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "gzip");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "cookies");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "response");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "plainArgs");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "gif");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "avif");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "imageLane");
	lua_pushboolean(L, 1); lua_setfield(L, -2, "filters");
	lua_setglobal(L, "OTAKIO_ENGINE");
	lua_newtable(L);
	luaL_setfuncs(L, Socket_functions, 0);
	lua_setglobal(L, "Socket");
}