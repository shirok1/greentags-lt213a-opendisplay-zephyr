#include "protocol.h"
#include "config_store.h"
#include "security.h"
#include "telemetry.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <hal/nrf_ficr.h>
#include <hal/nrf_wdt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <string.h>

static struct od_protocol protocol;
static struct od_security security;
static atomic_t generation;
static atomic_t link_up;
/* A coalescing wakeup covers both queued commands and connection changes. */
K_SEM_DEFINE(events, 0, 1);
static uint8_t msd[16] = {0x46, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

struct command {
    struct bt_conn *conn;
    atomic_val_t generation;
    uint8_t size;
    bool accepted;
    uint8_t data[OD_MAX_FRAME];
};
K_MSGQ_DEFINE(commands, sizeof(struct command), 1, 4);

static ssize_t written(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(attr);
    if (offset || (flags & BT_GATT_WRITE_FLAG_PREPARE)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 2 || len > OD_MAX_FRAME) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    struct command command = { .conn = bt_conn_ref(conn),
        .generation = atomic_get(&generation), .size = len };
    memcpy(command.data, buf, len);
    if (k_msgq_put(&commands, &command, K_NO_WAIT)) {
        bt_conn_unref(command.conn);
        /* WWR has no ATT error response. Drop the link rather than silently
         * losing bytes and allowing a later END to produce the wrong image. */
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    k_sem_give(&events);
    return len;
}

BT_GATT_SERVICE_DEFINE(display_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0x2446)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x2446),
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_WRITE, NULL, written, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static const struct bt_data advertising[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x46, 0x24),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, msd, sizeof(msd)),
};
static struct bt_data scan_response[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, NULL, 8),
};

static int update_name(void)
{
    BUILD_ASSERT(CONFIG_BT_DEVICE_NAME_MAX == OD_NAME_MAX);
    char name[OD_NAME_MAX + 1];
    size_t len = od_config_name(name, NRF_FICR->DEVICEID[1]);
    int err = bt_set_name(name);
    if (!err) {
        scan_response[0].data = (const uint8_t *)bt_get_name();
        scan_response[0].data_len = len;
    }
    return err;
}

/* Shared Mynewt/Zephyr profile: fast 100–150 ms for 30 s, slow 1 s. */
static int advertise(bool fast)
{
    return bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE,
                           fast ? 160 : 1600, fast ? 240 : 1600, NULL),
                           advertising, ARRAY_SIZE(advertising),
                           scan_response, ARRAY_SIZE(scan_response));
}

#define LINK_IDLE_MS 120000
#define TELEMETRY_MS 300000
#define FAST_ADV_MS 30000

static int sample_msd(void *ctx)
{
    ARG_UNUSED(ctx);
    return od_read_msd(msd);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    if (!err) {
        atomic_set(&link_up, 1);
        atomic_inc(&generation);
        k_sem_give(&events);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn); ARG_UNUSED(reason);
    atomic_clear(&link_up);
    atomic_inc(&generation);
    k_sem_give(&events);
    /* Legacy connectable advertising automatically resumes in Zephyr. */
}
BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static bool live(const struct command *command)
{
    struct bt_conn_info info;
    return command->generation == atomic_get(&generation) &&
           !bt_conn_get_info(command->conn, &info) && info.state == BT_CONN_STATE_CONNECTED;
}

static int send_wire(void *ctx, const uint8_t *data, size_t len)
{
    const struct command *command = ctx;
    int64_t deadline = k_uptime_get() + 2000;
    for (;;) {
        if (!live(command)) { return -ENOTCONN; }
        int err = bt_gatt_notify(command->conn, &display_service.attrs[2], data, len);
        if (err != -ENOMEM && err != -EAGAIN) { return err; }
        if (k_uptime_get() >= deadline) { return -ETIMEDOUT; }
        k_msleep(10);
    }
}

static int send_response(void *ctx, const uint8_t *data, size_t len)
{
    struct command *command = ctx;
    /* As upstream: rejected frames and handshake/discovery traffic must not
     * keep an otherwise idle connection alive indefinitely. */
    if (data[0] == 0 && data[1] == command->data[1] && data[1] != 0x43 && data[1] != 0x50) {
        command->accepted = true;
    }
    if (security.authenticated && data[1] != 0x43 && data[1] != 0x50 && data[0] != 0xfe) {
        uint8_t encrypted[64];
        int count = od_security_encrypt(&security, data, len, encrypted, sizeof(encrypted));
        return count < 0 ? count : send_wire(ctx, encrypted, count);
    }
    return send_wire(ctx, data, len);
}

static int secure_random(void *data, size_t len) { return sys_csrand_get(data, len); }

