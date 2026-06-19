/*
 * lcd_ipconfig.c
 *
 * Matrix Orbital GLK19264A-7T-1U-USB (BGK19264A-7T)
 * Interactive IP address configuration via 7-key pad.
 *
 * Key mapping (Matrix Orbital default keycodes):
 *   Up      = 0x41 ('A')
 *   Down    = 0x42 ('B')
 *   Left    = 0x44 ('D')
 *   Right   = 0x43 ('C')
 *   Enter   = 0x45 ('E')
 *   Back    = 0x46 ('F')
 *   Cancel  = 0x47 ('G')
 *
 * Networking: writes a NetworkManager keyfile, then reloads
 * the connection via nmcli.
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -o lcd_ipconfig lcd_ipconfig.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <time.h>

 /* ------------------------------------------------------------------ */
 /* Configuration                                                        */
 /* ------------------------------------------------------------------ */

#define SERIAL_PORT     "/dev/ttyACM0"
#define INTERFACE_NAME  "ens160"
#define NETWORK_FILE    "/etc/NetworkManager/system-connections/ens160.nmconnection"

/* Display dimensions */
#define LCD_COLS        192
#define LCD_ROWS        64

/* ------------------------------------------------------------------ */
/* Matrix Orbital command helpers                                       */
/* ------------------------------------------------------------------ */

#define MO_CMD      0xFE

static int fd_lcd = -1;

static void lcd_write_raw(const uint8_t *buf, size_t len)
{
    ssize_t written = write(fd_lcd, buf, len);
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

/* ------------------------------------------------------------------ */
/* LCD initialisation                                                  */
/* ------------------------------------------------------------------ */

static int lcd_open(const char *port)
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

    /* USB virtual COM: baud rate doesn't really matter but set it anyway */
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

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

/* Key codes reported by GLK19264A-7T-1U default keymap              */
#define KEY_UP      0x41
#define KEY_DOWN    0x42
#define KEY_RIGHT   0x43
#define KEY_LEFT    0x44
#define KEY_ENTER   0x45
#define KEY_BACK    0x46
#define KEY_CANCEL  0x47

/*
 * Read one keypress. Returns the key byte, or 0 on timeout, -1 on error.
 * timeout_ms: 0 = block indefinitely.
 */
static int lcd_read_key(int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_lcd, &rfds);

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms > 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int ret = select(fd_lcd + 1, &rfds, NULL, NULL, ptv);
    if (ret < 0)  return -1;
    if (ret == 0) return 0;   /* timeout */

    uint8_t b;
    ssize_t n = read(fd_lcd, &b, 1);
    if (n <= 0) return -1;
    return (int)b;
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
/* Network configuration write                                         */
/* ------------------------------------------------------------------ */

static int write_network_file(const char* ip,
    const char* netmask,
    const char* gateway)
{
    /* Convert dotted-quad netmask to prefix length */
    struct in_addr nm;
    if (inet_pton(AF_INET, netmask, &nm) != 1) return -1;

    uint32_t bits = ntohl(nm.s_addr);
    int prefix = 0;
    while (bits & 0x80000000u) { prefix++; bits <<= 1; }

    FILE* f = fopen(NETWORK_FILE, "w");
    if (!f) { perror("fopen network file"); return -1; }

    fprintf(f,
        "[connection]\n"
        "id=ens160\n"
        "type=ethernet\n"
        "interface-name=%s\n\n"
        "[ipv4]\n"
        "method=manual\n"
        "addresses=%s/%d\n"
        "gateway=%s\n"
        "dns=8.8.8.8;\n\n"
        "[ipv6]\n"
        "method=ignore\n",
        INTERFACE_NAME, ip, prefix, gateway);

    fflush(f);
    fclose(f);

    /* NM keyfiles must be root-owned and 600 */
    chmod(NETWORK_FILE, 0600);

    return 0;
}


static int apply_network(void)
{
    /* Reload the keyfile from disk, then bring the connection up */
    if (system("nmcli connection reload") != 0) return -1;
    if (system("nmcli connection up ens160") != 0) return -1;
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

static void show_idle_screen(void)
{
    lcd_clear();
    lcd_set_cursor(1, 1);
    lcd_print("  Network Config");
    lcd_hline(0, 9, 191);
    lcd_set_cursor(1, 3);
    lcd_print("  Press ENTER to");
    lcd_set_cursor(1, 4);
    lcd_print("  configure IP");
    lcd_set_cursor(1, 6);
    lcd_print("  Interface: " INTERFACE_NAME);
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
    lcd_set_cursor(1, 6);
    lcd_print("ENTER=apply  CANCEL=abort");
}

int main(void)
{
    if (lcd_open(SERIAL_PORT) < 0) {
        fprintf(stderr, "Failed to open LCD on %s\n", SERIAL_PORT);
        return 1;
    }

    lcd_backlight_on();
    lcd_contrast(128);
    lcd_clear();

    /* Working copies of address fields */
    int ip_oct[4]  = {192, 168, 1, 100};
    int nm_oct[4]  = {255, 255, 255, 0};
    int gw_oct[4]  = {192, 168, 1, 1};

    char ip_str[16], nm_str[16], gw_str[16];

    AppState state = STATE_IDLE;
    show_idle_screen();

    while (1) {
        int key = lcd_read_key(500);  /* 500 ms poll */

        switch (state) {

        case STATE_IDLE:
            if (key == KEY_ENTER) {
                state = STATE_EDIT_IP;
                int r = ui_edit_ip("IP Address", ip_oct);
                if (r == 1) {
                    state = STATE_EDIT_NETMASK;
                    r = ui_edit_ip("Netmask", nm_oct);
                    if (r == 1) {
                        state = STATE_EDIT_GATEWAY;
                        r = ui_edit_ip("Gateway", gw_oct);
                        if (r == 1) {
                            state = STATE_CONFIRM;
                        }
                    }
                }
                if (state != STATE_CONFIRM) {
                    /* Any cancel/back path returns to idle */
                    state = STATE_IDLE;
                    show_idle_screen();
                    break;
                }

                /* Fall through to confirm */
                octets_to_str(ip_oct, ip_str, sizeof(ip_str));
                octets_to_str(nm_oct, nm_str, sizeof(nm_str));
                octets_to_str(gw_oct, gw_str, sizeof(gw_str));
                show_confirm_screen(ip_str, nm_str, gw_str);
            }
            break;

        case STATE_CONFIRM:
            if (key == KEY_ENTER) {
                state = STATE_APPLY;

                lcd_clear();
                lcd_set_cursor(1, 3);
                lcd_print("  Applying...");

                int ok = 0;
                if (valid_ipv4(ip_str) && valid_ipv4(nm_str) && valid_ipv4(gw_str)) {
                    if (write_network_file(ip_str, nm_str, gw_str) == 0) {
                        if (apply_network() == 0) ok = 1;
                    }
                }

                if (ok) {
                    ui_message("  Config applied!", "  Network restarted.");
                } else {
                    ui_message("  ERROR", "  See syslog.");
                }

                state = STATE_IDLE;
                show_idle_screen();

            } else if (key == KEY_CANCEL || key == KEY_BACK) {
                state = STATE_IDLE;
                show_idle_screen();
            }
            break;

        /* These states are driven inline from STATE_IDLE above */
        default:
            state = STATE_IDLE;
            show_idle_screen();
            break;
        }
    }

    close(fd_lcd);
    return 0;
}
