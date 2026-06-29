/*
 * lcd_ipconfig.c
 *
 * Matrix Orbital GLK19264A-7T-1U-USB (BGK19264A-7T)
 * Interactive IP address configuration via 7-key pad.
 *
 * Key mapping (captured from this unit via the "keys" tester):
 *   Up      = 0x42 ('B')
 *   Down    = 0x48 ('H')
 *   Left    = 0x44 ('D')
 *   Right   = 0x43 ('C')
 *   Enter   = 0x45 ('E')
 *   Back    = 0x41 ('A')
 *   Cancel  = 0x47 ('G')
 *
 * Networking: writes a NetworkManager keyfile, then reloads
 * the connection via nmcli.
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -o lcd_ipconfig lcd_ipconfig.c
 */

/* The build uses -D_POSIX_C_SOURCE=200809L (strict POSIX), which hides the
   Linux/BSD socket-ioctl definitions we need (struct ifreq, IFNAMSIZ,
   SIOCGIFADDR, ...). Re-enable them with _DEFAULT_SOURCE. This MUST be
   defined before any system header is included. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <time.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

 /* ------------------------------------------------------------------ */
 /* Configuration                                                        */
 /* ------------------------------------------------------------------ */

/* Default port. The ttyUSBx number is NOT stable across replug/reboot,
   so it can be overridden on the command line (see usage in main).
   For a fixed path, prefer a /dev/serial/by-id/... symlink. */
#define SERIAL_PORT     "/dev/ttyUSB0"
#define DEFAULT_BAUD    B19200

/* The tty node appears as soon as the USB bridge enumerates, which can be
   before the bridge and the display's MCU are ready to accept commands.
   Wait this long after opening before talking to it, so the first init
   commands aren't dropped. Bump it up if init is still occasionally flaky. */
#define DEVICE_SETTLE_MS  600

/* The ethernet interface is discovered at runtime (its name may be
   ens160, eth0, enp3s0, ...) and chosen on the keypad, so it is no
   longer hardcoded. */
#define MAX_IFACES      16
#define IFNAME_LEN      32      /* > IFNAMSIZ (16); names are <=15 chars */

/* Display dimensions */
#define LCD_COLS        192
#define LCD_ROWS        64

/* ------------------------------------------------------------------ */
/* Logging: send every message to syslog and tee it to stdout/stderr.  */
/* ------------------------------------------------------------------ */

static void logmsg(int priority, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    syslog(priority, "%s", buf);

    /* Tee to the console only when it's an interactive terminal. Under systemd
       (or any pipe/redirect) stdout/stderr is already captured into the
       journal, so teeing there would log every line twice. */
    FILE *out = (priority <= LOG_WARNING) ? stderr : stdout;
    if (isatty(fileno(out))) {
        fprintf(out, "%s\n", buf);
        fflush(out);
    }
}

#define log_info(...)  logmsg(LOG_INFO,    __VA_ARGS__)
#define log_warn(...)  logmsg(LOG_WARNING, __VA_ARGS__)
#define log_err(...)   logmsg(LOG_ERR,     __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Matrix Orbital command helpers                                       */
/* ------------------------------------------------------------------ */

#define MO_CMD      0xFE

static int fd_lcd = -1;

/* Reconnect support. The display is hot-pluggable: when it is unplugged the
   serial I/O fails, and we longjmp back to the poll loop in main() rather
   than unwinding return codes through the whole UI. g_in_session gates this
   so a stray error outside a session can't jump into a stale buffer.
   g_dev_path lets the read path notice removal even without a hangup. */
static jmp_buf      g_reconnect;
static volatile int g_in_session = 0;
static char         g_dev_path[256] = {0};
static int          g_keytest = 0;     /* "keys" arg: run the keycode tester */
static int          g_ledtest = 0;     /* "leds" arg: run the LED/GPO tester */
static int          g_nolock  = 0;     /* "nolock" arg: skip the unlock code  */
static char         g_hostname[64] = "host";  /* machine name, shown as title */

/* True if errno indicates the serial device went away (USB unplug). */
static int io_errno_is_disconnect(void)
{
    return errno == EIO || errno == ENXIO || errno == ENODEV || errno == EBADF;
}

static void lcd_write_raw(const uint8_t *buf, size_t len)
{
    ssize_t written = write(fd_lcd, buf, len);
    if (written < 0 && g_in_session && io_errno_is_disconnect())
        longjmp(g_reconnect, 1);   /* device gone: back to the poll loop */
    (void)written;
    /* small delay after commands to avoid overrunning the display */
    struct timespec ts = { 0, 2000000L }; /* 2 ms */
    nanosleep(&ts, NULL);
}

static void lcd_cmd1(uint8_t cmd)
{
    uint8_t buf[2] = { MO_CMD, cmd };
    lcd_write_raw(buf, 2);
}

static void lcd_cmd2(uint8_t cmd, uint8_t p1)
{
    uint8_t buf[3] = { MO_CMD, cmd, p1 };
    lcd_write_raw(buf, 3);
}

static void lcd_cmd3(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    uint8_t buf[4] = { MO_CMD, cmd, p1, p2 };
    lcd_write_raw(buf, 4);
}

/* Clear screen */
static void lcd_clear(void)
{
    lcd_cmd1(0x58);
    struct timespec ts = { 0, 10000000L }; /* 10 ms */
    nanosleep(&ts, NULL);
}

/* Set text cursor (1-based col/row for text mode) */
static void lcd_set_cursor(uint8_t col, uint8_t row)
{
    lcd_cmd3(0x47, col, row);
}

/* Write text string at current cursor */
static void lcd_print(const char *s)
{
    lcd_write_raw((const uint8_t *)s, strlen(s));
}

/* Approximate text columns across the 192px display with the ~6px font. */
#define TEXT_COLS   32