static int dispatch(struct command *command, const struct od_io *io)
{
    uint32_t now = k_uptime_get_32();
    bool expired = od_security_expire(&security, now);
    if (expired) { od_abort(&protocol, io); od_config_cancel(); }
    if (command->data[0] == 0 && command->data[1] == 0x50) {
        od_abort(&protocol, io); od_config_cancel();
        uint32_t id = NRF_FICR->DEVICEID[0];
        uint8_t device_id[] = {id >> 24, id >> 16, id >> 8, id};
        uint8_t response[23];
        int count = od_security_auth(&security, command->data + 2, command->size - 2,
            now, device_id, secure_random, response);
        return send_wire(command, response, count);
    }
    if (od_security_enabled() && !(command->data[0] == 0 && command->data[1] == 0x43)) {
        if (!security.authenticated) {
            const uint8_t response[] = {0, command->data[1], 0xfe};
            return send_wire(command, response, sizeof(response));
        }
        int count = od_security_decrypt(&security, command->data, command->size, now);
        if (count < 0) {
            /* PIPE DATA retransmissions must not abort the transfer/session. */
            if (count == -EALREADY && command->data[0] == 0 && command->data[1] == 0x81) { return 0; }
            const uint8_t response[] = {0, command->data[1], 0xff};
            return send_wire(command, response, sizeof(response));
        }
        command->size = count;
    }
    return od_handle(&protocol, io, command->data, command->size);
}

static int begin(void *ctx)
{
    ARG_UNUSED(ctx);
    return epd_begin();
}

static int write_panel(void *ctx, const uint8_t *data, size_t len)
{
    ARG_UNUSED(ctx);
    return epd_write(data, len);
}

static int finish(void *ctx, uint8_t mode) { ARG_UNUSED(ctx); return epd_finish(mode); }
static int region(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ ARG_UNUSED(ctx); return epd_region(x, y, w, h); }
static void abort_panel(void *ctx) { ARG_UNUSED(ctx); (void)epd_off(); }

static void reboot(void *ctx)
{
    ARG_UNUSED(ctx);
    sys_reboot(SYS_REBOOT_COLD);
}

