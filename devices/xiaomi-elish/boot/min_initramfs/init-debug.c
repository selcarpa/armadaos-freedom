/*
 * Minimal elish M2 maintenance-init canary.
 *
 * This is intentionally not dracut: the elish kernel has the SM8250 display,
 * UFS and fbdev paths built in, so the first M2 goal is to prove that the
 * kShimC -> Linux handoff reaches a userspace init and that the CSOT panel can
 * show a recognisable pattern.  It mounts devtmpfs/proc/sys, logs a fixed
 * marker, paints the framebuffer in colour bands and then idles forever.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CANARY "ARMADA-ELISH-M2-CANARY"

static int kmsg_fd = -1;
static int console_fd = -1;
static int usb_fd = -1;

static void log_line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    if (kmsg_fd >= 0)
        (void)write(kmsg_fd, buf, (size_t)n);
    if (console_fd >= 0)
        (void)write(console_fd, buf, (size_t)n);
    if (usb_fd >= 0)
        (void)write(usb_fd, buf, (size_t)n);
}

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        log_line("canary: mkdir %s: %s\n", path, strerror(errno));
}

static void mount_one(const char *source, const char *target,
                      const char *fstype, unsigned long flags)
{
    if (mount(source, target, fstype, flags, NULL) < 0)
        log_line("canary: mount %s on %s: %s\n", fstype, target,
                 strerror(errno));
}

static void open_console(void)
{
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    console_fd = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (console_fd < 0)
        console_fd = open("/dev/tty0", O_WRONLY | O_CLOEXEC);
}

static uint32_t pack_pixel(const struct fb_var_screeninfo *var,
                           uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel = 0;
    uint32_t rv, gv, bv;

    if (var->bits_per_pixel == 16) {
        rv = (uint32_t)r * 31 / 255;
        gv = (uint32_t)g * 63 / 255;
        bv = (uint32_t)b * 31 / 255;
        pixel = (rv << var->red.offset) | (gv << var->green.offset) |
                (bv << var->blue.offset);
    } else if (var->bits_per_pixel == 32) {
        rv = (uint32_t)r * ((1u << var->red.length) - 1) / 255;
        gv = (uint32_t)g * ((1u << var->green.length) - 1) / 255;
        bv = (uint32_t)b * ((1u << var->blue.length) - 1) / 255;
        pixel = (rv << var->red.offset) | (gv << var->green.offset) |
                (bv << var->blue.offset);
    } else {
        rv = r >> (8 - var->red.length);
        gv = g >> (8 - var->green.length);
        bv = b >> (8 - var->blue.length);
        pixel = (rv << var->red.offset) | (gv << var->green.offset) |
                (bv << var->blue.offset);
    }
    return pixel;
}

static void put_rect(uint8_t *fb, const struct fb_var_screeninfo *var,
                     const struct fb_fix_screeninfo *fix,
                     int x0, int y0, int x1, int y1,
                     uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel = pack_pixel(var, r, g, b);
    int bpp = (int)var->bits_per_pixel / 8;
    int x, y;

    if (bpp <= 0)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)var->xres) x1 = (int)var->xres;
    if (y1 > (int)var->yres) y1 = (int)var->yres;
    for (y = y0; y < y1; y++) {
        uint8_t *row = fb + (size_t)y * fix->line_length + (size_t)x0 * bpp;
        for (x = x0; x < x1; x++) {
            if (bpp == 4)
                *(uint32_t *)row = pixel;
            else if (bpp == 2)
                *(uint16_t *)row = (uint16_t)pixel;
            else if (bpp == 3) {
                /* Conservative 24-bit path; offset order follows the DT. */
                row[0] = (uint8_t)(pixel & 0xff);
                row[1] = (uint8_t)((pixel >> 8) & 0xff);
                row[2] = (uint8_t)((pixel >> 16) & 0xff);
            } else if (bpp == 1)
                *row = (uint8_t)pixel;
            row += bpp;
        }
    }
}

