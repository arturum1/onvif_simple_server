/*
 * onvif_gateway - minimal HTTP <-> CGI bridge for onvif_simple_server.
 *
 * onvif_simple_server is modelled as a CGI application: one process per SOAP
 * request, the body on stdin, the "service" chosen from the last argv entry,
 * and the HTTP headers + body written to stdout.  It was built to sit behind
 * a small web server (e.g. lighttpd + mod_cgi).  This is a self-contained,
 * dependency-free alternative for static/embedded deployments where no HTTP
 * server exists (many IP cameras carry neither lighttpd nor a usable httpd
 * applet in busybox).
 *
 * The CGI environment handed to onvif_simple_server mirrors exactly what the
 * divinus firmware passes (divinus/src/server.c onvif_cgi_dispatch):
 *
 *   GATEWAY_INTERFACE, SERVER_PROTOCOL, SERVER_NAME, SERVER_PORT,
 *   CONTENT_LENGTH, REQUEST_URI, SCRIPT_NAME, SCRIPT_FILENAME, PATH_INFO,
 *   QUERY_STRING, CONTENT_TYPE, REQUEST_METHOD, REMOTE_ADDR, REMOTE_HOST,
 *   REMOTE_PORT
 *
 * argv:   <binary> -c <conffile> <service>
 * cwd:    the payload directory that contains the *_service_files templates.
 *
 * The child's stdout is translated back into a full HTTP response: a raw
 * "HTTP/1.x <code> ..." or "Status: <code> ..." first line becomes the status
 * line, remaining header lines are passed through verbatim, then CRLFCRLF and
 * the body.
 *
 * Build (static, no extra libs):
 *   cc -Os -o onvif_gateway onvif_gateway.c
 *
 * Usage:
 *   onvif_gateway [-p PORT] [-b BINARY] [-c CONFFILE] [-d CHDIR] [-t TIMEOUT]
 *
 *   -p  listen port                  (default 8080)
 *   -b  path of onvif_simple_server  (default ./onvif_simple_server)
 *   -c  config passed on to the CGI (default /usr/local/etc/onvif_simple_server.conf)
 *   -d  chdir before exec            (default current directory)
 *   -t  child timeout in seconds     (default 10)
 *
 * Extra route:  GET /ptz?act=<name>&speed=<1..63>&number=<n>
 * Bridges the stock Xiongmai web UI's PTZ panel to ptzctl (the same binary
 * the ONVIF PTZ hooks call).  "act" values: up/down/left/right, zoomin,
 * zoomout, focusin (near), focusout (far), stop, home, preset-set,
 * preset-goto, preset-del, hscan/vscan (no-op).  Speed is mapped to the
 * ptzctl 0..1 scale as speed/63; preset numbers are shifted +1 (stock UI is
 * 0-based, ptzctl is 1-based).  Response body is discarded into the hidden
 * frame, so we always answer 200.  ptzctl is located via $PTZCTL_BIN
 * (default /mnt/mtd/ipc/onvifd/bin/ptzctl); PELCO_DEBUG=1 is set in the
 * child when $PTZ_DEBUG is set for the gateway.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PORT    8080
#define DEFAULT_BINARY  "onvif_simple_server"
#define DEFAULT_CONF    "/usr/local/etc/onvif_simple_server.conf"
#define DEFAULT_TIMEOUT 10

#define MAX_HDR  32768
#define MAX_BODY 262144
#define MAX_RESP 1048576

static const char *service_names[] = {
    "device_service", "media_service", "media2_service", "ptz_service",
    "imaging_service", "events_service", "deviceio_service", NULL
};

static int is_valid_service(const char *s)
{
    int i;
    for (i = 0; service_names[i]; i++)
        if (strcmp(s, service_names[i]) == 0)
            return 1;
    return 0;
}

static void http_error(int fd, int code)
{
    const char *reason = (code == 404) ? "Not Found"
                       : (code == 405) ? "Method Not Allowed"
                       : (code == 413) ? "Payload Too Large"
                       : "Internal Server Error";
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
                     code, reason, strlen(reason) + 1, reason);
    if (n > 0)
        write(fd, buf, (size_t)n);
    shutdown(fd, SHUT_RDWR);
}

static char resp_buf[MAX_RESP];

/* --------------------------------------------------------- /ptz bridge -- */

