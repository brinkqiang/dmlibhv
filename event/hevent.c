#include "hevent.h"
#include "hsocket.h"
#include "hatomic.h"
#include "hlog.h"

uint64_t hloop_next_event_id() {
    static hatomic_t s_id = HATOMIC_VAR_INIT(0);
    return ++s_id;
}

uint32_t hio_next_id() {
    static hatomic_t s_id = HATOMIC_VAR_INIT(0);
    return ++s_id;
}

static void fill_io_type(hio_t* io) {
    int type = 0;
    socklen_t optlen = sizeof(int);
    int ret = getsockopt(io->fd, SOL_SOCKET, SO_TYPE, (char*)&type, &optlen);
    printd("getsockopt SO_TYPE fd=%d ret=%d type=%d errno=%d\n", io->fd, ret, type, socket_errno());
    if (ret == 0) {
        switch (type) {
        case SOCK_STREAM:   io->io_type = HIO_TYPE_TCP; break;
        case SOCK_DGRAM:    io->io_type = HIO_TYPE_UDP; break;
        case SOCK_RAW:      io->io_type = HIO_TYPE_IP;  break;
        default: io->io_type = HIO_TYPE_SOCKET;         break;
        }
    }
    else if (socket_errno() == ENOTSOCK) {
        switch (io->fd) {
        case 0: io->io_type = HIO_TYPE_STDIN;   break;
        case 1: io->io_type = HIO_TYPE_STDOUT;  break;
        case 2: io->io_type = HIO_TYPE_STDERR;  break;
        default: io->io_type = HIO_TYPE_FILE;   break;
        }
    }
    else {
        io->io_type = HIO_TYPE_TCP;
    }
}

static void hio_socket_init(hio_t* io) {
    if (io->io_type & HIO_TYPE_SOCK_RAW || io->io_type & HIO_TYPE_SOCK_DGRAM) {
        // NOTE: sendto multiple peeraddr cannot use io->write_queue
        blocking(io->fd);
    } else {
        nonblocking(io->fd);
    }
    // fill io->localaddr io->peeraddr
    if (io->localaddr == NULL) {
        HV_ALLOC(io->localaddr, sizeof(sockaddr_u));
    }
    if (io->peeraddr == NULL) {
        HV_ALLOC(io->peeraddr, sizeof(sockaddr_u));
    }
    socklen_t addrlen = sizeof(sockaddr_u);
    int ret = getsockname(io->fd, io->localaddr, &addrlen);
    printd("getsockname fd=%d ret=%d errno=%d\n", io->fd, ret, socket_errno());
    // NOTE: udp peeraddr set by recvfrom/sendto
    if (io->io_type & HIO_TYPE_SOCK_STREAM) {
        addrlen = sizeof(sockaddr_u);
        ret = getpeername(io->fd, io->peeraddr, &addrlen);
        printd("getpeername fd=%d ret=%d errno=%d\n", io->fd, ret, socket_errno());
    }
}

void hio_init(hio_t* io) {
    // alloc localaddr,peeraddr when hio_socket_init
    /*
    if (io->localaddr == NULL) {
        HV_ALLOC(io->localaddr, sizeof(sockaddr_u));
    }
    if (io->peeraddr == NULL) {
        HV_ALLOC(io->peeraddr, sizeof(sockaddr_u));
    }
    */

    // write_queue init when hwrite try_write failed
    // write_queue_init(&io->write_queue, 4);

    hrecursive_mutex_init(&io->write_mutex);
}