static void disconnect_idle(struct bt_conn *conn, void *ctx)
{
    if (atomic_get(&generation) != *(atomic_val_t *)ctx) { return; }
    struct bt_conn_info info;
    if (!bt_conn_get_info(conn, &info) && info.state == BT_CONN_STATE_CONNECTED) {
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* No driver state or extra thread: this board has only 16 KiB RAM. Run while
 * sleeping, pause only for a halted debugger. Feed solely from the main loop;
 * 180 s covers its 60 s idle wait plus bounded panel timeout/cleanup paths. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    ARG_UNUSED(reason); ARG_UNUSED(esf);
    sys_reboot(SYS_REBOOT_COLD);
}

int main(void)
{
    nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK);
    nrf_wdt_reload_value_set(NRF_WDT, 180u * 32768u - 1u);
    nrf_wdt_reload_request_enable(NRF_WDT, NRF_WDT_RR0);
    nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);
    extern int storage_init(void);
    int storage_err = storage_init();
    if (storage_err) { return storage_err; }
    od_security_reset(&security);
    if (od_security_enabled()) { msd[15] |= BIT(3); }
    int err = epd_init();
    if (err) { return err; }
    (void)od_read_msd(msd);
    /* Preserve Nordic's factory static identity without vendor HCI commands.
     * Same FICR validity and static-address bits as Zephyr's Nordic controller.
     */
    if ((NRF_FICR->DEVICEADDRTYPE & 1) &&
        (NRF_FICR->DEVICEADDR[0] != UINT32_MAX ||
         (NRF_FICR->DEVICEADDR[1] & UINT16_MAX) != UINT16_MAX)) {
        bt_addr_le_t identity = {.type = BT_ADDR_LE_RANDOM};
        sys_put_le32(NRF_FICR->DEVICEADDR[0], identity.a.val);
        sys_put_le16(NRF_FICR->DEVICEADDR[1], identity.a.val + 4);
        BT_ADDR_SET_STATIC(&identity.a);
        err = bt_id_create(&identity, NULL);
        if (err < 0) { return err; }
    }
    err = bt_enable(NULL);
    if (err) { return err; }
    err = update_name();
    if (err) { return err; }
    err = advertise(true);
    if (err) { return err; }
    bool fast_advertising = true;
    bool restart_advertising = false;
    int64_t retry_adv_at = 0;
    int64_t slow_adv_at = k_uptime_get() + FAST_ADV_MS;
    atomic_val_t current = -1;
    int64_t last_packet = k_uptime_get();
    int64_t last_activity = last_packet;
    int64_t last_telemetry = k_uptime_get();
    for (;;) {
        nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0);
        struct command command;
        const struct od_io io = { .send = send_response, .begin = begin,
            .write = write_panel, .finish = finish, .abort = abort_panel,
            .reboot = reboot, .ctx = &command, .msd = msd, .region = region,
            .sample_msd = sample_msd };
        int64_t now = k_uptime_get();
        /* Bound watchdog feeding independently of the five-minute telemetry. */
        int64_t deadline = now + 60000;
        if (!atomic_get(&link_up)) { deadline = MIN(deadline, last_telemetry + TELEMETRY_MS); }
        if (protocol.active || od_config_writing()) { deadline = MIN(deadline, last_packet + 30000); }
        if (atomic_get(&link_up)) { deadline = MIN(deadline, last_activity + LINK_IDLE_MS); }
        else if (restart_advertising) { deadline = MIN(deadline, retry_adv_at); }
        else if (fast_advertising) { deadline = MIN(deadline, slow_adv_at); }
        /* No one-second polling. Connection callbacks wake this wait even
         * when the command queue is full, so disconnect cleanup is prompt. */
        if (atomic_get(&generation) == current) {
            (void)k_sem_take(&events, K_MSEC(MAX(deadline - now, 0)));
        }
        /* Waiting and command processing each get a full watchdog interval.
         * A slow multi-notification config read can outlast one idle period. */
        nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0);
        int received = k_msgq_get(&commands, &command, K_NO_WAIT);
        now = k_uptime_get();
        atomic_val_t next = atomic_get(&generation);
        if (next != current || ((protocol.active || od_config_writing()) && now - last_packet >= 30000)) {
            od_abort(&protocol, &io);
            od_config_cancel();
            if (next != current) {
                od_security_reset(&security);
                last_packet = now;
                last_activity = now;
                if (current != -1 && !atomic_get(&link_up)) {
                    /* Stop Zephyr's automatically resumed legacy advertiser
                     * and restart the fast window after every disconnect. */
                    fast_advertising = true;
                    slow_adv_at = now + FAST_ADV_MS;
                    restart_advertising = true;
                    retry_adv_at = now;
                }
            }
            current = next;
        }
        if (!atomic_get(&link_up) &&
            ((restart_advertising && now >= retry_adv_at) ||
             (!restart_advertising && fast_advertising && now >= slow_adv_at))) {
            /* Retry transient HCI errors without ending the fast window early.
             * A connection may race stop/start; never disconnect that peer. */
            bool fast = now < slow_adv_at;
            if (!fast && fast_advertising && last_telemetry < slow_adv_at) {
                (void)od_read_msd(msd);
                last_telemetry = now;
            }
            int adv_err = bt_le_adv_stop();
            if (!adv_err && !atomic_get(&link_up)) {
                adv_err = advertise(fast);
                if (!adv_err) { fast_advertising = fast; }
            }
            restart_advertising = adv_err != 0;
            retry_adv_at = k_uptime_get() + 5000;
        }
        if (!atomic_get(&link_up) && now - last_telemetry >= TELEMETRY_MS) {
            if (!od_read_msd(msd)) {
                (void)bt_le_adv_update_data(advertising, ARRAY_SIZE(advertising),
                                           scan_response, ARRAY_SIZE(scan_response));
            }
            last_telemetry = now;
        }
        if (!received) {
            if (live(&command)) {
                size_t config_len;
                const uint8_t *old_config = od_config_get(&config_len);
                err = dispatch(&command, &io);
                if (old_config != od_config_get(&config_len)) {
                    od_security_reset(&security);
                    msd[15] = (msd[15] & ~BIT(3)) | (od_security_enabled() ? BIT(3) : 0);
                    (void)update_name();
                    (void)bt_le_adv_update_data(advertising, ARRAY_SIZE(advertising), scan_response, ARRAY_SIZE(scan_response));
                }
                if (command.data[0] == 0 && command.data[1] == 0x44 && command.size == 2 && command.accepted && !err) {
                    msd[15] &= ~BIT(1);
                    (void)bt_le_adv_update_data(advertising, ARRAY_SIZE(advertising),
                                               scan_response, ARRAY_SIZE(scan_response));
                }
                last_packet = k_uptime_get();
                if (command.accepted) { last_activity = last_packet; }
                if (err) {
                    od_abort(&protocol, &io);
                    bt_conn_disconnect(command.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                }
            }
            bt_conn_unref(command.conn);
        }
        /* Consume queued work first. Synchronous refresh also finishes before
         * this check and restamps activity, regardless of its wall-clock cost. */
        now = k_uptime_get();
        if (atomic_get(&link_up) && now - last_activity >= LINK_IDLE_MS) {
            bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_idle, &current);
            last_activity = now; /* Bound retries on an HCI error. */
        }
    }
}