/* Print a string centred on the given text row. */
static void lcd_print_centered(uint8_t row, const char *s)
{
    int len = (int)strlen(s);
    int col = (len >= TEXT_COLS) ? 1 : (TEXT_COLS - len) / 2 + 1;
    lcd_set_cursor((uint8_t)col, row);
    lcd_print(s);
}

/* Backlight on indefinitely */
static void lcd_backlight_on(void)
{
    lcd_cmd2(0x42, 0);  /* 0 = stay on */
}

/* Set contrast (0-255, ~128 is mid) */
static void lcd_contrast(uint8_t c)
{
    lcd_cmd2(0x50, c);
}

/* Draw a horizontal line using the drawing commands */
static void lcd_hline(uint8_t x1, uint8_t y, uint8_t x2)
{
    uint8_t buf[6] = { MO_CMD, 0x6C, x1, y, x2, y };
    lcd_write_raw(buf, 6);
}

/* ---- Keypad LEDs (GLK..-7T) -------------------------------------------- */
/*
 * The three keypad LEDs are wired to General Purpose Outputs (GPOs); each
 * LED is bi-colour with one GPO for green and one for red. Matrix Orbital
 * GPO commands:  On = 0xFE 0x57 <n>,  Off = 0xFE 0x56 <n>.
 *
 * The mapping below is the documented GLK19264A-7T layout. It is kept in a
 * table because hardware revisions differ — if testing shows the wrong
 * behaviour, adjust here rather than hunting through the code:
 *   - LEDs light the WRONG COLOUR  -> swap the GREEN/RED numbers.
 *   - LEDs stay DARK when they should be lit (active-low wiring)
 *                                  -> set LED_ACTIVE_LOW to 1.
 */
#define LED_ACTIVE_LOW   0

static const uint8_t LED_GREEN_GPO[3] = { 1, 3, 5 };
static const uint8_t LED_RED_GPO[3]   = { 2, 4, 6 };

static void lcd_gpo_set(uint8_t n, int on)
{
    if (LED_ACTIVE_LOW) on = !on;
    lcd_cmd2(on ? 0x57 : 0x56, n);
}

/* Set all three keypad LEDs to one colour: green_on=1 -> green, 0 -> red. */
static void lcd_leds_set(int green_on)
{
    for (int i = 0; i < 3; i++) {
        lcd_gpo_set(LED_GREEN_GPO[i], green_on);
        lcd_gpo_set(LED_RED_GPO[i], !green_on);
    }
}

static void lcd_leds_green(void) { lcd_leds_set(1); }
static void lcd_leds_red(void)   { lcd_leds_set(0); }

/* ---- Heartbeat: blink the centre LED to show an active connection ------ */
/* The centre LED (index 1) toggles green on/off once per second while a
   session is active; the top and bottom LEDs stay solid green. */
#define HEARTBEAT_MS      1000
#define LED_CENTER        1

static int             g_heartbeat = 0;     /* enabled during a live session */
static int             g_hb_on = 1;         /* current centre-LED state      */
static struct timespec g_hb_last;           /* time of the last toggle       */

/*
 * Drive the centre LED for the heartbeat: on -> green, off -> amber.
 * (On this module both GPOs off lights both elements = amber, which is an
 * acceptable "off" phase for the pulse.)
 */
static void heartbeat_apply(int on)
{
    lcd_gpo_set(LED_GREEN_GPO[LED_CENTER], on);
    lcd_gpo_set(LED_RED_GPO[LED_CENTER], 0);
}

/* Begin heartbeat: centre LED on, timer reset. */
static void heartbeat_start(void)
{
    g_heartbeat = 1;
    g_hb_on = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_hb_last);
    heartbeat_apply(1);
}

/* Stop the heartbeat (centre LED left under caller's control). */
static void heartbeat_stop(void)
{
    g_heartbeat = 0;
}

/* Toggle the centre LED if a full HEARTBEAT_MS has elapsed. Cheap to call
   often; it only touches the LED when the interval is up. */
static void heartbeat_tick(void)
{
    if (!g_heartbeat)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec  - g_hb_last.tv_sec)  * 1000
            + (now.tv_nsec - g_hb_last.tv_nsec) / 1000000;
    if (ms >= HEARTBEAT_MS) {
        g_hb_on = !g_hb_on;
        heartbeat_apply(g_hb_on);
        g_hb_last = now;
    }
}

/* ------------------------------------------------------------------ */
/* LCD initialisation                                                  */
/* ------------------------------------------------------------------ */

