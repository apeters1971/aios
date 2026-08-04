// SPDX-License-Identifier: GPL-2.0
#include "internal.h"

#include <linux/in.h>
#include <linux/inet.h>
#include <linux/mm.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <net/sock.h>

#ifdef CONFIG_DNS_RESOLVER
#include <linux/dns_resolver.h>
#endif

static int resolve_ipv4(const char *host, __be32 *addr)
{
	if (in4_pton(host, -1, (u8 *)addr, -1, NULL) == 1)
		return 0;

#ifdef CONFIG_DNS_RESOLVER
	{
		char *ip = NULL;
		int len;

		len = dns_query(NULL, host, strlen(host), NULL, &ip, NULL);
		if (len > 0 && ip) {
			int ok = in4_pton(ip, len, (u8 *)addr, -1, NULL);
			kfree(ip);
			if (ok == 1)
				return 0;
		}
	}
#endif
	return -EHOSTUNREACH;
}

static int sock_send_all(struct socket *sock, const void *buf, size_t len)
{
	struct kvec iov;
	struct msghdr msg;
	size_t sent = 0;

	while (sent < len) {
		int n;

		iov.iov_base = (void *)buf + sent;
		iov.iov_len = len - sent;
		memset(&msg, 0, sizeof(msg));
		n = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		if (n <= 0)
			return n ? n : -EIO;
		sent += n;
	}
	return 0;
}

static int sock_recv_some(struct socket *sock, void *buf, size_t len)
{
	struct kvec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg;

	memset(&msg, 0, sizeof(msg));
	return kernel_recvmsg(sock, &msg, &iov, 1, len, 0);
}

static int sock_recv_until(struct socket *sock, char *buf, size_t cap, const char *needle,
			   size_t *have)
{
	size_t nlen = strlen(needle);

	*have = 0;
	while (*have + 1 < cap) {
		char *p;
		int n;

		n = sock_recv_some(sock, buf + *have, cap - *have - 1);
		if (n <= 0)
			return n ? n : -EIO;
		*have += n;
		buf[*have] = '\0';
		p = strnstr(buf, needle, *have);
		if (p)
			return 0;
		if (*have >= cap - 1)
			return -EMSGSIZE;
	}
	return -EMSGSIZE;
}

int aios_http_header_get(const char *hdrs, const char *name, char *out, size_t out_len)
{
	const char *p = hdrs;
	size_t nlen = strlen(name);

	if (!hdrs || !name || !out || !out_len)
		return -EINVAL;
	out[0] = '\0';

	while (*p) {
		const char *eol = strstr(p, "\r\n");
		size_t line_len;
		const char *colon;

		if (!eol)
			eol = p + strlen(p);
		line_len = eol - p;
		if (line_len == 0)
			break;
		colon = memchr(p, ':', line_len);
		if (colon && (size_t)(colon - p) == nlen && !strncasecmp(p, name, nlen)) {
			const char *v = colon + 1;
			size_t vlen;

			while (v < eol && (*v == ' ' || *v == '\t'))
				v++;
			vlen = eol - v;
			if (vlen >= out_len)
				vlen = out_len - 1;
			memcpy(out, v, vlen);
			out[vlen] = '\0';
			return 0;
		}
		if (*eol == '\0')
			break;
		p = eol + 2;
	}
	return -ENOENT;
}

u64 aios_http_attr_u64(const char *hdrs, const char *attr_name)
{
	char key[128];
	char val[64];
	u64 v = 0;

	snprintf(key, sizeof(key), "x-aios-attr-%s", attr_name);
	if (aios_http_header_get(hdrs, key, val, sizeof(val)))
		return 0;
	if (kstrtou64(val, 10, &v))
		return 0;
	return v;
}

static int parse_status_line(const char *hdrs, int *status_out)
{
	/* HTTP/1.1 200 OK */
	const char *p = hdrs;
	unsigned int code = 0;

	while (*p && *p != ' ')
		p++;
	if (!*p)
		return -EIO;
	p++;
	if (kstrtouint(p, 10, &code))
		return -EIO;
	*status_out = (int)code;
	return 0;
}

int aios_http_parse_location(const char *loc, char *host, size_t host_len, char *port,
			     size_t port_len, char *path, size_t path_len)
{
	const char *p = loc;
	const char *slash;
	const char *colon;

	if (!loc || !*loc)
		return -EINVAL;
	if (loc[0] == '/') {
		strscpy(path, loc, path_len);
		return 1; /* path-only */
	}
	if (!strncmp(loc, "http://", 7))
		p = loc + 7;
	else if (!strncmp(loc, "https://", 8))
		return -EOPNOTSUPP;
	else
		return -EINVAL;

	slash = strchr(p, '/');
	if (!slash) {
		strscpy(host, p, host_len);
		strscpy(path, "/", path_len);
	} else {
		size_t hlen = slash - p;

		if (hlen >= host_len)
			return -EINVAL;
		memcpy(host, p, hlen);
		host[hlen] = '\0';
		strscpy(path, slash, path_len);
	}
	colon = strrchr(host, ':');
	if (colon) {
		strscpy(port, colon + 1, port_len);
		*colon = '\0';
	} else {
		strscpy(port, "80", port_len);
	}
	return 0;
}