/* The /ptz route is served from a different origin than the stock web UI
 * (port 80 vs 8080), so the panel's XHR for act=features needs this header to
 * be readable at all.  Harmless here: every /ptz act is a GET, and the ones
 * that change hardware only reach commands the device itself advertises. */
#define CORS "Access-Control-Allow-Origin: *\r\n"

static void reply_ok(int fd)
{
    static const char okhead[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" CORS
        "Content-Length: 2\r\nConnection: close\r\n\r\n"
        "ok\n";
    write(fd, okhead, sizeof(okhead) - 1);
    shutdown(fd, SHUT_RDWR);
}

/* Reply carrying a text/plain payload (act=features). */
static void reply_text(int fd, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" CORS
                     "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", strlen(body));
    if (n <= 0)
        return;
    write(fd, head, (size_t)n);
    write(fd, body, strlen(body));
    shutdown(fd, SHUT_RDWR);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    unsigned char *dst = (unsigned char *)s, *src = dst;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hexval(src[1]), lo = hexval(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (unsigned char)(hi * 16 + lo);
                src += 3;
                continue;
            }
        }
        *dst++ = (unsigned char)((*src == '+') ? ' ' : *src);
        src++;
    }
    *dst = '\0';
}

/* Run ptzctl in a child without a shell; the exit status may be ignored.
 * The child's stdout/stderr go to /dev/null so it can never dirty the
 * HTTP response stream. */