static int lcd_open(const char *port, speed_t baud)
{
    fd_lcd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_lcd < 0) {
        perror("open serial port");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd_lcd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    /* Matrix Orbital GLK modules decode the serial stream at their
       configured baud rate even over USB. It MUST match the display's
       setting or every byte is mis-sampled and you get garbage glyphs.
       This module is configured for 19200 (the MO factory default). */
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= ~(PARENB | CSTOPB);
    tty.c_cflag |= CLOCAL | CREAD;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;   /* 100 ms read timeout */

    if (tcsetattr(fd_lcd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }

    fcntl(fd_lcd, F_SETFL, 0);   /* switch to blocking after config */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Keypad                                                              */
/* ------------------------------------------------------------------ */

/* Key codes as reported by this GLK19264A-7T-USB unit (captured with the
   "keys" tester). Note Up/Down/Back differ from the generic MO defaults. */
#define KEY_UP      0x42  /* 'B' */
#define KEY_DOWN    0x48  /* 'H' */
#define KEY_RIGHT   0x43  /* 'C' */
#define KEY_LEFT    0x44  /* 'D' */
#define KEY_ENTER   0x45  /* 'E' */
#define KEY_BACK    0x41  /* 'A' */
#define KEY_CANCEL  0x47  /* 'G' */

/* Key sequence that unlocks the UI on connect. This is a casual tamper
   deterrent, not real security. Change the keys (or length) freely; skip the
   whole thing at runtime with the "nolock" argument. */
static const int g_passcode[] = { KEY_UP, KEY_UP, KEY_DOWN, KEY_ENTER };
#define PASSCODE_LEN  ((int)(sizeof(g_passcode) / sizeof(g_passcode[0])))

/*
 * Read one keypress. Returns the key byte, or 0 on timeout, -1 on error.
 * timeout_ms: 0 = block indefinitely.
 *
 * To stay responsive to hot-unplug, a blocking wait is serviced in 1-second
 * slices; on each slice we verify the device node still exists, and a vanished
 * device (or an I/O error) longjmps back to the reconnect loop.
 */
static int lcd_read_key(int timeout_ms)
{
    for (;;) {
        heartbeat_tick();      /* blink the centre LED while we wait */

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_lcd, &rfds);

        /* Blocking reads wait in heartbeat-sized slices so the LED keeps
           ticking; timed reads (all < HEARTBEAT_MS) use their own timeout. */
        int slice_ms = (timeout_ms > 0) ? timeout_ms : HEARTBEAT_MS;
        struct timeval tv = { slice_ms / 1000, (slice_ms % 1000) * 1000 };

        int ret = select(fd_lcd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (g_in_session && io_errno_is_disconnect()) longjmp(g_reconnect, 1);
            return -1;
        }
        if (ret == 0) {
            /* timeout slice: confirm the device is still present */
            if (g_in_session && g_dev_path[0] && access(g_dev_path, F_OK) != 0)
                longjmp(g_reconnect, 1);
            if (timeout_ms > 0) return 0;   /* bounded wait expired */
            continue;                        /* blocking wait: keep going */
        }

        uint8_t b;
        ssize_t n = read(fd_lcd, &b, 1);
        if (n <= 0) {
            if (g_in_session) longjmp(g_reconnect, 1);
            return -1;
        }
        return (int)b;
    }
}

/* ------------------------------------------------------------------ */
/* IP validation                                                       */
/* ------------------------------------------------------------------ */

static int valid_ipv4(const char *s)
{
    struct in_addr dummy;
    return inet_pton(AF_INET, s, &dummy) == 1;
}

/* ------------------------------------------------------------------ */
/* Live interface configuration (to preload the editor)               */
/* ------------------------------------------------------------------ */

/* Split a network-order IPv4 address into four octets. */
static void addr_to_octets(uint32_t net_addr, int oct[4])
{
    uint32_t h = ntohl(net_addr);
    oct[0] = (int)((h >> 24) & 0xFF);
    oct[1] = (int)((h >> 16) & 0xFF);
    oct[2] = (int)((h >> 8)  & 0xFF);
    oct[3] = (int)( h        & 0xFF);
}

/*
 * Query the current IPv4 address and netmask of an interface via ioctl.
 * On success fills ip_oct/nm_oct and returns 0. Returns -1 if the
 * interface has no IPv4 address (e.g. unconfigured) or on error.
 */
static int get_iface_ipv4(const char *ifname, int ip_oct[4], int nm_oct[4])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    int rc = -1;
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        addr_to_octets(sin->sin_addr.s_addr, ip_oct);

        if (ioctl(s, SIOCGIFNETMASK, &ifr) == 0) {
            sin = (struct sockaddr_in *)&ifr.ifr_netmask;
            addr_to_octets(sin->sin_addr.s_addr, nm_oct);
            rc = 0;
        }
    }
    close(s);
    return rc;
}

/*
 * Read the default-route gateway for an interface from /proc/net/route.
 * On success fills gw_oct and returns 0; returns -1 if no default route
 * is associated with the interface.
 *
 * The Gateway column is the address as a native-endian hex of the raw
 * (network-order) 32-bit value, so on a little-endian host the parsed
 * value can be used directly as an in_addr.s_addr.
 */
static int get_iface_gateway(const char *ifname, int gw_oct[4])
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {   /* discard header row */
        fclose(f);
        return -1;
    }

    int rc = -1;
    char iface[64];
    unsigned long dest, gw;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %lx %lx", iface, &dest, &gw) != 3) continue;
        if (dest != 0) continue;                    /* default route only */
        if (strcmp(iface, ifname) != 0) continue;
        addr_to_octets((uint32_t)gw, gw_oct);
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Ethernet interface discovery                                        */
/* ------------------------------------------------------------------ */

/* Read a single integer out of a sysfs file. Returns 0 on success. */
static int read_sysfs_long(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int rc = (fscanf(f, "%ld", out) == 1) ? 0 : -1;
    fclose(f);
    return rc;
}

/* Skip names that are obviously virtual/non-physical interfaces. */
static int is_virtual_iface(const char *name)
{
    static const char *pfx[] = {
        "lo", "veth", "docker", "br-", "virbr",
        "vnet", "tap", "tun", "bond", "dummy", "vmnet",
    };
    for (size_t i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++)
        if (strncmp(name, pfx[i], strlen(pfx[i])) == 0) return 1;
    return 0;
}

/*
 * Enumerate real ethernet interfaces by scanning /sys/class/net, in
 * alphabetical order (glob sorts). An interface qualifies when:
 *   - its type is ARPHRD_ETHER (1),
 *   - it is not a Wi-Fi device (no "wireless" attribute), and
 *   - its name is not an obvious virtual prefix.
 * Fills names[] and returns the count (0 if none).
 */