void hio_ready(hio_t* io) {
    if (io->ready) return;
    // flags
    io->ready = 1;
    io->closed = 0;
    io->accept = io->connect = io->connectex = 0;
    io->recv = io->send = 0;
    io->recvfrom = io->sendto = 0;
    io->close = 0;
    // public:
    io->id = hio_next_id();
    io->io_type = HIO_TYPE_UNKNOWN;
    io->error = 0;
    io->events = io->revents = 0;
    // readbuf
    io->alloced_readbuf = 0;
    io->readbuf.base = io->loop->readbuf.base;
    io->readbuf.len = io->loop->readbuf.len;
    io->readbuf.offset = 0;
    io->read_once = 0;
    io->read_until = 0;
    io->small_readbytes_cnt = 0;
    // write_queue
    io->write_queue_bytes = 0;
    // callbacks
    io->read_cb = NULL;
    io->write_cb = NULL;
    io->close_cb = NULL;
    io->accept_cb = NULL;
    io->connect_cb = NULL;
    // timers
    io->connect_timeout = 0;
    io->connect_timer = NULL;
    io->close_timeout = 0;
    io->close_timer = NULL;
    io->keepalive_timeout = 0;
    io->keepalive_timer = NULL;
    io->heartbeat_interval = 0;
    io->heartbeat_fn = NULL;
    io->heartbeat_timer = NULL;
    // upstream
    io->upstream_io = NULL;
    // unpack
    io->unpack_setting = NULL;
    // ssl
    io->ssl = NULL;
    // context
    io->ctx = NULL;
    // private:
#if defined(EVENT_POLL) || defined(EVENT_KQUEUE)
    io->event_index[0] = io->event_index[1] = -1;
#endif
#ifdef EVENT_IOCP
    io->hovlp = NULL;
#endif

    // io_type
    fill_io_type(io);
    if (io->io_type & HIO_TYPE_SOCKET) {
        hio_socket_init(io);
    }

#if WITH_RUDP
    if (io->io_type & HIO_TYPE_SOCK_RAW || io->io_type & HIO_TYPE_SOCK_DGRAM) {
        rudp_init(&io->rudp);
    }
#endif
}

void hio_done(hio_t* io) {
    if (!io->ready) return;
    io->ready = 0;

    hio_del(io, HV_RDWR);

    // readbuf
    hio_free_readbuf(io);

    // write_queue
    offset_buf_t* pbuf = NULL;
    hrecursive_mutex_lock(&io->write_mutex);
    while (!write_queue_empty(&io->write_queue)) {
        pbuf = write_queue_front(&io->write_queue);
        HV_FREE(pbuf->base);
        write_queue_pop_front(&io->write_queue);
    }
    write_queue_cleanup(&io->write_queue);
    hrecursive_mutex_unlock(&io->write_mutex);

#if WITH_RUDP
    if (io->io_type & HIO_TYPE_SOCK_RAW || io->io_type & HIO_TYPE_SOCK_DGRAM) {
        rudp_cleanup(&io->rudp);
    }
#endif
}

void hio_free(hio_t* io) {
    if (io == NULL) return;
    hio_close(io);
    hrecursive_mutex_destroy(&io->write_mutex);
    HV_FREE(io->localaddr);
    HV_FREE(io->peeraddr);
    HV_FREE(io);
}

bool hio_is_opened(hio_t* io) {
    if (io == NULL) return false;
    return io->ready == 1 && io->closed == 0;
}

bool hio_is_closed(hio_t* io) {
    if (io == NULL) return true;
    return io->ready == 0 && io->closed == 1;
}

uint32_t hio_id (hio_t* io) {
    return io->id;
}

int hio_fd(hio_t* io) {
    return io->fd;
}

hio_type_e hio_type(hio_t* io) {
    return io->io_type;
}

int hio_error(hio_t* io) {
    return io->error;
}

int hio_events(hio_t* io) {
    return io->events;
}

int hio_revents(hio_t* io) {
    return io->revents;
}

struct sockaddr* hio_localaddr(hio_t* io) {
    return io->localaddr;
}

struct sockaddr* hio_peeraddr(hio_t* io) {
    return io->peeraddr;
}

void hio_set_context(hio_t* io, void* ctx) {
    io->ctx = ctx;
}

void* hio_context(hio_t* io) {
    return io->ctx;
}

size_t hio_read_bufsize(hio_t* io) {
    return io->readbuf.len;
}

size_t hio_write_bufsize(hio_t* io) {
    return io->write_queue_bytes;
}

haccept_cb hio_getcb_accept(hio_t* io) {
    return io->accept_cb;
}

hconnect_cb hio_getcb_connect(hio_t* io) {
    return io->connect_cb;
}

hread_cb hio_getcb_read(hio_t* io) {
    return io->read_cb;
}

hwrite_cb hio_getcb_write(hio_t* io) {
    return io->write_cb;
}

hclose_cb hio_getcb_close(hio_t* io) {
    return io->close_cb;
}

void hio_setcb_accept(hio_t* io, haccept_cb accept_cb) {
    io->accept_cb = accept_cb;
}

void hio_setcb_connect(hio_t* io, hconnect_cb connect_cb) {
    io->connect_cb = connect_cb;
}

