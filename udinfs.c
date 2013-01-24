/*
This file is part of udinfs, a fuse interface to the USB IO devices

Copyright © 2013 John Hedges <john@drystone.co.uk>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include <syslog.h>
#include <stdarg.h>
#include <libgen.h>

#include "fuse.h"

#define MAX_RELAYS 8

static const char* _version = "0.1.1";

static struct {
    char * id;
    unsigned int num_relays;
} _device_info[] = {
    { "UDIN-8R 8 x Relay V1.0", 8 },
    { NULL, 0 }
};

typedef enum { relay_off = 0, relay_on } relay_state;

static struct {
    time_t mtime;
    relay_state state;
} _relays[MAX_RELAYS];

static time_t _ctime;
static time_t _mtime;
static unsigned int _num_relays;
static int _ttyfd = -1;
static char * _device_path = NULL;
static int _debug = 0;

static const char * _strerror() {
    static char buf[64];
    buf[0] = '\0';
    strerror_r(errno, buf, sizeof(buf));
    return buf;
}

static void _log(int level, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (_debug) {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    vsyslog(level, fmt, ap);
}

static int _relay_from_path(const char * path)
{
    if (path[0] == '/' && path[1] == 'r' && path[2] >= '1' && path[2] < (unsigned int)'1' + _num_relays && path[3] == '\0')
        return path[2] - '1';
    return -1;
}
        
static void _close_device()
{
    if (_ttyfd != -1)
        close(_ttyfd);
    _ttyfd = -1;
}

static int _read_device(char * buf, int size)
{
    int n, i;

    // try read for up to a second
    for (i = 0; i < 100; i++) {
        n = read(_ttyfd, buf, size);
        if (n >= 0 || errno != EAGAIN)
            break;
        usleep(10000);
    }

    if (n == -1) {
        _log(LOG_ERR, "Failed to read from UDIN device: %s", _strerror());
        return -1;
    }

    if (_debug) {
        fprintf(stderr, "%d bytes read from UDIN device: [", n);
        for (i = 0; i < n; i++)
            fprintf(stderr, "%d,", buf[i]);
        fputs("]\n", stderr);
    }
    
    if (n < 2 || buf[n-1] != '\n' || buf[n-2] != '\r') { // should at least have \r\n
        _log(LOG_ERR, "Short read from UDIN device");
        return -1;
    }

    buf[n-2] = '\0';
    return 0;
}

static int _write_device(const char * buf)
{
    int n = write(_ttyfd, buf, strlen(buf));
    if (n == -1) {
        _log(LOG_ERR, "Failed to write to UDIN device: %s", _strerror());
        return -1;
    }

    if (_debug) {
        int i;
        fprintf(stderr, "%d bytes write to UDIN device: [", n);
        for (i = 0; i < n; i++)
            fprintf(stderr, "%d,", buf[i]);
        fputs("]\n", stderr);
    }
    
    if (n < strlen(buf)) {
        _log(LOG_ERR, "Partial write: %d of %d written", n, (int)strlen(buf));
        return -1;
    }
    return 0;
}

static int _send_command(const char * command, char * response_buf, size_t bufsiz)
{
    char buf[64];
    int i;

    for (i = 0; command[i]; i++)
        buf[i] = command[i];
    buf[i++] = '\r';
    buf[i] = '\0';

    if (_write_device(buf) != 0)
        return -1;

    if (_read_device(buf, sizeof(buf)) != 0)
        return -1;

    if (strcmp(buf, command)) {
        _log(LOG_ERR, "UDIN device error: failed to echo command (%s != %s)", buf, command);
        return -1;
    }

    if (response_buf && _read_device(response_buf, bufsiz) != 0) {
        _log(LOG_ERR, "UDIN device error: failed to get response for command (%s)", command);
        return -1;
    }

    return 0;
}

static int _open_device() {
    struct termios tios;

    _ttyfd = open(_device_path, O_RDWR | O_NONBLOCK);
    if (_ttyfd == -1) {
        _log(LOG_ERR,"failed to open UDIN device %s: %s", _device_path, _strerror());
        return -1;
    }

    memset(&tios, 0, sizeof(tios));
    tios.c_cflag = B9600 | CLOCAL | CS8 | CREAD;
    tios.c_lflag = ICANON;
    tcflush(_ttyfd, TCIFLUSH);
    tcsetattr(_ttyfd, TCSANOW, &tios);

    return 0;
}

static void * _init(struct fuse_conn_info * conn)
{
    int i;
    char buf[64];

    memset(_relays, 0, sizeof(_relays));
    _num_relays = 0;

    if (_open_device() != 0)
        return NULL;

    if (_send_command("?", buf, sizeof(buf)) != 0) {
        _close_device();
        return NULL;
    }

    for (i = 0; _device_info[i].id; i++) {
        if (!strcmp(buf, _device_info[i].id)) {
            _num_relays = _device_info[i].num_relays;
            _log(LOG_INFO, "UDIN device identified as %s", _device_info[i].id);
            break;
        }
    }

    if (!_device_info[i].id) {
        _log(LOG_NOTICE, "Not a supported UDIN device at %s", _device_path);
        _close_device();
        return NULL;
    }

    // get current channel states
    _send_command("s0", buf, sizeof(buf));
    int all_state = atoi(buf);
    for (i = 0; i < _num_relays; i++)
        _relays[i].state = (all_state & (1 << i)) ? relay_on : relay_off;

    return NULL;
}

static void _switch_relay(int relay_num, relay_state s)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    if (_relays[relay_num].state != s) {
        pthread_mutex_lock( &mutex );
        char buf[64];
        buf[0] = (s == relay_on ? 'n' : 'f');
        buf[1] = '1' + relay_num;
        buf[2] = '\0';
        _send_command(buf, NULL, 0);
        _relays[relay_num].state = s;

        // check relay states and set _num_relays = 0 if inconsistent
        // this way the virtual relay files will disappear 
        _send_command("s0", buf, sizeof(buf));
        int i, all_state = atoi(buf);
        for (i = 0; i < _num_relays; i++) {
            int state = all_state & (1 << i);
            if ((_relays[i].state && !state) || (!_relays[i].state && state)) {
                _log(LOG_NOTICE, "Relay %d has inconsistent state", i+1);
                _num_relays = 0;
            }
        }
        pthread_mutex_unlock( &mutex );
   }
}

static int _getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));

    if (!strcmp(path, "/")) {
        stbuf->st_mode = S_IFDIR | 0775;
        stbuf->st_nlink = 2;
        stbuf->st_ctime = _ctime;
        stbuf->st_mtime = _mtime;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    int channel = _relay_from_path(path);
    if (channel >= 0) {
        stbuf->st_mode = S_IFREG | 0664;
        stbuf->st_nlink = 1;
        stbuf->st_size = 1;
        stbuf->st_ctime = _ctime;
        stbuf->st_mtime = _relays[channel].mtime;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    return -ENOENT;
}

static int _readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    if(strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int i;
    for (i = 0; i < _num_relays; i++) {
        char fnam[3];
        fnam[0] = 'r';
        fnam[1] = '1' + i;
        fnam[2] = '\0';
        filler(buf, fnam, NULL, 0);
    }

    return 0;
}

static int _open(const char *path, struct fuse_file_info *fi)
{
    return _relay_from_path(path) >= 0 ? 0 : -ENOENT;
}

static int _read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    int channel = _relay_from_path(path);

    if (channel < 0)
        return -ENOENT;

    if (!size || offset)
        return 0;

    *buf = _relays[channel].state ? '1' : '0';
    return 1;
}

static int _write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    int channel = _relay_from_path(path);

    if (channel < 0)
        return -ENOENT;

    if (!size || offset)
        return 0;

    _switch_relay(channel, *buf == '1' ? relay_on : relay_off);

    return size;
}

static void _destroy(void * nuttin)
{
    _close_device();
}
 
static int _chmod(const char * path, mode_t mode)
{
    return 0;
}

static int _chown(const char * path, uid_t uid, gid_t gid)
{
    return 0;
}

static int _utime(const char * path, struct utimbuf * t)
{
    return 0;
}

static int _truncate(const char* path, off_t o)
{
    return 0;
}

static struct fuse_operations _oper = {
    .getattr = _getattr,
    .readdir = _readdir,
    .open = _open,
    .write = _write,
    .read = _read,
    .init = _init,
    .destroy = _destroy,
    .chmod = _chmod,
    .chown = _chown,
    .utime = _utime,
    .truncate = _truncate,
};

int main(int argc, char *argv[])
{
    static const char * usage = "Usage: %s [fuse-opts] <udin-device-path> <mount-point>\n";

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);

    fuse_opt_add_arg(&args, argv[0]);
    int opt;
    while ((opt = getopt(argc, argv, "-vsdho:")) != -1) {
        switch(opt) {
        case 'v':
            printf("%s version %s\n", basename(argv[0]), _version);
            return 0;
        case 's':
            fuse_opt_add_arg(&args, "-s");
            break;
        case 'h':
            printf(usage, basename(argv[0]));
            break;
        case 'd':
            _debug = 1;
            fuse_opt_add_arg(&args, "-d");
            break;
        case 'o':
            fuse_opt_add_arg(&args, "-o");
            fuse_opt_add_arg(&args, optarg);
            break;
        case 1:
            // first bare argument is device, pass others on to fuse
            if (!_device_path)
                _device_path = optarg;
            else
                fuse_opt_add_arg(&args, optarg);
            break;
        }
    }

    if (_device_path)
        return fuse_main(args.argc, args.argv, &_oper, NULL);
    else {
        fprintf(stderr, usage, basename(argv[0]));
        return -1;
    }
}