static int paint_framebuffer(void)
{
    const char *paths[] = { "/dev/fb0", "/dev/graphics/fb0", NULL };
    int fd = -1;
    int i;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    size_t map_len;
    uint8_t *fb;

    for (i = 0; paths[i] != NULL; i++) {
        fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0)
            break;
    }
    if (fd < 0) {
        log_line("canary: framebuffer missing (last error: %s)\n",
                 strerror(errno));
        return -1;
    }
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        log_line("canary: framebuffer ioctl failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (var.bits_per_pixel < 8 || fix.line_length == 0) {
        log_line("canary: unsupported fb bpp=%u line=%u\n",
                 var.bits_per_pixel, fix.line_length);
        close(fd);
        return -1;
    }
    map_len = (size_t)fix.line_length * var.yres;
    fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        log_line("canary: framebuffer mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    log_line("canary: fb %ux%u bpp=%u stride=%u\n", var.xres, var.yres,
             var.bits_per_pixel, fix.line_length);

    /* Three unmistakable bands plus a white centre block.  Rotation does not
     * hide the pattern even if the panel is mounted 90/270 degrees. */
    put_rect(fb, &var, &fix, 0, 0, (int)var.xres, (int)var.yres / 3,
             220, 20, 20);
    put_rect(fb, &var, &fix, 0, (int)var.yres / 3,
             (int)var.xres, (int)(var.yres * 2 / 3), 20, 200, 20);
    put_rect(fb, &var, &fix, 0, (int)(var.yres * 2 / 3),
             (int)var.xres, (int)var.yres, 20, 40, 220);
    put_rect(fb, &var, &fix, (int)var.xres / 2 - (int)var.xres / 10,
             (int)var.yres / 2 - (int)var.yres / 10,
             (int)var.xres / 2 + (int)var.xres / 10,
             (int)var.yres / 2 + (int)var.yres / 10, 255, 255, 255);
    msync(fb, map_len, MS_SYNC);
    munmap(fb, map_len);
    close(fd);
    return 0;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0)
            return;
        buf += n;
        len -= (size_t)n;
    }
}

static void dump_file(int out, const char *path, size_t limit)
{
    char buf[4096];
    size_t total = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) {
        log_line("debug: open %s: %s\n", path, strerror(errno));
        return;
    }
    log_line("debug: --- %s ---\n", path);
    while (total < limit && (n = read(fd, buf, sizeof(buf))) > 0) {
        size_t take = (size_t)n;
        if (take > limit - total)
            take = limit - total;
        write_all(out, buf, take);
        total += take;
    }
    close(fd);
    if (total == 0)
        log_line("debug: (empty)\n");
    else if (total > 0 && buf[0] != '\n')
        write_all(out, "\n", 1);
}

static void dump_dir(int out, const char *path, size_t limit)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    char full[512];
    if (dir == NULL) {
        log_line("debug: opendir %s: %s\n", path, strerror(errno));
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(full, sizeof(full), "%s/%s", path, entry->d_name)
                >= (int)sizeof(full))
            continue;
        dump_file(out, full, limit);
    }
    closedir(dir);
}

static void setup_usb_acm(void)
{
    char udc[256] = { 0 };
    DIR *dir;
    struct dirent *entry;
    int fd;

    ensure_dir("/sys/kernel/config");
    mount_one("configfs", "/sys/kernel/config", "configfs", 0);
    ensure_dir("/sys/kernel/config/usb_gadget");
    ensure_dir("/sys/kernel/config/usb_gadget/g1");
    ensure_dir("/sys/kernel/config/usb_gadget/g1/strings/0x409");
    ensure_dir("/sys/kernel/config/usb_gadget/g1/configs/c.1");
    ensure_dir("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409");
    ensure_dir("/sys/kernel/config/usb_gadget/g1/functions/acm.usb0");

    write_text("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b");
    write_text("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0104");
    write_text("/sys/kernel/config/usb_gadget/g1/bcdUSB", "0x0200");
    write_text("/sys/kernel/config/usb_gadget/g1/bcdDevice", "0x0100");
    write_text("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer",
               "ArmadaOS");
    write_text("/sys/kernel/config/usb_gadget/g1/strings/0x409/product",
               "elish M2 debug");
    write_text("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber",
               "elish-m2-debug");
    write_text("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/"
               "configuration", "ACM debug console");
    if (symlink("../../functions/acm.usb0",
                "/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.usb0") < 0 &&
        errno != EEXIST)
        log_line("debug: acm symlink: %s\n", strerror(errno));

    dir = opendir("/sys/class/udc");
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            snprintf(udc, sizeof(udc), "%s", entry->d_name);
            break;
        }
        closedir(dir);
    }
    if (udc[0] == '\0') {
        log_line("debug: no UDC available\n");
        return;
    }
    log_line("debug: binding USB ACM to %s\n", udc);
    if (write_text("/sys/kernel/config/usb_gadget/g1/UDC", udc) < 0)
        log_line("debug: UDC write: %s\n", strerror(errno));

    for (int i = 0; i < 50; ++i) {
        fd = open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            usb_fd = fd;
            log_line("debug: USB ACM ready at /dev/ttyGS0\n");
            return;
        }
        usleep(100000);
    }
    log_line("debug: /dev/ttyGS0 did not appear\n");
}