int aios_http_tcp_request(struct aios_http_client *c, const char *method, const char *path,
			  const char *extra_hdrs, const void *body, size_t body_len,
			  int *status_out, char *location_out, size_t location_len,
			  struct aios_http_buf *resp_body, char *resp_hdrs,
			  size_t resp_hdrs_len)
{
	struct socket *sock = NULL;
	struct sockaddr_in sin = { 0 };
	char *req = NULL;
	char *hdrbuf = NULL;
	char auth[512];
	size_t have = 0;
	unsigned long content_length = 0;
	char clen[32];
	char *body_start;
	size_t header_bytes;
	size_t already;
	__be32 addr;
	u16 port_n;
	int err;
	int n;

	if (location_out && location_len)
		location_out[0] = '\0';
	if (resp_body) {
		resp_body->data = NULL;
		resp_body->len = 0;
	}
	if (resp_hdrs && resp_hdrs_len)
		resp_hdrs[0] = '\0';

	if (body_len > AIOS_HTTP_MAX_BODY)
		return -EFBIG;

	err = resolve_ipv4(c->host, &addr);
	if (err)
		return err;
	if (kstrtou16(c->port, 10, &port_n))
		return -EINVAL;

	err = aios_http_build_auth(c, method, path, auth, sizeof(auth));
	if (err)
		return err;

	req = kmalloc(AIOS_HTTP_MAX_HDR + 512, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	{
		char app_line[96] = "";

		if (c->app_label[0])
			snprintf(app_line, sizeof(app_line), "x-aios-app-label: %s\r\n",
				 c->app_label);
		n = snprintf(req, AIOS_HTTP_MAX_HDR + 512,
			     "%s %s HTTP/1.1\r\n"
			     "Host: %s\r\n"
			     "Connection: close\r\n"
			     "Content-Length: %zu\r\n"
			     "%s%s%s"
			     "\r\n",
			     method, path, c->endpoint, body_len, auth, app_line,
			     extra_hdrs ? extra_hdrs : "");
	}
	if (n < 0 || n >= AIOS_HTTP_MAX_HDR + 512) {
		err = -EOVERFLOW;
		goto out;
	}

	err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err)
		goto out;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port_n);
	sin.sin_addr.s_addr = addr;
	err = kernel_connect(sock, (struct sockaddr *)&sin, sizeof(sin), 0);
	if (err)
		goto out_sock;

	err = sock_send_all(sock, req, n);
	if (err)
		goto out_sock;
	if (body_len && body) {
		err = sock_send_all(sock, body, body_len);
		if (err)
			goto out_sock;
	}

	hdrbuf = kmalloc(AIOS_HTTP_MAX_HDR, GFP_KERNEL);
	if (!hdrbuf) {
		err = -ENOMEM;
		goto out_sock;
	}
	err = sock_recv_until(sock, hdrbuf, AIOS_HTTP_MAX_HDR, "\r\n\r\n", &have);
	if (err)
		goto out_sock;

	body_start = strnstr(hdrbuf, "\r\n\r\n", have);
	if (!body_start) {
		err = -EIO;
		goto out_sock;
	}
	header_bytes = body_start - hdrbuf + 4;
	if (resp_hdrs && resp_hdrs_len) {
		size_t copy = min(header_bytes, resp_hdrs_len - 1);

		memcpy(resp_hdrs, hdrbuf, copy);
		resp_hdrs[copy] = '\0';
	}

	err = parse_status_line(hdrbuf, status_out);
	if (err)
		goto out_sock;

	if (!aios_http_header_get(hdrbuf, "Content-Length", clen, sizeof(clen))) {
		if (kstrtoul(clen, 10, &content_length))
			content_length = 0;
	}
	if (location_out && location_len)
		aios_http_header_get(hdrbuf, "Location", location_out, location_len);

	already = have - header_bytes;
	if (resp_body && content_length > 0) {
		if (content_length > AIOS_HTTP_MAX_BODY) {
			err = -EFBIG;
			goto out_sock;
		}
		resp_body->data = kvmalloc(content_length, GFP_KERNEL);
		if (!resp_body->data) {
			err = -ENOMEM;
			goto out_sock;
		}
		resp_body->len = content_length;
		if (already) {
			size_t take = min(already, content_length);

			memcpy(resp_body->data, body_start + 4, take);
			already = take;
		} else {
			already = 0;
		}
		while (already < content_length) {
			int got = sock_recv_some(sock, (char *)resp_body->data + already,
						 content_length - already);
			if (got <= 0) {
				err = got ? got : -EIO;
				kvfree(resp_body->data);
				resp_body->data = NULL;
				resp_body->len = 0;
				goto out_sock;
			}
			already += got;
		}
	} else if (resp_body && already > 0 && content_length == 0) {
		/* No Content-Length: keep what we already have after headers (rare). */
		resp_body->data = kvmalloc(already, GFP_KERNEL);
		if (!resp_body->data) {
			err = -ENOMEM;
			goto out_sock;
		}
		memcpy(resp_body->data, body_start + 4, already);
		resp_body->len = already;
	}

	err = 0;

out_sock:
	if (sock)
		sock_release(sock);
out:
	kfree(req);
	kfree(hdrbuf);
	return err;
}
