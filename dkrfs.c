/*
This file is part of dkrfs, a fuse interface to denkovi DAEnetIP2

Copyright © 2015 John Hedges <john@drystone.co.uk>

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
#include <errno.h>
#include <pthread.h>
#include <libgen.h>

#include "fuse.h"
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#define MAX_RELAYS 16

static const char* _version = "0.1.1";

static time_t _start_time;

typedef enum { relay_off, relay_on } relay_state;

enum {
    KEY_NUM_RELAYS,
    KEY_COMMUNITY,
    KEY_HELP,
    KEY_VERSION
};

static struct fuse_opt options[] = {
    FUSE_OPT_KEY("-r %u",          KEY_NUM_RELAYS),
    FUSE_OPT_KEY("relays=%u",      KEY_NUM_RELAYS),
    FUSE_OPT_KEY("-c %s",          KEY_COMMUNITY),
    FUSE_OPT_KEY("community=%s",   KEY_COMMUNITY),
    FUSE_OPT_KEY("-V",             KEY_VERSION),
    FUSE_OPT_KEY("--version",      KEY_VERSION),
    FUSE_OPT_KEY("-h",             KEY_HELP),
    FUSE_OPT_KEY("--help",         KEY_HELP),
    FUSE_OPT_END
};

static unsigned int _num_relays = 16;
static char * _peername = NULL;
static char * _community = NULL;

static struct snmp_session * _snmp_session;

static struct {
    oid id[MAX_OID_LEN];
    size_t len;
} _oids[MAX_RELAYS];

static int _relay_from_path(const char * path)
{
    if (path[0] == '/' && path[1] == 'r' && path[2] >= '1' && path[2] <= '9') {
        char *e;
        long n = strtol(path + 2, &e, 10);
        if (*e == '\0' && n >= 1 && n <= _num_relays)
            return (int)n - 1;
    }
    return -1;
}
        
static void * _init(struct fuse_conn_info * conn)
{
    return NULL;
}

static int _snmp_synch(struct snmp_pdu * pdu, relay_state * s) {
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    int ret = 0;
    struct snmp_pdu * response = NULL;

    pthread_mutex_lock( &mutex );
    int status = snmp_synch_response(_snmp_session, pdu, &response);
    pthread_mutex_unlock( &mutex );

    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        ret = 1;
        if (s)
            *s = *response->variables->val.integer == 0 ? relay_off : relay_on;
    }

    if (response)
        snmp_free_pdu(response);

    return ret;
}

static int _set_relay(int relay_num, relay_state s)
{
    struct snmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_SET);
    long v = s == relay_on ? 1 : 0;
    snmp_pdu_add_variable(pdu, _oids[relay_num].id, _oids[relay_num].len, ASN_INTEGER, &v, 1);
    return _snmp_synch(pdu, NULL);
}

static int _get_relay(int relay_num, relay_state * s)
{
    struct snmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_GET);
    snmp_add_null_var(pdu, _oids[relay_num].id, _oids[relay_num].len);
    return _snmp_synch(pdu, s);
}

static int _getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));

    if (!strcmp(path, "/")) {
        stbuf->st_mode = S_IFDIR | 0775;
        stbuf->st_nlink = 2;
        stbuf->st_ctime = _start_time;
        stbuf->st_mtime = _start_time;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    int channel = _relay_from_path(path);
    if (channel >= 0) {
        stbuf->st_mode = S_IFREG | 0664;
        stbuf->st_nlink = 1;
        stbuf->st_size = 1;
        stbuf->st_ctime = _start_time;
        stbuf->st_mtime = time(NULL);   // use current time as we can't assume we were last to switch
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
        char fnam[16];
        sprintf(fnam, "r%d", i + 1);
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

    relay_state s;
    if (_get_relay(channel, &s)) {
        *buf = s == relay_on ? '1' : '0';
        return 1;
    }

    return -EIO;
}

static int _write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    int channel = _relay_from_path(path);

    if (channel < 0)
        return -ENOENT;

    if (!size || offset)
        return 0;

    _set_relay(channel, *buf == '1' ? relay_on : relay_off);

    return size;
}

static void _destroy(void * nuttin)
{
    snmp_close(_snmp_session);
    free(_community);
    free(_peername);
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

static void usage(const char * progname) {
    printf("Usage: %s [fuse-opts] -c community -n num_relays <device-address> <mount-point>\n", progname);
}

static int opt_proc(void * data, const char * arg, int key, struct fuse_args * outargs)
{
    switch (key) {
    case FUSE_OPT_KEY_NONOPT:
        if (_peername == NULL) {
            _peername = strdup(arg);
            return 0;
        }
        return 1;

    case KEY_NUM_RELAYS:
        if (arg[0] == '-')
            _num_relays = atoi(arg + 2);
        else
            _num_relays = atoi(strchr(arg, '=') + 1);

        if (_num_relays > MAX_RELAYS)
            _num_relays = MAX_RELAYS;
        return 0;

    case KEY_COMMUNITY:
        free(_community);
        if (arg[0] == '-')
            _community = strdup(arg + 2);
        else
            _community = strdup(strchr(arg, '=') + 1);
        return 0;

    case KEY_HELP:
        usage(outargs->argv[0]);
        fuse_opt_add_arg(outargs, "-ho");
        fuse_main(outargs->argc, outargs->argv, &_oper, NULL);
        exit(1);

    case KEY_VERSION:
        printf("dkrfs version %s\n", _version);
        fuse_opt_add_arg(outargs, "--version");
        fuse_main(outargs->argc, outargs->argv, &_oper, NULL);
        exit(0); 
    }

    return 1;
}

int main(int argc, char *argv[])
{

    _start_time = time(NULL);

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    fuse_opt_parse(&args, NULL, options, opt_proc);

    if (_peername && _community) {
        unsigned int i;
        struct snmp_session sess;

        init_snmp(basename(argv[0]));
        snmp_sess_init(&sess);
        sess.peername = _peername;
        sess.version = SNMP_VERSION_1;
        sess.community = (unsigned char *)_community;
        sess.community_len = strlen(_community);
        _snmp_session = snmp_open(&sess);
        if(!_snmp_session)
            return -1;

        for (i = 0; i < _num_relays; i++) {
            char buf[64];
            sprintf(buf, ".1.3.6.1.4.1.19865.1.2.%d.%d.0", i / 8 + 1, i % 8 + 1);
            _oids[i].len = MAX_OID_LEN;
            read_objid(buf, _oids[i].id, &_oids[i].len);
        }

        return fuse_main(args.argc, args.argv, &_oper, NULL);
    } else {
        usage(argv[0]);
        return -1;
    }
}