static long read_long(const char *path)
{
    char buf[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtol(buf, NULL, 0);
}

static void wake_backlight(void)
{
    DIR *dir = opendir("/sys/class/backlight");
    struct dirent *entry;
    char path[512];
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        long max;
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness",
                     entry->d_name) >= (int)sizeof(path))
            continue;
        max = read_long(path);
        if (max <= 0)
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power",
                     entry->d_name) < (int)sizeof(path))
            write_text(path, "0");
        if (snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
                     entry->d_name) < (int)sizeof(path)) {
            char value[32];
            snprintf(value, sizeof(value), "%ld", max);
            if (write_text(path, value) == 0)
                log_line("debug: backlight %s set to %ld\n", entry->d_name, max);
            else
                log_line("debug: backlight %s write failed: %s\n",
                         entry->d_name, strerror(errno));
        }
    }
    closedir(dir);
}

static unsigned parse_reboot_after(const char *cmdline)
{
    const char *p = strstr(cmdline, "armada.reboot_after=");
    if (p == NULL)
        return 0;
    return (unsigned)strtoul(p + strlen("armada.reboot_after="), NULL, 0);
}

static void dump_diagnostics(void)
{
    int out = usb_fd;
    if (out < 0)
        out = console_fd >= 0 ? console_fd : 1;

    log_line("%s: debug init reached\n", CANARY);
    log_line("%s: dumping early state\n", CANARY);
    dump_file(out, "/proc/version", 8192);
    dump_file(out, "/proc/cmdline", 8192);
    dump_file(out, "/proc/device-tree/model", 8192);
    dump_file(out, "/proc/device-tree/compatible", 8192);
    dump_file(out, "/proc/meminfo", 65536);
    dump_file(out, "/proc/interrupts", 131072);

    ensure_dir("/sys/fs/pstore");
    mount_one("pstore", "/sys/fs/pstore", "pstore", 0);
    dump_dir(out, "/sys/fs/pstore", 1024 * 1024);

    dump_dir(out, "/sys/class/drm", 65536);
    dump_dir(out, "/sys/class/backlight", 8192);
    dump_dir(out, "/sys/class/graphics", 8192);
    dump_dir(out, "/sys/class/udc", 8192);
    dump_file(out, "/sys/kernel/debug/devices_deferred", 65536);
    log_line("%s: debug dump complete\n", CANARY);
}

int main(void)
{
    char cmdline[1024] = { 0 };
    char buf[4096];
    int fd, kfd;
    ssize_t n;

    ensure_dir("/dev");
    ensure_dir("/proc");
    ensure_dir("/sys");
    ensure_dir("/sys/kernel");
    ensure_dir("/sys/kernel/debug");
    ensure_dir("/sys/fs");
    ensure_dir("/sys/fs/pstore");
    ensure_dir("/tmp");
    ensure_dir("/run");

    mount_one("devtmpfs", "/dev", "devtmpfs", 0);
    mount_one("proc", "/proc", "proc", 0);
    mount_one("sysfs", "/sys", "sysfs", 0);
    mount_one("debugfs", "/sys/kernel/debug", "debugfs", 0);
    open_console();

    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (n > 0) {
            while (n > 0 && (cmdline[n - 1] == '\n' || cmdline[n - 1] == '\r'))
                cmdline[--n] = '\0';
        }
    }
    log_line("%s: userspace init reached\n", CANARY);
    log_line("%s: cmdline=%s\n", CANARY, cmdline[0] ? cmdline : "(empty)");

    setup_usb_acm();
    wake_backlight();
    dump_diagnostics();

    log_line("%s: painting framebuffer canary\n", CANARY);
    if (paint_framebuffer() == 0)
        log_line("%s: framebuffer canary active\n", CANARY);
    else
        log_line("%s: framebuffer canary unavailable\n", CANARY);

    unsigned reboot_after = parse_reboot_after(cmdline);
    if (reboot_after > 0) {
        log_line("%s: rebooting in %u s\n", CANARY, reboot_after);
        sleep(reboot_after);
        sync();
        log_line("%s: rebooting to bootloader-selected OS\n", CANARY);
        reboot(RB_AUTOBOOT);
    }

    /* Forward new kernel messages to the USB ACM console if it enumerated.
     * /dev/kmsg is non-blocking; EAGAIN/EPIPE are expected on a fresh read. */
    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    for (;;) {
        if (kfd >= 0) {
            while ((n = read(kfd, buf, sizeof(buf))) > 0)
                write_all(usb_fd >= 0 ? usb_fd : console_fd, buf, (size_t)n);
        }
        sleep(1);
    }
    return 0;
}
