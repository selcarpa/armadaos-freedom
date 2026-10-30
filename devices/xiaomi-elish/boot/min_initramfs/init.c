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

int main(void)
{
    char cmdline[1024] = { 0 };
    int fd;
    ssize_t n;

    ensure_dir("/dev");
    ensure_dir("/proc");
    ensure_dir("/sys");
    ensure_dir("/tmp");
    ensure_dir("/run");

    mount_one("devtmpfs", "/dev", "devtmpfs", 0);
    mount_one("proc", "/proc", "proc", 0);
    mount_one("sysfs", "/sys", "sysfs", 0);
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
    log_line("%s: painting framebuffer canary\n", CANARY);
    if (paint_framebuffer() == 0)
        log_line("%s: framebuffer canary active; idling\n", CANARY);
    else
        log_line("%s: framebuffer canary unavailable; console marker only\n",
                 CANARY);

    /* Keep the canary visible.  fastboot boot is non-destructive; holding the
     * power button is the intended way out of this diagnostic init. */
    for (;;)
        pause();
    return 0;
}