static int ptz_exec(char *const argv[], int debug)
{
    pid_t pid;
    int st;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const char *tfile = getenv("PTZ_TRACE");
        int nfd = open("/dev/null", O_WRONLY);
        if (nfd >= 0) {
            dup2(nfd, 1);
            dup2(nfd, 2);
            if (nfd > 2)
                close(nfd);
        }
        /* NOTE: children inherit our environ (musl execv ignores its envp
         * argument), so per-child overrides must go through setenv().
         * PELCO_DEV / RS485_DIR_DEV are inherited as-is and ptzctl's own
         * defaults (/dev/ttyAMA1 + /dev/rs485) are correct for this camera. */
        if (debug)
            setenv("PELCO_DEBUG", "1", 1);
        else
            unsetenv("PELCO_DEBUG");
        if (tfile && tfile[0])
            setenv("PELCO_TRACE_FILE", tfile, 1);
        execv(argv[0], argv);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *web_to_move(const char *act)
{
    if (!strcmp(act, "up"))     return "up";
    if (!strcmp(act, "down"))   return "down";
    if (!strcmp(act, "left"))   return "left";
    if (!strcmp(act, "right"))  return "right";
    if (!strcmp(act, "zoomin")) return "zoom-in";
    if (!strcmp(act, "zoomout")) return "zoom-out";
    return NULL;
}

/* ------------------------------------------------- conf-driven features -- */

/* onvif_simple_server.conf is the feature registry: aux_command/aux_exec
 * pairs and ir_cut_filter_set are what the device advertises over ONVIF and
 * what it is willing to execute.  Rather than hardcoding a command list in
 * this bridge, /ptz speaks ONVIF to the CGI we already fork, so the stock web
 * UI panel stays in sync with the conf automatically - and any other project
 * reusing this gateway can change its whole feature set in one file. */

struct gw_cfg {
    const char *binary;
    const char *conf;
    const char *chdir_dir;
    int timeout_s;
    char client_ip[80];
    uint16_t client_port;
};
static struct gw_cfg gw;

/* run_soap() is defined below run_ptz(); declared here for the self-call. */
static int run_soap(int fd, const char *method, const char *uri,
                    const char *client_ip, uint16_t client_port,
                    const char *service, const char *content_type,
                    const char *query, const char *body, size_t body_len,
                    int timeout_s, const char *binary, const char *conf,
                    const char *chdir_dir, uint16_t local_port);

#define SOAP_ENV_OPEN "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body>"
#define SOAP_ENV_CLOSE "</soap:Body></soap:Envelope>"

/* Run one SOAP method through the CGI and copy the response body (whatever
 * follows the HTTP header block run_soap() produced) into out.
 * Returns 0 on success. */
static int onvif_call(const char *service, const char *inner,
                      char *out, size_t outcap)
{
    static char req[4096];
    int n, devnull;
    const char *sep, *b;
    size_t blen;

    n = snprintf(req, sizeof(req), SOAP_ENV_OPEN "%s" SOAP_ENV_CLOSE, inner);
    if (n < 0 || (size_t)n >= sizeof(req))
        return -1;

    /* run_soap() writes the full response to fd, but the body it builds for
     * us stays in resp_buf, so the fd only has to be somewhere harmless. */
    devnull = open("/dev/null", O_WRONLY);
    if (run_soap(devnull < 0 ? -1 : devnull, "POST", "/onvif", gw.client_ip,
                 gw.client_port, service, "application/soap+xml", "",
                 req, (size_t)n, gw.timeout_s, gw.binary, gw.conf,
                 gw.chdir_dir, 0) != 0) {
        if (devnull >= 0)
            close(devnull);
        return -1;
    }
    if (devnull >= 0)
        close(devnull);

    sep = strstr(resp_buf, "\r\n\r\n");
    b = sep ? sep + 4 : resp_buf;
    blen = strlen(b);
    while (blen && (b[blen - 1] == '\n' || b[blen - 1] == '\r'))
        blen--;
    if (outcap == 0)
        return -1;
    if (blen >= outcap)
        blen = outcap - 1;
    memcpy(out, b, blen);
    out[blen] = '\0';
    return 0;
}

#define MAX_AUX_ENTRIES 32
struct aux_entry {
    char cmd[256];        /* the exact value the device advertises */
    char name[64];        /* label for the UI */
    char action[24];      /* "On"/"Off"/... - a stateful vs a tap button */
};
static struct aux_entry g_aux[MAX_AUX_ENTRIES];
static int g_aux_num;
static int g_aux_loaded;  /* conf re-read per request, so cache per process */

/* Mirror of ptz_service.c's is_safe_aux_command(): an advertised value is
 * forwarded into a SOAP body verbatim, so refuse anything that is not
 * already safe to execute. */
static int aux_value_ok(const char *s)
{
    if (!s || !*s || strlen(s) > 255)
        return 0;
    return strspn(s, "abcdefghijklmnopqrstuvwxyz"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                     "_.-:|") == strlen(s);
}

/* Scrape the <tt:AuxiliaryCommands> values out of a GetNode response.  The
 * conf value doubles as the ONVIF match key and the UI label; upstream's
 * convention is "tt:<Feature>|<Action>" (see onvif_simple_server.conf.example),
 * so split on the last '|' for the label.  A value with no '|' is a plain
 * tap button. */
static int aux_list_load(void)
{
    static const char otag[] = "<tt:AuxiliaryCommands>";
    static const char ctag[] = "</tt:AuxiliaryCommands>";
    static char resp[16384];
    char *p = resp, *v, *end, *val, *bar;

    g_aux_num = 0;
    if (onvif_call("ptz_service",
                   "<GetNode xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                   "<NodeToken>PTZNodeToken</NodeToken></GetNode>",
                   resp, sizeof(resp)) != 0)
        return -1;

    while ((p = strstr(p, otag)) != NULL) {
        p += sizeof(otag) - 1;
        end = strstr(p, ctag);
        if (!end)
            break;
        *end = '\0';
        v = p;
        p = end + sizeof(ctag) - 1;   /* advance now: the checks below may skip */
        if (g_aux_num >= MAX_AUX_ENTRIES)
            break;
        if (!aux_value_ok(v))
            continue;

        /* Copy the advertised value out before splitting the label: the split
         * writes a NUL into the shared response buffer. */
        snprintf(g_aux[g_aux_num].cmd, sizeof(g_aux[g_aux_num].cmd), "%s", v);

        val = v;
        if (strncmp(val, "tt:", 3) == 0)
            val += 3;
        bar = strrchr(val, '|');
        if (bar) {
            *bar = '\0';
            snprintf(g_aux[g_aux_num].name, sizeof(g_aux[g_aux_num].name),
                     "%s", val);
            snprintf(g_aux[g_aux_num].action, sizeof(g_aux[g_aux_num].action),
                     "%s", bar + 1);
        } else {
            snprintf(g_aux[g_aux_num].name, sizeof(g_aux[g_aux_num].name),
                     "%s", val);
            snprintf(g_aux[g_aux_num].action, sizeof(g_aux[g_aux_num].action),
                     "Press");
        }
        g_aux_num++;
    }
    g_aux_loaded = 1;
    return g_aux_num;
}

/* act=features: a plain-text, newline-separated list the panel parses
 * without any eval.  Lines are "aux\t<action>\t<name>\t<value>" and
 * "ircut\tIrCutFilter".  The device is the only source of truth here. */
static void act_features(int fd)
{
    char body[4096];
    size_t off = 0;
    int i, n;

    if (!g_aux_loaded)
        aux_list_load();

    for (i = 0; i < g_aux_num; i++) {
        if (off + 1 >= sizeof(body))
            break;
        n = snprintf(body + off, sizeof(body) - off, "aux\t%s\t%s\t%s\n",
                     g_aux[i].action, g_aux[i].name, g_aux[i].cmd);
        if (n < 0 || (size_t)n >= sizeof(body) - off)
            break;
        off += (size_t)n;
    }

    /* The imaging service always renders IrCutFilter, so its presence in
     * GetImagingSettings is the signal that the feature is wired up.
     * imaging_service.c:check_video_source_token() rejects the call unless the
     * literal VideoSourceToken element is present. */
    if (off + 1 < sizeof(body)) {
        static char img[4096];
        if (onvif_call("imaging_service",
                       "<GetImagingSettings xmlns=\"http://www.onvif.org/ver20/imaging/wsdl\">"
                       "<VideoSourceToken>VideoSourceToken</VideoSourceToken>"
                       "</GetImagingSettings>",
                       img, sizeof(img)) == 0 &&
            strstr(img, "IrCutFilter")) {
            n = snprintf(body + off, sizeof(body) - off, "ircut\tIrCutFilter\n");
            if (n > 0 && (size_t)n < sizeof(body) - off)
                off += (size_t)n;
        }
    }

    if (off == 0)
        off = (size_t)snprintf(body, sizeof(body), "none\n");
    body[off] = '\0';
    reply_text(fd, body);
}

/* act=aux&cmd=<value>: only values the device just advertised are accepted,
 * so this cannot be used to reach a command the conf does not expose.
 * Compared case-insensitively because SendAuxiliaryCommand is. */
static void act_aux(int fd, const char *want)
{
    int i;

    if (!g_aux_loaded)
        aux_list_load();
    for (i = 0; i < g_aux_num; i++) {
        if (!strcasecmp(g_aux[i].cmd, want)) {
            char inner[512];
            static char resp[4096];
            snprintf(inner, sizeof(inner),
                     "<SendAuxiliaryCommand xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                     "<AuxiliaryData><AuxiliaryCommand>%s</AuxiliaryCommand>"
                     "</AuxiliaryData></SendAuxiliaryCommand>", want);
            onvif_call("ptz_service", inner, resp, sizeof(resp));
            break;
        }
    }
    reply_ok(fd);
}

/* act=ircut&v=on|off|auto -> SetImagingSettings.  Values are forwarded
 * uppercase, which is what imaging_service.c substitutes into
 * ir_cut_filter_set=%s. */
static void act_ircut(int fd, const char *v)
{
    char inner[256];
    static char resp[4096];

    if (!v[0])
        v = "auto";
    if (strcasecmp(v, "on") && strcasecmp(v, "off") && strcasecmp(v, "auto")) {
        reply_ok(fd);
        return;
    }
    snprintf(inner, sizeof(inner),
             "<SetImagingSettings xmlns=\"http://www.onvif.org/ver20/imaging/wsdl\">"
             "<VideoSourceToken>VideoSourceToken</VideoSourceToken>"
             "<ImagingSettings><IrCutFilter>%s</IrCutFilter></ImagingSettings>"
             "</SetImagingSettings>", v);
    onvif_call("imaging_service", inner, resp, sizeof(resp));
    reply_ok(fd);
}

static void run_ptz(int fd, const char *query)
{
    const char *bin = getenv("PTZCTL_BIN");
    static char act[24], spd[24], num[24];
    static char cmd[256], val2[24];
    char np[16];
    double sval;
    int n, debug = getenv("PTZ_DEBUG") != NULL;
    const char *p = query;

    if (!bin || !bin[0])
        bin = "/mnt/mtd/ipc/onvifd/bin/ptzctl";

    act[0] = spd[0] = num[0] = cmd[0] = val2[0] = '\0';
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t tlen = amp ? (size_t)(amp - p) : strlen(p);
        char tok[320];
        char *name, *val;
        char *eq;
        if (tlen == 0) { p = amp ? amp + 1 : NULL; continue; }
        if (tlen >= sizeof(tok))
            tlen = sizeof(tok) - 1;
        memcpy(tok, p, tlen);
        tok[tlen] = '\0';
        eq = strchr(tok, '=');
        if (!eq) { p = amp ? amp + 1 : NULL; continue; }
        *eq = '\0';
        name = tok;
        val = eq + 1;
        url_decode(val);
        if (!strcmp(name, "act") && *val)
            snprintf(act, sizeof(act), "%s", val);
        else if (!strcmp(name, "speed") && *val && strspn(val, "0123456789") == strlen(val))
            snprintf(spd, sizeof(spd), "%s", val);
        else if (!strcmp(name, "number") && *val && strspn(val, "0123456789") == strlen(val))
            snprintf(num, sizeof(num), "%s", val);
        else if (!strcmp(name, "cmd") && *val)
            snprintf(cmd, sizeof(cmd), "%s", val);
        else if (!strcmp(name, "v") && *val)
            snprintf(val2, sizeof(val2), "%s", val);
        p = amp ? amp + 1 : NULL;
    }

    if (!act[0]) { reply_ok(fd); return; }     /* empty/health request */

    fprintf(stderr, "ptz: act=%s spd=%s num=%s cmd=%s v=%s\n", act, spd, num,
            cmd, val2);

    if (!strcmp(act, "hscan") || !strcmp(act, "vscan")) { reply_ok(fd); return; }

    /* Conf-driven features (see the section above): discovery and execution
     * both go through the CGI, so the conf decides what exists. */
    if (!strcmp(act, "features")) { act_features(fd); return; }
    if (!strcmp(act, "aux"))       { act_aux(fd, cmd); return; }
    if (!strcmp(act, "ircut"))     { act_ircut(fd, val2); return; }

    if (!strcmp(act, "stop")) {
        char *a1[] = { (char *)bin, "move", "-m", "stop", NULL };
        char *a2[] = { (char *)bin, "focus", "-m", "stop", NULL };
        ptz_exec(a1, debug);
        ptz_exec(a2, debug);
        reply_ok(fd);
        return;
    }

    if (!strcmp(act, "home")) {
        char *a[] = { (char *)bin, "home", NULL };
        ptz_exec(a, debug);
        reply_ok(fd);
        return;
    }

    if (!strcmp(act, "preset-set") || !strcmp(act, "preset-goto") ||
        !strcmp(act, "preset-del")) {
        const char *pa = !strcmp(act, "preset-set") ? "add"
                       : !strcmp(act, "preset-goto") ? "goto" : "del";
        n = num[0] ? atoi(num) : 0;
        if (n < 0) n = 0;
        if (n > 98) n = 98;
        snprintf(np, sizeof(np), "%d", n + 1);
        {
            char *a[] = { (char *)bin, "preset", "-a", (char *)pa, "-n", np, NULL };
            ptz_exec(a, debug);
        }
        reply_ok(fd);
        return;
    }

    {
        const char *sub, *m = web_to_move(act);
        if (!m) {
            if (!strcmp(act, "focusin"))  { sub = "focus"; m = "near"; }
            else if (!strcmp(act, "focusout")) { sub = "focus"; m = "far"; }
            else { reply_ok(fd); return; }      /* unknown -> silent 200 */
        } else {
            sub = "move";
        }
        sval = 1.0;
        if (spd[0]) {
            int sp = atoi(spd);
            if (sp < 0) sp = 0;
            if (sp > 63) sp = 63;
            sval = sp / 63.0;
        }
        snprintf(spd, sizeof(spd), "%.3f", sval);
        {
            char *a[] = { (char *)bin, (char *)sub, "-m", (char *)m,
                          "-s", spd, NULL };
            ptz_exec(a, debug);
        }
    }
    reply_ok(fd);
}