void hio_setcb_read(hio_t* io, hread_cb read_cb) {
    io->read_cb = read_cb;
}

void hio_setcb_write(hio_t* io, hwrite_cb write_cb) {
    io->write_cb = write_cb;
}

void hio_setcb_close(hio_t* io, hclose_cb close_cb) {
    io->close_cb = close_cb;
}

void hio_accept_cb(hio_t* io) {
    /*
    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN] = {0};
    printd("accept connfd=%d [%s] <= [%s]\n", io->fd,
            SOCKADDR_STR(io->localaddr, localaddrstr),
            SOCKADDR_STR(io->peeraddr, peeraddrstr));
    */
    if (io->accept_cb) {
        // printd("accept_cb------\n");
        io->accept_cb(io);
        // printd("accept_cb======\n");
    }
}

void hio_connect_cb(hio_t* io) {
    /*
    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN] = {0};
    printd("connect connfd=%d [%s] => [%s]\n", io->fd,
            SOCKADDR_STR(io->localaddr, localaddrstr),
            SOCKADDR_STR(io->peeraddr, peeraddrstr));
    */
    if (io->connect_cb) {
        // printd("connect_cb------\n");
        io->connect_cb(io);
        // printd("connect_cb======\n");
    }
}

void hio_read_cb(hio_t* io, void* buf, int len) {
    if (io->read_cb) {
        // printd("read_cb------\n");
        io->read_cb(io, buf, len);
        // printd("read_cb======\n");
    }

    // for readbuf autosize
    if (hio_is_alloced_readbuf(io) && io->readbuf.len > READ_BUFSIZE_HIGH_WATER) {
        size_t small_size = io->readbuf.len / 2;
        if (len < small_size) {
            ++io->small_readbytes_cnt;
        } else {
            io->small_readbytes_cnt = 0;
        }
    }
}

void hio_write_cb(hio_t* io, const void* buf, int len) {
    if (io->write_cb) {
        // printd("write_cb------\n");
        io->write_cb(io, buf, len);
        // printd("write_cb======\n");
    }
}

void hio_close_cb(hio_t* io) {
    if (io->close_cb) {
        // printd("close_cb------\n");
        io->close_cb(io);
        // printd("close_cb======\n");
    }
}

void hio_set_type(hio_t* io, hio_type_e type) {
    io->io_type = type;
}

void hio_set_localaddr(hio_t* io, struct sockaddr* addr, int addrlen) {
    if (io->localaddr == NULL) {
        HV_ALLOC(io->localaddr, sizeof(sockaddr_u));
    }
    memcpy(io->localaddr, addr, addrlen);
}

void hio_set_peeraddr (hio_t* io, struct sockaddr* addr, int addrlen) {
    if (io->peeraddr == NULL) {
        HV_ALLOC(io->peeraddr, sizeof(sockaddr_u));
    }
    memcpy(io->peeraddr, addr, addrlen);
}

int hio_enable_ssl(hio_t* io) {
    io->io_type = HIO_TYPE_SSL;
    return 0;
}

bool hio_is_ssl(hio_t* io) {
    return io->io_type == HIO_TYPE_SSL;
}

hssl_t hio_get_ssl(hio_t* io) {
    return io->ssl;
}

int hio_set_ssl(hio_t* io, hssl_t ssl) {
    io->io_type = HIO_TYPE_SSL;
    io->ssl = ssl;
    return 0;
}

void hio_set_readbuf(hio_t* io, void* buf, size_t len) {
    assert(io && buf && len != 0);
    hio_free_readbuf(io);
    io->readbuf.base = (char*)buf;
    io->readbuf.len = len;
    io->readbuf.offset = 0;
    io->alloced_readbuf = 0;
}

void hio_del_connect_timer(hio_t* io) {
    if (io->connect_timer) {
        htimer_del(io->connect_timer);
        io->connect_timer = NULL;
        io->connect_timeout = 0;
    }
}

void hio_del_close_timer(hio_t* io) {
    if (io->close_timer) {
        htimer_del(io->close_timer);
        io->close_timer = NULL;
        io->close_timeout = 0;
    }
}

void hio_del_keepalive_timer(hio_t* io) {
    if (io->keepalive_timer) {
        htimer_del(io->keepalive_timer);
        io->keepalive_timer = NULL;
        io->keepalive_timeout = 0;
    }
}

