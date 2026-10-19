/*
 * USBPI keyboard wire format (shared by the Pi daemon and the client).
 *
 * Flat, headerless and included by both unity builds.  Every field is
 * serialized explicitly as little-endian so an armhf Pi and an x86_64 client
 * agree on the layout regardless of compiler padding or struct alignment.
 *
 * Server -> client: EVENTS / SNAPSHOT / GAP
 * Client -> server: HELLO / ACK
 */

struct kbd_ringev {
    uint32_t seq;
    uint16_t code;
    uint8_t value;
};

struct kbd_wire_hdr {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t payload_len;
    uint32_t session;
    uint32_t seq;
    uint32_t ack;
    uint32_t flags;
    uint64_t timestamp_ns;
};

static __attribute__((unused)) void kbd_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static __attribute__((unused)) void kbd_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static __attribute__((unused)) void kbd_put_u64(uint8_t *p, uint64_t v)
{
    kbd_put_u32(p, (uint32_t)(v & 0xffffffffu));
    kbd_put_u32(p + 4, (uint32_t)(v >> 32));
}

static __attribute__((unused)) uint16_t kbd_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static __attribute__((unused)) uint32_t kbd_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static __attribute__((unused)) uint64_t kbd_get_u64(const uint8_t *p)
{
    return (uint64_t)kbd_get_u32(p) |
           ((uint64_t)kbd_get_u32(p + 4) << 32);
}

static __attribute__((unused)) size_t kbd_hdr_write(uint8_t *buf, uint8_t type, uint32_t session,
                            uint32_t seq, uint32_t ack, uint32_t flags,
                            uint16_t payload_len)
{
    memset(buf, 0, KBD_HDR_LEN);
    kbd_put_u32(buf + 0, KBD_MAGIC);
    buf[4] = (uint8_t)KBD_VERSION;
    buf[5] = type;
    kbd_put_u16(buf + 6, payload_len);
    kbd_put_u32(buf + 8, session);
    kbd_put_u32(buf + 12, seq);
    kbd_put_u32(buf + 16, ack);
    kbd_put_u32(buf + 20, flags);
    kbd_put_u64(buf + 24, 0);
    return KBD_HDR_LEN;
}

static __attribute__((unused)) int kbd_hdr_read(const uint8_t *buf, size_t len,
                        struct kbd_wire_hdr *h)
{
    if (len < KBD_HDR_LEN)
        return -1;
    h->magic = kbd_get_u32(buf + 0);
    h->version = buf[4];
    h->type = buf[5];
    h->payload_len = kbd_get_u16(buf + 6);
    h->session = kbd_get_u32(buf + 8);
    h->seq = kbd_get_u32(buf + 12);
    h->ack = kbd_get_u32(buf + 16);
    h->flags = kbd_get_u32(buf + 20);
    h->timestamp_ns = kbd_get_u64(buf + 24);
    if (h->magic != KBD_MAGIC || h->version != KBD_VERSION)
        return -1;
    if ((size_t)KBD_HDR_LEN + h->payload_len > len)
        return -1;
    return 0;
}

/* Build an EVENTS or SNAPSHOT packet: header + count/key_bytes + the recent
 * transition window + the complete pressed-key state. */
static __attribute__((unused)) size_t kbd_pkt_state(uint8_t *buf, size_t cap, uint8_t type,
                            uint32_t session, uint32_t seq, uint32_t flags,
                            const unsigned char *state, size_t key_bytes,
                            const struct kbd_ringev *evs, unsigned count)
{
    size_t payload, total;
    uint8_t *p;
    unsigned i;

    if (count > KBD_MAX_EVENTS_PKT)
        count = KBD_MAX_EVENTS_PKT;
    payload = 4 + (size_t)count * KBD_EV_WIRE_LEN + key_bytes;
    if (payload > 0xffff)
        return 0;
    total = KBD_HDR_LEN + payload;
    if (total > cap)
        return 0;

    kbd_hdr_write(buf, type, session, seq, 0, flags, (uint16_t)payload);
    p = buf + KBD_HDR_LEN;
    kbd_put_u16(p, (uint16_t)count);
    kbd_put_u16(p + 2, (uint16_t)key_bytes);
    p += 4;
    for (i = 0; i < count; i++) {
        kbd_put_u32(p, evs[i].seq);
        kbd_put_u16(p + 4, evs[i].code);
        p[6] = evs[i].value;
        p[7] = 0;
        p += KBD_EV_WIRE_LEN;
    }
    memcpy(p, state, key_bytes);
    return total;
}

static __attribute__((unused)) int kbd_pkt_parse(const uint8_t *buf, size_t len,
                         struct kbd_wire_hdr *h, uint16_t *count,
                         unsigned char *state, size_t key_bytes,
                         struct kbd_ringev *evs, unsigned maxev)
{
    const uint8_t *p;
    unsigned n, i;

    if (kbd_hdr_read(buf, len, h) < 0)
        return -1;
    if (h->type != KBD_PKT_EVENTS && h->type != KBD_PKT_SNAPSHOT)
        return -1;
    if (h->payload_len < 4)
        return -1;
    p = buf + KBD_HDR_LEN;
    n = kbd_get_u16(p);
    if (kbd_get_u16(p + 2) != key_bytes)
        return -1;
    if (4 + n * KBD_EV_WIRE_LEN + key_bytes > h->payload_len)
        return -1;
    if (n > maxev)
        return -1;
    p += 4;
    for (i = 0; i < n; i++) {
        evs[i].seq = kbd_get_u32(p);
        evs[i].code = kbd_get_u16(p + 4);
        evs[i].value = p[6];
        p += KBD_EV_WIRE_LEN;
    }
    memcpy(state, p, key_bytes);
    *count = (uint16_t)n;
    return 0;
}

static __attribute__((unused)) size_t kbd_pkt_client(uint8_t *buf, size_t cap, uint8_t type,
                             uint32_t session, uint32_t last_applied,
                             uint32_t flags, uint8_t led)
{
    size_t total = KBD_HDR_LEN + 4;

    if (cap < total)
        return 0;
    kbd_hdr_write(buf, type, session, last_applied, 0, flags, 4);
    buf[KBD_HDR_LEN + 0] = led;
    buf[KBD_HDR_LEN + 1] = 0;
    buf[KBD_HDR_LEN + 2] = 0;
    buf[KBD_HDR_LEN + 3] = 0;
    return total;
}

static __attribute__((unused)) int kbd_pkt_client_parse(const uint8_t *buf, size_t len,
                                struct kbd_wire_hdr *h, uint8_t *led)
{
    if (kbd_hdr_read(buf, len, h) < 0)
        return -1;
    if (h->type != KBD_PKT_HELLO && h->type != KBD_PKT_ACK)
        return -1;
    if (h->payload_len < 1)
        return -1;
    *led = buf[KBD_HDR_LEN];
    return 0;
}