static int list_eth_ifaces(char names[][IFNAME_LEN], int max)
{
    glob_t g;
    if (glob("/sys/class/net/*", 0, NULL, &g) != 0) return 0;

    int n = 0;
    for (size_t i = 0; i < g.gl_pathc && n < max; i++) {
        const char *slash = strrchr(g.gl_pathv[i], '/');
        const char *name = slash ? slash + 1 : g.gl_pathv[i];
        if (is_virtual_iface(name)) continue;

        char path[256];
        long type;
        snprintf(path, sizeof(path), "%s/type", g.gl_pathv[i]);
        if (read_sysfs_long(path, &type) != 0 || type != 1) continue;  /* ARPHRD_ETHER */

        snprintf(path, sizeof(path), "%s/wireless", g.gl_pathv[i]);
        if (access(path, F_OK) == 0) continue;   /* skip Wi-Fi */

        snprintf(names[n], IFNAME_LEN, "%s", name);
        n++;
    }
    globfree(&g);
    return n;
}

/* ------------------------------------------------------------------ */
/* Network configuration write                                         */
/* ------------------------------------------------------------------ */

static int write_network_file(const char* iface,
    const char* ip,
    const char* netmask,
    const char* gateway)
{
    /* Convert dotted-quad netmask to prefix length */
    struct in_addr nm;
    if (inet_pton(AF_INET, netmask, &nm) != 1) return -1;

    uint32_t bits = ntohl(nm.s_addr);
    int prefix = 0;
    while (bits & 0x80000000u) { prefix++; bits <<= 1; }

    /* Keyfile is named after the connection id, which we set to the
       interface name. */
    char path[256];
    snprintf(path, sizeof(path),
             "/etc/NetworkManager/system-connections/%.*s.nmconnection",
             IFNAME_LEN - 1, iface);

    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen network file"); return -1; }

    fprintf(f,
        "[connection]\n"
        "id=%s\n"
        "type=ethernet\n"
        "interface-name=%s\n\n"
        "[ipv4]\n"
        "method=manual\n"
        "addresses=%s/%d\n"
        "gateway=%s\n"
        "dns=8.8.8.8;\n\n"
        "[ipv6]\n"
        "method=ignore\n",
        iface, iface, ip, prefix, gateway);

    fflush(f);
    fclose(f);

    /* NM keyfiles must be root-owned and 600 */
    chmod(path, 0600);

    return 0;
}