void hio_del_heartbeat_timer(hio_t* io) {
    if (io->heartbeat_timer) {
        htimer_del(io->heartbeat_timer);
        io->heartbeat_timer = NULL;
        io->heartbeat_interval = 0;
        io->heartbeat_fn = NULL;
    }
}

void hio_set_connect_timeout(hio_t* io, int timeout_ms) {
    io->connect_timeout = timeout_ms;
}

void hio_set_close_timeout(hio_t* io, int timeout_ms) {
    io->close_timeout = timeout_ms;
}

static void __keepalive_timeout_cb(htimer_t* timer) {
    hio_t* io = (hio_t*)timer->privdata;
    if (io) {
        char localaddrstr[SOCKADDR_STRLEN] = {0};
        char peeraddrstr[SOCKADDR_STRLEN] = {0};
        hlogw("keepalive timeout [%s] <=> [%s]",
                SOCKADDR_STR(io->localaddr, localaddrstr),
                SOCKADDR_STR(io->peeraddr, peeraddrstr));
        io->error = ETIMEDOUT;
        hio_close(io);
    }
}

void hio_set_keepalive_timeout(hio_t* io, int timeout_ms) {
    if (timeout_ms == 0) {
        // del
        hio_del_keepalive_timer(io);
        return;
    }

    if (io->keepalive_timer) {
        // reset
        ((struct htimeout_s*)io->keepalive_timer)->timeout = timeout_ms;
        htimer_reset(io->keepalive_timer);
    } else {
        // add
        io->keepalive_timer = htimer_add(io->loop, __keepalive_timeout_cb, timeout_ms, 1);
        io->keepalive_timer->privdata = io;
    }
    io->keepalive_timeout = timeout_ms;
}

static void __heartbeat_timer_cb(htimer_t* timer) {
    hio_t* io = (hio_t*)timer->privdata;
    if (io && io->heartbeat_fn) {
        io->heartbeat_fn(io);
    }
}

void hio_set_heartbeat(hio_t* io, int interval_ms, hio_send_heartbeat_fn fn) {
    if (interval_ms == 0) {
        // del
        hio_del_heartbeat_timer(io);
        return;
    }

    if (io->heartbeat_timer) {
        // reset
        ((struct htimeout_s*)io->heartbeat_timer)->timeout = interval_ms;
        htimer_reset(io->heartbeat_timer);
    } else {
        // add
        io->heartbeat_timer = htimer_add(io->loop, __heartbeat_timer_cb, interval_ms, INFINITE);
        io->heartbeat_timer->privdata = io;
    }
    io->heartbeat_interval = interval_ms;
    io->heartbeat_fn = fn;
}

void hio_alloc_readbuf(hio_t* io, int len) {
    if (hio_is_alloced_readbuf(io)) {
        io->readbuf.base = (char*)safe_realloc(io->readbuf.base, len, io->readbuf.len);
    } else {
        HV_ALLOC(io->readbuf.base, len);
    }
    io->readbuf.len = len;
    io->alloced_readbuf = 1;
}

void hio_free_readbuf(hio_t* io) {
    if (hio_is_alloced_readbuf(io)) {
        HV_FREE(io->readbuf.base);
        io->alloced_readbuf = 0;
        // reset to loop->readbuf
        io->readbuf.base = io->loop->readbuf.base;
        io->readbuf.len = io->loop->readbuf.len;
    }
}

int hio_read_once (hio_t* io) {
    io->read_once = 1;
    return hio_read_start(io);
}

int hio_read_until(hio_t* io, int len) {
    io->read_until = len;
    // NOTE: prepare readbuf
    if (hio_is_loop_readbuf(io) ||
        io->readbuf.len < len) {
        hio_alloc_readbuf(io, len);
    }
    return hio_read_once(io);
}