static int run_soap(int fd, const char *method, const char *uri,
                    const char *client_ip, uint16_t client_port,
                    const char *service, const char *content_type,
                    const char *query, const char *body, size_t body_len,
                    int timeout_s, const char *binary, const char *conf,
                    const char *chdir_dir, uint16_t local_port)
{
    int inpipe[2], outpipe[2];
    pid_t pid;
    int resp_len = 0, timeout, pr, n;
    struct pollfd pfd;

    if (pipe(inpipe) || pipe(outpipe)) {
        http_error(fd, 500);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        http_error(fd, 500);
        return -1;
    }

    if (pid == 0) {
        char env_gi[] = "GATEWAY_INTERFACE=CGI/1.1";
        char env_sg[] = "SERVER_PROTOCOL=HTTP/1.1";
        char env_sn[96], env_sp[32], env_len[48], env_uri[196];
        char env_script[112], env_sf[300], env_pi[196], env_qs[196];
        char env_ct[128], env_rm[32], env_ra[80], env_rh[80], env_rp[32];
        char *envp[] = { env_gi, env_sg, env_sn, env_sp, env_len, env_uri,
                         env_script, env_sf, env_pi, env_qs, env_ct, env_rm,
                         env_ra, env_rh, env_rp, NULL };
        char *argv[] = { (char *)binary, "-c", (char *)conf,
                         (char *)service, NULL };

        snprintf(env_sn, sizeof(env_sn), "SERVER_NAME=%s", "goke");
        snprintf(env_sp, sizeof(env_sp), "SERVER_PORT=%u", local_port);
        snprintf(env_len, sizeof(env_len), "CONTENT_LENGTH=%zu", body_len);
        snprintf(env_uri, sizeof(env_uri), "REQUEST_URI=%s", uri);
        snprintf(env_script, sizeof(env_script), "SCRIPT_NAME=/onvif/%s", service);
        snprintf(env_sf, sizeof(env_sf), "SCRIPT_FILENAME=%s", binary);
        snprintf(env_pi, sizeof(env_pi), "PATH_INFO=%s", uri);
        snprintf(env_qs, sizeof(env_qs), "QUERY_STRING=%s", query ? query : "");
        snprintf(env_ct, sizeof(env_ct), "CONTENT_TYPE=%s",
                 content_type ? content_type : "text/xml");
        snprintf(env_rm, sizeof(env_rm), "REQUEST_METHOD=%s", method);
        snprintf(env_ra, sizeof(env_ra), "REMOTE_ADDR=%s", client_ip);
        snprintf(env_rh, sizeof(env_rh), "REMOTE_HOST=%s", client_ip);
        snprintf(env_rp, sizeof(env_rp), "REMOTE_PORT=%u", client_port);

        close(inpipe[1]);
        close(outpipe[0]);
        dup2(inpipe[0], 0);
        dup2(outpipe[1], 1);
        if (inpipe[0] > 2) close(inpipe[0]);
        if (outpipe[1] > 2) close(outpipe[1]);
        if (chdir_dir && chdir(chdir_dir) != 0)
            _exit(126);
        execve(binary, argv, envp);
        _exit(127);
    }

    close(inpipe[0]);
    close(outpipe[1]);

    if (body_len > 0)
        write(inpipe[1], body, body_len);
    close(inpipe[1]);

    timeout = timeout_s * 1000;
    resp_len = 0;
    while (timeout > 0) {
        pfd.fd = outpipe[0];
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 1000);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            n = (int)read(outpipe[0], resp_buf + resp_len,
                          sizeof(resp_buf) - 1 - (size_t)resp_len);
            if (n > 0) {
                resp_len += n;
                if (resp_len >= (int)sizeof(resp_buf) - 1)
                    break;
                continue;
            }
            break;
        }
        if (pr == 0) {
            if (waitpid(pid, NULL, WNOHANG) == pid)
                break;
            timeout -= 1000;
        } else if (errno != EINTR) {
            break;
        }
    }
    resp_buf[resp_len] = '\0';

    if (timeout <= 0 || resp_len == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(outpipe[0]);
        http_error(fd, 500);
        return -1;
    }
    close(outpipe[0]);
    waitpid(pid, NULL, 0);

    /* Translate CGI stdout into a full HTTP response.
     * Pass 1: locate the status line (HTTP/1.x or Status: header).
     * Pass 2: emit status line + remaining header lines + body. */
    {
        char *boundary = strstr(resp_buf, "\r\n\r\n");
        int hdr_end = boundary ? (int)(boundary - resp_buf) : resp_len;
        const char *rb = boundary ? boundary + 4 : "";
        int rbody = boundary ? resp_len - (hdr_end + 4) : 0;
        int off, sline_len = 0;
        char sline[128];

        for (off = 0; off < hdr_end;) {
            char *eol = memchr(resp_buf + off, '\n', hdr_end - off);
            int len = eol ? (int)(eol - (resp_buf + off)) : hdr_end - off;
            int adv = len + (eol ? 1 : 0);
            if (len > 0 && resp_buf[off + len - 1] == '\r')
                len--;
            if (len >= 7 && !strncmp(resp_buf + off, "HTTP/1.", 7)) {
                if (len > (int)sizeof(sline) - 3)
                    len = (int)sizeof(sline) - 3;
                memcpy(sline, resp_buf + off, (size_t)len);
                sline[len] = '\r'; sline[len + 1] = '\n'; sline[len + 2] = '\0';
                sline_len = len + 2;
                break;
            } else if (len > 7 && !strncasecmp(resp_buf + off, "Status:", 7)) {
                const char *reason = resp_buf + off + 7;
                int rlen;
                while (*reason == ' ') reason++;
                rlen = len - (int)(reason - (resp_buf + off));
                if (rlen > (int)sizeof(sline) - 12)
                    rlen = (int)sizeof(sline) - 12;
                memcpy(sline, "HTTP/1.1 ", 9);
                memcpy(sline + 9, reason, (size_t)rlen);
                sline[9 + rlen] = '\r'; sline[10 + rlen] = '\n';
                sline[11 + rlen] = '\0';
                sline_len = rlen + 11;
                break;
            }
            off += adv;
        }

        if (sline_len > 0)
            write(fd, sline, (size_t)sline_len);
        else
            write(fd, "HTTP/1.1 200 OK\r\n", 17);

        /* Remaining header lines (skip the status line, keep all others). */
        for (off = 0; off < hdr_end;) {
            char *eol = memchr(resp_buf + off, '\n', hdr_end - off);
            int len = eol ? (int)(eol - (resp_buf + off)) : hdr_end - off;
            int adv = len + (eol ? 1 : 0);
            int is_status = 0, is_st;
            if (len > 0 && resp_buf[off + len - 1] == '\r')
                len--;
            is_st = (len >= 7 && !strncmp(resp_buf + off, "HTTP/1.", 7));
            is_status = is_st ||
                        (len > 7 && !strncasecmp(resp_buf + off, "Status:", 7));
            if (!is_status) {
                if (len > 0) {
                    write(fd, resp_buf + off, (size_t)len);
                    write(fd, "\r\n", 2);
                }
            }
            off += adv;
        }
        write(fd, "\r\n", 2);
        if (rbody > 0)
            write(fd, rb, (size_t)rbody);
        shutdown(fd, SHUT_RDWR);
        return 0;
    }
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, timeout_s = DEFAULT_TIMEOUT, opt;
    const char *binary = DEFAULT_BINARY;
    const char *conf = DEFAULT_CONF;
    const char *chdir_dir = NULL;
    int lfd;
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    struct timeval rto = { 8, 0 }, sto = { 20, 0 };
    static char hdr[MAX_HDR + 1];
    static char body[MAX_BODY + 1];

    signal(SIGCHLD, SIG_IGN);

    while ((opt = getopt(argc, argv, "p:b:c:d:t:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'b': binary = optarg; break;
        case 'c': conf = optarg; break;
        case 'd': chdir_dir = optarg; break;
        case 't': timeout_s = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s [-p PORT] [-b BINARY] [-c CONFFILE] "
                            "[-d CHDIR] [-t TIMEOUT]\n", argv[0]);
            return 2;
        }
    }

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    {
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen");
        return 1;
    }

    for (;;) {
        int cfd = accept(lfd, (struct sockaddr *)&sa, &salen);
        size_t hlen = 0, i;
        char *method = NULL, *uri = NULL, *ct = NULL, *qs = "";
        char *boundary = NULL;
        size_t clen = 0;
        char client_ip[64] = "127.0.0.1";

        if (cfd < 0) continue;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));

        while (hlen < sizeof(hdr) - 1) {
            ssize_t rd = read(cfd, hdr + hlen, sizeof(hdr) - 1 - hlen);
            if (rd <= 0) break;
            hlen += (size_t)rd;
            for (i = 0; i + 3 < hlen; i++)
                if (!memcmp(hdr + i, "\r\n\r\n", 4)) { boundary = hdr + i; break; }
            if (boundary) break;
        }
        hdr[hlen] = '\0';
        if (hlen == 0 || boundary == NULL) {
            close(cfd);
            continue;
        }

        /* Request line: METHOD SP URI SP VERSION */
        method = hdr;
        uri = strchr(method, ' ');
        if (!uri) { http_error(cfd, 400); close(cfd); continue; }
        *uri++ = '\0';
        while (*uri == ' ') uri++;
        {
            char *sp = uri;
            while (*sp && *sp != ' ' && *sp != '?') sp++;
            if (*sp == '?') {
                char *q;
                qs = sp + 1;
                *sp = '\0';
                /* strip " HTTP/1.1" that still trails the query string */
                for (q = qs; *q && *q != ' '; q++)
                    ;
                *q = '\0';
            } else if (*sp == ' ') {
                *sp = '\0';
            }
        }

        /* Headers: iterate lines between request line and boundary. */
        {
            char *cur = uri + strlen(uri) + 1;   /* start of header block */
            while (cur < boundary) {
                char *nl = memchr(cur, '\n', (size_t)(boundary - cur));
                char *name, *colon, *val;
                if (!nl) nl = boundary;
                if (nl != boundary)
                    *nl = '\0';
                if (nl > cur && nl[-1] == '\r') nl[-1] = '\0';
                name = cur;
                if (*name == '\0') { cur = nl + 1; continue; }
                colon = strchr(name, ':');
                if (!colon) { cur = nl + 1; continue; }
                *colon++ = '\0';
                while (*colon == ' ') colon++;
                val = colon;
                if (!strcasecmp(name, "Content-Length"))
                    clen = strtoul(val, NULL, 10);
                else if (!strcasecmp(name, "Content-Type") && *val)
                    ct = val;
                cur = nl + 1;
            }
        }

        /* Extra route: GET /ptz (stock web UI PTZ panel bridge). */
        if (uri[0] == '/' && !strncmp(uri, "/ptz", 4)
                && (uri[4] == '\0' || uri[4] == '/')) {
            if (strcmp(method, "GET") != 0)
                http_error(cfd, 405);
            else {
                /* the conf-driven /ptz acts call back into this CGI */
                inet_ntop(AF_INET, &sa.sin_addr, client_ip, sizeof(client_ip));
                gw.binary = binary;
                gw.conf = conf;
                gw.chdir_dir = chdir_dir;
                gw.timeout_s = timeout_s;
                snprintf(gw.client_ip, sizeof(gw.client_ip), "%s", client_ip);
                gw.client_port = ntohs(sa.sin_port);
                run_ptz(cfd, qs);
            }
            close(cfd);
            continue;
        }

        /* Service token from /onvif/<service>. */
        {
            const char *p = uri;
            char service[64];
            char *dst = service;
            int slen = 0, bad = 0;
            if (*p == '/') p++;
            if (!strncmp(p, "onvif/", 6)) p += 6;
            while (*p && *p != '/' && slen < (int)sizeof(service) - 1) {
                if (!(isalnum((unsigned char)*p) || *p == '_')) { bad = 1; break; }
                *dst++ = *p++; slen++;
            }
            *dst = '\0';
            if (bad || !is_valid_service(service)) {
                http_error(cfd, 404);
                close(cfd);
                continue;
            }
            if (strcmp(method, "POST") != 0) {
                http_error(cfd, 405);
                close(cfd);
                continue;
            }

            if (clen > (size_t)sizeof(body) - 1) {
                http_error(cfd, 413);
                close(cfd);
                continue;
            }
            {
                size_t got = 0;
                size_t avail = hlen - (size_t)(boundary + 4 - hdr);
                if (avail > sizeof(body)) avail = sizeof(body);
                if (avail > 0) {
                    got = avail;
                    memcpy(body, boundary + 4, got);
                }
                while (got < clen) {
                    ssize_t rd = read(cfd, body + got, clen - got);
                    if (rd <= 0) break;
                    got += (size_t)rd;
                }
                body[got] = '\0';
                inet_ntop(AF_INET, &sa.sin_addr, client_ip, sizeof(client_ip));
                run_soap(cfd, method, uri, client_ip, ntohs(sa.sin_port),
                         service, ct, qs, body, got, timeout_s, binary, conf,
                         chdir_dir, (uint16_t)port);
            }
        }
        close(cfd);
    }
    return 0;
}