static int apply_network(const char *iface)
{
    /* Reload the keyfile from disk, then bring the connection up.
       iface comes from the kernel's interface list (safe charset), but
       single-quote it defensively when handed to the shell. */
    char cmd[128];
    if (system("nmcli connection reload") != 0) return -1;
    snprintf(cmd, sizeof(cmd), "nmcli connection up '%.*s'", IFNAME_LEN - 1, iface);
    if (system(cmd) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
/* ------------------------------------------------------------------ */

/* Display a two-line status message, centred, then wait for Enter   */
static void ui_message(const char *line1, const char *line2)
{
    lcd_clear();
    lcd_set_cursor(1, 2);
    lcd_print(line1);
    lcd_set_cursor(1, 3);
    lcd_print(line2);
    lcd_set_cursor(1, 4);
    lcd_print("  [ Press ENTER ]");

    /* drain keys then wait for ENTER or CANCEL */
    while (1) {
        int k = lcd_read_key(0);
        if (k == KEY_ENTER || k == KEY_CANCEL || k < 0) break;
    }
}

/*
 * ui_select_iface
 *
 * Present a scrollable list of interface names and let the user pick one with
 * UP/DOWN + ENTER. Returns the chosen index, or -1 if the user pressed BACK or
 * CANCEL to quit (release the display).
 */
static int ui_select_iface(char names[][IFNAME_LEN], int count)
{
    const int visible = 5;   /* list rows, leaving the last row for the hint */
    int sel = 0;
    int top = 0;

    while (1) {
        if (sel < top)            top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        lcd_clear();
        lcd_print_centered(1, g_hostname);     /* machine name as the title */
        lcd_hline(0, 9, 191);

        for (int i = 0; i < visible && (top + i) < count; i++) {
            int idx = top + i;

            /* Show the interface's current IPv4 next to its name. */
            int ip[4], nm[4];
            char ipbuf[20];
            if (get_iface_ipv4(names[idx], ip, nm) == 0)
                snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d",
                         ip[0], ip[1], ip[2], ip[3]);
            else
                snprintf(ipbuf, sizeof(ipbuf), "no IP");

            char row[IFNAME_LEN + 24];
            snprintf(row, sizeof(row), "%c%-7s %s",
                     (idx == sel) ? '>' : ' ', names[idx], ipbuf);
            lcd_set_cursor(1, (uint8_t)(3 + i));
            lcd_print(row);
        }

        lcd_set_cursor(1, 8);
        lcd_print("ENTER=select BACK=quit");

        int k = lcd_read_key(0);
        switch (k) {
            case KEY_UP:     sel = (sel == 0)         ? count - 1 : sel - 1; break;
            case KEY_DOWN:   sel = (sel == count - 1) ? 0         : sel + 1; break;
            case KEY_ENTER:  return sel;
            case KEY_BACK:                 /* quit: release the display */
            case KEY_CANCEL: return -1;
            default: break;
        }
    }
}

/*
 * ui_edit_octet
 *
 * Edit a single decimal field (0-255) in place on the display.
 * x, y: pixel position of the 3-char field.
 * *value: current value, updated on exit.
 * Returns 1 if Enter pressed, 0 if Back pressed, -1 if Cancel.
 */
static int ui_edit_octet(uint8_t x, uint8_t y, int *value)
{
    int v = *value;
    char buf[4];

    /* Enable auto-scrolling off, use drawing mode */
    while (1) {
        /* Clear the field area (3 chars wide, ~18px per char on default font) */
        snprintf(buf, sizeof(buf), "%3d", v);

        /* Draw field with simple invert trick: print with brackets */
        /* Position cursor at pixel row. The GLK text rows are ~8px each;
           pixel y / 8 gives approx row, x / 6 gives approx col (6px wide font).
           We use text cursor commands for simplicity: col = x/6+1, row = y/8+1 */
        uint8_t col = (uint8_t)(x / 6 + 1);
        uint8_t row = (uint8_t)(y / 8 + 1);
        lcd_set_cursor(col, row);
        lcd_print("[");
        lcd_print(buf);
        lcd_print("]");

        int k = lcd_read_key(0);
        switch (k) {
            case KEY_UP:
                v = (v >= 255) ? 0 : v + 1;
                break;
            case KEY_DOWN:
                v = (v <= 0) ? 255 : v - 1;
                break;
            case KEY_RIGHT:
                v = (v > 245) ? 255 : v + 10;
                break;
            case KEY_LEFT:
                v = (v < 10) ? 0 : v - 10;
                break;
            case KEY_ENTER:
                *value = v;
                /* clear brackets */
                lcd_set_cursor(col, row);
                lcd_print(" ");
                lcd_print(buf);
                lcd_print(" ");
                return 1;
            case KEY_BACK:
                *value = v;
                lcd_set_cursor(col, row);
                lcd_print(" ");
                lcd_print(buf);
                lcd_print(" ");
                return 0;
            case KEY_CANCEL:
                return -1;
            default:
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* IP field editor                                                     */
/* ------------------------------------------------------------------ */

/*
 * Edit a full dotted-quad address.
 * label:   e.g. "IP Address"
 * octets[4]: current values, updated on success.
 * Returns 1 = confirmed, 0 = back, -1 = cancel.
 */
static int ui_edit_ip(const char *label, int octets[4])
{
    /*
     * Layout (192x64 display):
     *   Row 1 (y=0):  label
     *   Row 2 (y=8):  separator line
     *   Row 3 (y=16): "  AAA . BBB . CCC . DDD"
     *   Row 4 (y=24): instructions
     *   Row 5 (y=32): "UP/DN=+1  L/R=+10"
     *   Row 6 (y=40): "ENTER=next  BACK=prev"
     */
    int work[4];
    memcpy(work, octets, sizeof(work));

    /* Pixel x positions of the four octet fields (approx, 6px-wide font) */
    /* "  [AAA].[BBB].[CCC].[DDD]" starting at x=6 */
    /* Field widths: "[AAA]" = 5 chars = 30px each, dots in between */
    /* col positions (1-based text cols): 3, 9, 15, 21  (approx)   */
    /* We address by octet index 0..3                               */
    const uint8_t octet_x[4] = { 6, 42, 78, 114 };  /* pixel x    */
    const uint8_t octet_y    = 16;                    /* pixel y    */

    lcd_clear();

    /* Title */
    lcd_set_cursor(1, 1);
    lcd_print(label);

    /* Separator */
    lcd_hline(0, 9, 191);

    /* Dots between octets */
    lcd_set_cursor(7, 3);  lcd_print(".");
    lcd_set_cursor(13, 3); lcd_print(".");
    lcd_set_cursor(19, 3); lcd_print(".");

    /* Hint rows */
    lcd_set_cursor(1, 5);
    lcd_print("UP/DN:+/-1  L/R:+/-10");
    lcd_set_cursor(1, 6);
    lcd_print("ENTER:next BACK:prev CANC:abort");

    int octet = 0;
    while (octet >= 0 && octet < 4) {
        int r = ui_edit_octet(octet_x[octet], octet_y, &work[octet]);
        if (r == -1) return -1;   /* Cancel */
        if (r == 1)  octet++;     /* Enter: advance */
        if (r == 0)  octet--;     /* Back: go back  */
    }

    if (octet < 0) return 0;      /* Backed out of first field */

    memcpy(octets, work, sizeof(work));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main UI state machine                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    STATE_IDLE,
    STATE_MENU,
    STATE_EDIT_IP,
    STATE_EDIT_NETMASK,
    STATE_EDIT_GATEWAY,
    STATE_CONFIRM,
    STATE_APPLY,
    STATE_DONE
} AppState;

/* Convert octets array to dotted string */
static void octets_to_str(const int oct[4], char *out, size_t sz)
{
    snprintf(out, sz, "%d.%d.%d.%d", oct[0], oct[1], oct[2], oct[3]);
}

static void show_idle_screen(const char *iface)
{
    char line[IFNAME_LEN + 16];
    lcd_clear();
    lcd_print_centered(1, g_hostname);     /* machine name as the title */
    lcd_hline(0, 9, 191);
    lcd_set_cursor(1, 3);
    lcd_print("  ENTER=configure");
    lcd_set_cursor(1, 4);
    lcd_print("  BACK =quit");
    lcd_set_cursor(1, 6);
    snprintf(line, sizeof(line), "  Interface: %s", iface);
    lcd_print(line);
}

static void show_confirm_screen(const char *ip,
                                 const char *nm,
                                 const char *gw)
{
    char line[32];
    lcd_clear();
    lcd_set_cursor(1, 1);
    lcd_print("Confirm settings:");
    lcd_hline(0, 9, 191);
    lcd_set_cursor(1, 2);
    snprintf(line, sizeof(line), "IP : %s", ip);
    lcd_print(line);
    lcd_set_cursor(1, 3);
    snprintf(line, sizeof(line), "NM : %s", nm);
    lcd_print(line);
    lcd_set_cursor(1, 4);
    snprintf(line, sizeof(line), "GW : %s", gw);
    lcd_print(line);
    lcd_set_cursor(1, 5);
    lcd_print("ENTER=apply RIGHT=+reboot");
    lcd_set_cursor(1, 6);
    lcd_print("CANCEL=abort");
}

/*
 * Hunt for a USB serial device when no port is given on the command line.
 * Scans /dev in priority order and returns the first node we can actually
 * open. /dev/serial/by-id is tried first because those names are stable
 * across replug/reboot, unlike the ttyUSBx number.
 *
 * Writes the chosen path into out (size sz). Returns 0 on success, -1 if
 * nothing suitable was found.
 */
static int find_usb_serial(char *out, size_t sz)
{
    static const char *patterns[] = {
        "/dev/serial/by-id/*",   /* stable symlinks, preferred */
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
    };

    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        glob_t g;
        if (glob(patterns[p], 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                int fd = open(g.gl_pathv[i], O_RDWR | O_NOCTTY | O_NONBLOCK);
                if (fd >= 0) {
                    close(fd);
                    snprintf(out, sz, "%s", g.gl_pathv[i]);
                    globfree(&g);
                    return 0;
                }
                /* couldn't open (busy/permission) — try the next match */
            }
        }
        globfree(&g);
    }
    return -1;
}

/* Sleep for a number of milliseconds. */
static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * Block until a usable serial device is present, polling every 500 ms.
 * If port_override is set, wait for that specific node to appear; otherwise
 * auto-detect. The chosen path is written to out (size sz).
 */
static void wait_for_device(const char *port_override, char *out, size_t sz)
{
    for (;;) {
        if (port_override) {
            if (access(port_override, F_OK) == 0) {
                snprintf(out, sz, "%s", port_override);
                return;
            }
        } else if (find_usb_serial(out, sz) == 0) {
            return;
        }
        msleep(500);
    }
}

/* Map a baud-rate string ("115200", "19200", ...) to a termios speed_t.
   Returns B0 for an unrecognised value. */
static speed_t baud_from_str(const char *s)
{
    if (strcmp(s, "9600")   == 0) return B9600;
    if (strcmp(s, "19200")  == 0) return B19200;
    if (strcmp(s, "38400")  == 0) return B38400;
    if (strcmp(s, "57600")  == 0) return B57600;
    if (strcmp(s, "115200") == 0) return B115200;
    return B0;
}

/*
 * Keycode tester (run with the "keys" argument). Shows the hex value of each
 * key as it is pressed, on the LCD and on stdout, so the real keypad codes
 * can be read off and plugged into the KEY_* defines. Exit with Ctrl-C (or
 * by unplugging the display).
 */
static void run_keytest(void)
{
    lcd_clear();
    lcd_set_cursor(1, 1);
    lcd_print("Keycode test");
    lcd_hline(0, 9, 191);
    lcd_set_cursor(1, 3);
    lcd_print("Press each key;");
    lcd_set_cursor(1, 4);
    lcd_print("note the codes.");

    log_info("Keycode test: press keys (Ctrl-C to quit)");

    for (;;) {
        int k = lcd_read_key(0);
        if (k <= 0) continue;

        char line[32];
        snprintf(line, sizeof(line), "code = 0x%02X (%c)  ",
                 k, (k >= 32 && k < 127) ? k : '.');
        lcd_set_cursor(1, 6);
        lcd_print(line);
        log_info("key code = 0x%02X ('%c')", k, (k >= 32 && k < 127) ? k : '.');
    }
}

/*
 * LED/GPO tester (run with the "leds" argument). Cycles through "all GPOs
 * off" and then each GPO 1..6 lit alone, ~1.3 s apart, reporting the state on
 * the LCD and in syslog. Use it to map which GPO drives which LED/colour and
 * to confirm whether "all off" actually goes dark. CANCEL exits.
 */
static void run_ledtest(void)
{
    lcd_clear();
    lcd_set_cursor(1, 1);
    lcd_print("LED / GPO test");
    lcd_hline(0, 9, 191);
    lcd_set_cursor(1, 6);
    lcd_print("CANCEL to exit");

    log_info("LED test: cycling all-off then GPO 1..6 (CANCEL to quit)");

    int n = 0;   /* 0 = all GPOs off; 1..6 = that GPO lit alone */
    for (;;) {
        for (int g = 1; g <= 6; g++)
            lcd_gpo_set((uint8_t)g, 0);

        char line[32];
        if (n == 0) {
            snprintf(line, sizeof(line), "ALL GPOs OFF   ");
        } else {
            lcd_gpo_set((uint8_t)n, 1);
            snprintf(line, sizeof(line), "GPO %d ON       ", n);
        }
        lcd_set_cursor(1, 3);
        lcd_print(line);
        log_info("LED test: %s", line);

        int k = lcd_read_key(1300);    /* auto-advance, or a key */
        if (k == KEY_CANCEL)
            return;
        n = (n + 1) % 7;
    }
}

/*
 * Show the lock screen and read the unlock key sequence. Returns 1 if the
 * full sequence is entered correctly, or 0 on the first wrong key (a failed
 * attempt, which the caller turns into the reconnect-USB screen).
 */
static int ui_unlock(void)
{
    lcd_clear();
    lcd_print_centered(1, g_hostname);
    lcd_hline(0, 9, 191);
    lcd_print_centered(3, "Locked");
    lcd_print_centered(4, "Enter code");
    log_info("Locked; awaiting unlock sequence");

    int pos = 0;
    while (pos < PASSCODE_LEN) {
        /* Progress indicator: one '*' per accepted key, fixed width. */
        char show[PASSCODE_LEN + 1];
        for (int i = 0; i < PASSCODE_LEN; i++)
            show[i] = (i < pos) ? '*' : ' ';
        show[PASSCODE_LEN] = '\0';
        lcd_print_centered(6, show);

        int k = lcd_read_key(0);
        if (k <= 0)
            continue;                  /* ignore non-keys */
        if (k == g_passcode[pos]) {
            pos++;                     /* correct key: advance */
        } else {
            log_warn("Unlock failed (wrong key 0x%02X)", k);
            return 0;                  /* wrong key: failed attempt */
        }
    }
    log_info("Unlocked");
    return 1;
}

/*
 * Release the display: turn the LEDs off and show a "reconnect USB" prompt.
 * The program itself stays resident; the caller ends the current session and
 * the main loop waits for the device to be unplugged/replugged to restart.
 */
static void deinit_display(void)
{
    log_info("Display released by user; reconnect USB to restart");
    heartbeat_stop();      /* stop blinking; centre LED goes solid red below */
    lcd_leds_red();
    lcd_clear();
    lcd_print_centered(1, g_hostname);
    lcd_hline(0, 9, 191);
    lcd_print_centered(4, "Reconnect USB");
    lcd_print_centered(5, "to restart");

    /* Make sure all of the above actually reaches the display before the
       caller closes the port, otherwise the queued bytes are discarded and
       the screen is never updated. */
    if (fd_lcd >= 0)
        tcdrain(fd_lcd);
}

/* run_config_ui() outcomes. */
typedef enum { UI_RESELECT = 0, UI_QUIT = 1 } UiResult;

/*
 * Run the IP-config UI for one interface, starting from the preloaded octets.
 * Returns UI_RESELECT when the user presses CANCEL (caller re-shows the
 * interface picker) or UI_QUIT when the user presses BACK to release the
 * display (caller ends the session; the program stays resident).
 */
static UiResult run_config_ui(const char *iface,
                              int ip_oct[4], int nm_oct[4], int gw_oct[4])
{
    char ip_str[16], nm_str[16], gw_str[16];

    AppState state = STATE_IDLE;
    show_idle_screen(iface);

    while (1) {
        int key = lcd_read_key(500);  /* 500 ms poll */

        switch (state) {

        case STATE_IDLE:
            if (key == KEY_CANCEL)
                return UI_RESELECT;   /* back to interface selection */
            if (key == KEY_BACK) {
                deinit_display();     /* red LEDs + reconnect-USB message */
                return UI_QUIT;       /* end session; program stays resident */
            }
            if (key == KEY_ENTER) {
                int r = ui_edit_ip("IP Address", ip_oct);
                if (r == -1) return UI_RESELECT;  /* cancel -> reselect */
                if (r == 1) {
                    r = ui_edit_ip("Netmask", nm_oct);
                    if (r == -1) return UI_RESELECT;
                    if (r == 1) {
                        r = ui_edit_ip("Gateway", gw_oct);
                        if (r == -1) return UI_RESELECT;
                    }
                }

                if (r != 1) {
                    /* Backed out (not cancelled): redraw idle */
                    state = STATE_IDLE;
                    show_idle_screen(iface);
                    break;
                }

                state = STATE_CONFIRM;
                octets_to_str(ip_oct, ip_str, sizeof(ip_str));
                octets_to_str(nm_oct, nm_str, sizeof(nm_str));
                octets_to_str(gw_oct, gw_str, sizeof(gw_str));
                show_confirm_screen(ip_str, nm_str, gw_str);
            }
            break;

        case STATE_CONFIRM:
            if (key == KEY_ENTER || key == KEY_RIGHT) {
                int do_reboot = (key == KEY_RIGHT);
                state = STATE_APPLY;

                lcd_clear();
                lcd_set_cursor(1, 3);
                lcd_print("  Applying...");

                int ok = 0;
                if (valid_ipv4(ip_str) && valid_ipv4(nm_str) && valid_ipv4(gw_str)) {
                    if (write_network_file(iface, ip_str, nm_str, gw_str) == 0) {
                        if (apply_network(iface) == 0) ok = 1;
                    }
                }

                if (ok)
                    log_info("Applied %s: ip=%s netmask=%s gateway=%s",
                             iface, ip_str, nm_str, gw_str);
                else
                    log_err("Failed to apply config for %s (ip=%s nm=%s gw=%s)",
                            iface, ip_str, nm_str, gw_str);

                if (ok && do_reboot) {
                    /* Signal shutdown with all three LEDs red, then reboot. */
                    log_warn("Config applied; rebooting now");
                    heartbeat_stop();
                    lcd_leds_red();
                    lcd_clear();
                    lcd_set_cursor(1, 3);
                    lcd_print("  Rebooting...");
                    msleep(800);            /* let the red LEDs/message show */
                    g_in_session = 0;
                    if (fd_lcd >= 0) { close(fd_lcd); fd_lcd = -1; }
                    if (system("systemctl reboot") != 0)
                        system("reboot");   /* fallback */
                    exit(0);
                }

                if (ok) {
                    ui_message("  Config applied!", "  Network restarted.");
                } else {
                    ui_message("  ERROR", "  See syslog.");
                }

                state = STATE_IDLE;
                show_idle_screen(iface);

            } else if (key == KEY_CANCEL) {
                return UI_RESELECT;   /* back to interface selection */
            } else if (key == KEY_BACK) {
                state = STATE_IDLE;
                show_idle_screen(iface);
            }
            break;

        /* These states are driven inline from STATE_IDLE above */
        default:
            state = STATE_IDLE;
            show_idle_screen(iface);
            break;
        }
    }
}

/*
 * Run one display session on an already-open device: light the LEDs,
 * initialise the panel, then loop on interface selection. Picking an
 * interface runs the editor; pressing CANCEL in the editor returns here to
 * choose again. Returns (ending the session) when the selection screen
 * itself is cancelled or there is no interface to configure. On hot-unplug
 * the serial helpers longjmp back to the reconnect loop in main().
 */
static void run_session(void)
{
    /* Display is alive: signal "ready" with all three LEDs green. */
    lcd_leds_green();
    lcd_backlight_on();
    lcd_contrast(128);
    lcd_clear();

    if (g_keytest) {       /* diagnostic mode: just report key codes */
        run_keytest();
        return;
    }
    if (g_ledtest) {       /* diagnostic mode: cycle the LEDs/GPOs */
        run_ledtest();
        return;
    }

    /* Require the unlock sequence (unless disabled with "nolock"). A wrong
       key releases the display so the user must reconnect to try again. */
    if (!g_nolock && !ui_unlock()) {
        deinit_display();
        return;
    }

    heartbeat_start();     /* centre LED blinks while the session is live */

    for (;;) {
        /* Discover the ethernet interfaces and let the user choose one.
           If there's exactly one, use it without prompting. */
        char ifaces[MAX_IFACES][IFNAME_LEN];
        int nif = list_eth_ifaces(ifaces, MAX_IFACES);
        if (nif <= 0) {
            ui_message("No ethernet", "interface found");
            return;
        }

        int sel = 0;
        if (nif > 1) {
            sel = ui_select_iface(ifaces, nif);
            if (sel < 0) {             /* BACK/CANCEL: quit from first screen */
                deinit_display();      /* red LEDs + reconnect-USB message */
                return;                /* ends the session; stays resident */
            }
        }
        const char *iface = ifaces[sel];
        log_info("Configuring interface: %s", iface);

        /* Working copies of address fields. These are fallbacks: if the
           interface is already configured, the live values are loaded over
           them below so the editor starts from the current settings. */
        int ip_oct[4]  = {192, 168, 1, 100};
        int nm_oct[4]  = {255, 255, 255, 0};
        int gw_oct[4]  = {192, 168, 1, 1};

        if (get_iface_ipv4(iface, ip_oct, nm_oct) == 0)
            log_info("Preloaded %s: %d.%d.%d.%d / %d.%d.%d.%d", iface,
                     ip_oct[0], ip_oct[1], ip_oct[2], ip_oct[3],
                     nm_oct[0], nm_oct[1], nm_oct[2], nm_oct[3]);
        else
            log_info("No live IPv4 on %s; using default address.", iface);

        if (get_iface_gateway(iface, gw_oct) == 0)
            log_info("Preloaded gateway: %d.%d.%d.%d",
                     gw_oct[0], gw_oct[1], gw_oct[2], gw_oct[3]);
        else
            log_info("No default route on %s; using default gateway.", iface);

        /* CANCEL re-selects (loop); BACK releases the display and ends the
           session so the main loop waits for the USB to be reconnected. */
        if (run_config_ui(iface, ip_oct, nm_oct, gw_oct) == UI_QUIT)
            return;
    }
}

int main(int argc, char **argv)
{
    /* volatile: these live across setjmp()/longjmp() in the reconnect loop,
       so they must not be cached in a register that a longjmp would clobber. */
    const char *volatile port_override = NULL;
    volatile speed_t baud = DEFAULT_BAUD;

    /* Usage: lcd_ipconfig [port|auto] [baud] [keys]
     *   port: serial device, e.g. /dev/ttyUSB1, or "auto" (default) to
     *         continuously auto-detect the display as it is plugged in.
     *   baud: 9600|19200|38400|57600|115200          (default 19200)
     *   keys:   run the keycode tester instead of the config UI.
     *   leds:   run the LED/GPO tester instead of the config UI.
     *   nolock: skip the unlock key sequence.
     * Arguments are matched by content, so order does not matter.
     *
     * The program runs as a resident poller: it waits for the display,
     * lights the LEDs green and serves the UI while connected, and returns
     * to waiting when the display is unplugged.
     */
    /* Log to syslog (LOG_PID so each line carries the pid); logmsg() also
       tees every message to the console. */
    openlog("lcd_ipconfig", LOG_PID, LOG_DAEMON);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "keys") == 0) {
            g_keytest = 1;
        } else if (strcmp(argv[i], "leds") == 0) {
            g_ledtest = 1;
        } else if (strcmp(argv[i], "nolock") == 0) {
            g_nolock = 1;
        } else if (strcmp(argv[i], "auto") == 0) {
            port_override = NULL;
        } else {
            speed_t b = baud_from_str(argv[i]);
            if (b != B0) baud = b;
            else         port_override = argv[i];
        }
    }

    /* Machine name, used as the on-screen title. */
    if (gethostname(g_hostname, sizeof(g_hostname)) != 0 || g_hostname[0] == '\0')
        snprintf(g_hostname, sizeof(g_hostname), "host");
    g_hostname[sizeof(g_hostname) - 1] = '\0';

    for (;;) {                       /* hot-plug reconnect loop */
        char dev_path[256];

        log_info("Waiting for display...");
        wait_for_device(port_override, dev_path, sizeof(dev_path));
        snprintf(g_dev_path, sizeof(g_dev_path), "%s", dev_path);
        log_info("Display detected on %s", dev_path);

        if (lcd_open(dev_path, baud) < 0) {
            log_err("Failed to open %s; retrying.", dev_path);
            if (fd_lcd >= 0) { close(fd_lcd); fd_lcd = -1; }
            g_dev_path[0] = '\0';
            msleep(1000);
            continue;
        }

        /* Let the bridge/display settle before sending init commands. */
        msleep(DEVICE_SETTLE_MS);

        if (setjmp(g_reconnect) == 0) {
            g_in_session = 1;
            run_session();           /* returns on normal end */
            g_in_session = 0;

            /* Normal end with the device still attached: tear down and wait
               for it to be unplugged so we don't immediately re-open it. */
            close(fd_lcd);
            fd_lcd = -1;
            while (g_dev_path[0] && access(g_dev_path, F_OK) == 0)
                msleep(500);
        } else {
            /* Display was unplugged mid-session (longjmp landed here). */
            g_in_session = 0;
            log_warn("Display disconnected.");
            if (fd_lcd >= 0) { close(fd_lcd); fd_lcd = -1; }
        }

        g_dev_path[0] = '\0';
    }

    return 0;
}