//-----------------unpack---------------------------------------------
void hio_set_unpack(hio_t* io, unpack_setting_t* setting) {
    hio_unset_unpack(io);
    if (setting == NULL) return;

    io->unpack_setting = setting;
    if (io->unpack_setting->package_max_length == 0) {
        io->unpack_setting->package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
    }
    if (io->unpack_setting->mode == UNPACK_BY_FIXED_LENGTH) {
        assert(io->unpack_setting->fixed_length != 0 &&
               io->unpack_setting->fixed_length <= io->unpack_setting->package_max_length);
    }
    else if (io->unpack_setting->mode == UNPACK_BY_DELIMITER) {
        if (io->unpack_setting->delimiter_bytes == 0) {
            io->unpack_setting->delimiter_bytes = strlen((char*)io->unpack_setting->delimiter);
        }
    }
    else if (io->unpack_setting->mode == UNPACK_BY_LENGTH_FIELD) {
        assert(io->unpack_setting->body_offset >=
               io->unpack_setting->length_field_offset +
               io->unpack_setting->length_field_bytes);
    }

    // NOTE: unpack must have own readbuf
    if (io->unpack_setting->mode == UNPACK_BY_FIXED_LENGTH) {
        io->readbuf.len = io->unpack_setting->fixed_length;
    } else {
        io->readbuf.len = HLOOP_READ_BUFSIZE;
    }
    hio_alloc_readbuf(io, io->readbuf.len);
}

void hio_unset_unpack(hio_t* io) {
    if (io->unpack_setting) {
        io->unpack_setting = NULL;
        // NOTE: unpack has own readbuf
        hio_free_readbuf(io);
    }
}

//-----------------upstream---------------------------------------------
void hio_read_upstream(hio_t* io) {
    hio_t* upstream_io = io->upstream_io;
    if (upstream_io) {
        hio_read(io);
        hio_read(upstream_io);
    }
}

void hio_write_upstream(hio_t* io, void* buf, int bytes) {
    hio_t* upstream_io = io->upstream_io;
    if (upstream_io) {
        hio_write(upstream_io, buf, bytes);
    }
}

void hio_close_upstream(hio_t* io) {
    hio_t* upstream_io = io->upstream_io;
    if (upstream_io) {
        hio_close(upstream_io);
    }
}

void hio_setup_upstream(hio_t* io1, hio_t* io2) {
    io1->upstream_io = io2;
    io2->upstream_io = io1;
    hio_setcb_read(io1, hio_write_upstream);
    hio_setcb_read(io2, hio_write_upstream);
}

hio_t* hio_get_upstream(hio_t* io) {
    return io->upstream_io;
}

hio_t* hio_setup_tcp_upstream(hio_t* io, const char* host, int port, int ssl) {
    hio_t* upstream_io = hio_create_socket(io->loop, host, port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (upstream_io == NULL) return NULL;
    if (ssl) hio_enable_ssl(upstream_io);
    hio_setup_upstream(io, upstream_io);
    hio_setcb_close(io, hio_close_upstream);
    hio_setcb_close(upstream_io, hio_close_upstream);
    hconnect(io->loop, upstream_io->fd, hio_read_upstream);
    return upstream_io;
}

hio_t* hio_setup_udp_upstream(hio_t* io, const char* host, int port) {
    hio_t* upstream_io = hio_create_socket(io->loop, host, port, HIO_TYPE_UDP, HIO_CLIENT_SIDE);
    if (upstream_io == NULL) return NULL;
    hio_setup_upstream(io, upstream_io);
    hio_read_upstream(io);
    return upstream_io;
}

#if WITH_RUDP
rudp_entry_t* hio_get_rudp(hio_t* io) {
    rudp_entry_t* rudp = rudp_get(&io->rudp, io->peeraddr);
    rudp->io = io;
    return rudp;
}

static void hio_close_rudp_event_cb(hevent_t* ev) {
    rudp_entry_t* entry = (rudp_entry_t*)ev->userdata;
    rudp_del(&entry->io->rudp, (struct sockaddr*)&entry->addr);
    // rudp_entry_free(entry);
}

int hio_close_rudp(hio_t* io, struct sockaddr* peeraddr) {
    if (peeraddr == NULL) peeraddr = io->peeraddr;
    // NOTE: do rudp_del for thread-safe
    rudp_entry_t* entry = rudp_get(&io->rudp, peeraddr);
    // NOTE: just rudp_remove first, do rudp_entry_free async for safe.
    // rudp_entry_t* entry = rudp_remove(&io->rudp, peeraddr);
    if (entry) {
        hevent_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.cb = hio_close_rudp_event_cb;
        ev.userdata = entry;
        ev.priority = HEVENT_HIGH_PRIORITY;
        hloop_post_event(io->loop, &ev);
    }
    return 0;
}
#endif

#if WITH_KCP
static kcp_setting_t s_kcp_setting;
static int __kcp_output(const char* buf, int len, ikcpcb* ikcp, void* userdata) {
    // printf("ikcp_output len=%d\n", len);
    rudp_entry_t* rudp = (rudp_entry_t*)userdata;
    assert(rudp != NULL && rudp->io != NULL);
    int nsend = sendto(rudp->io->fd, buf, len, 0, &rudp->addr.sa, SOCKADDR_LEN(&rudp->addr));
    // printf("sendto nsend=%d\n", nsend);
    return nsend;
}

static void __kcp_update_timer_cb(htimer_t* timer) {
    rudp_entry_t* rudp = (rudp_entry_t*)timer->privdata;
    assert(rudp != NULL && rudp->io != NULL && rudp->kcp.ikcp != NULL);
    ikcp_update(rudp->kcp.ikcp, (IUINT32)(rudp->io->loop->cur_hrtime / 1000));
}

int hio_set_kcp(hio_t* io, kcp_setting_t* setting) {
    io->io_type = HIO_TYPE_KCP;
    io->kcp_setting = setting;
    return 0;
}

kcp_t* hio_get_kcp(hio_t* io, uint32_t conv) {
    rudp_entry_t* rudp = hio_get_rudp(io);
    assert(rudp != NULL);
    kcp_t* kcp = &rudp->kcp;
    if (kcp->ikcp != NULL) return kcp;
    if (io->kcp_setting == NULL) {
        io->kcp_setting = &s_kcp_setting;
    }
    kcp_setting_t* setting = io->kcp_setting;
    assert(io->kcp_setting != NULL);
    kcp->ikcp = ikcp_create(conv, rudp);
    // printf("ikcp_create conv=%u ikcp=%p\n", conv, kcp->ikcp);
    kcp->ikcp->output = __kcp_output;
    kcp->conv = conv;
    if (setting->interval > 0) {
        ikcp_nodelay(kcp->ikcp, setting->nodelay, setting->interval, setting->fastresend, setting->nocwnd);
    }
    if (setting->sndwnd > 0 && setting->rcvwnd > 0) {
        ikcp_wndsize(kcp->ikcp, setting->sndwnd, setting->rcvwnd);
    }
    if (setting->mtu > 0) {
        ikcp_setmtu(kcp->ikcp, setting->mtu);
    }
    if (kcp->update_timer == NULL) {
        int update_interval = setting->update_interval;
        if (update_interval == 0) {
            update_interval = DEFAULT_KCP_UPDATE_INTERVAL;
        }
        kcp->update_timer = htimer_add(io->loop, __kcp_update_timer_cb, update_interval, INFINITE);
        kcp->update_timer->privdata = rudp;
    }
    // NOTE: alloc kcp->readbuf when hio_read_kcp
    return kcp;
}

int hio_write_kcp(hio_t* io, const void* buf, size_t len) {
    IUINT32 conv = io->kcp_setting ? io->kcp_setting->conv : 0;
    kcp_t* kcp = hio_get_kcp(io, conv);
    int nsend = ikcp_send(kcp->ikcp, (const char*)buf, len);
    // printf("ikcp_send len=%d nsend=%d\n", (int)len, nsend);
    if (nsend < 0) {
        hio_close(io);
    } else {
        ikcp_update(kcp->ikcp, (IUINT32)io->loop->cur_hrtime / 1000);
    }
    return nsend;
}

int hio_read_kcp (hio_t* io, void* buf, int readbytes) {
    IUINT32 conv = ikcp_getconv(buf);
    kcp_t* kcp = hio_get_kcp(io, conv);
    if (kcp->conv != conv) {
        hloge("recv invalid kcp packet!");
        hio_close_rudp(io, io->peeraddr);
        return -1;
    }
    // printf("ikcp_input len=%d\n", readbytes);
    ikcp_input(kcp->ikcp, (const char*)buf, readbytes);
    if (kcp->readbuf.base == NULL || kcp->readbuf.len == 0) {
        kcp->readbuf.len = DEFAULT_KCP_READ_BUFSIZE;
        HV_ALLOC(kcp->readbuf.base, kcp->readbuf.len);
    }
    int ret = 0;
    while (1) {
        int nrecv = ikcp_recv(kcp->ikcp, kcp->readbuf.base, kcp->readbuf.len);
        // printf("ikcp_recv nrecv=%d\n", nrecv);
        if (nrecv < 0) break;
        hio_read_cb(io, kcp->readbuf.base, nrecv);
        ret += nrecv;
    }
    return ret;
}
#endif
