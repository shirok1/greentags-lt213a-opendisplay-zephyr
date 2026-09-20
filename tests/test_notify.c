/* Deterministic Zephyr scheduling seam for main.c's real notification functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

typedef int32_t atomic_val_t;
typedef atomic_val_t atomic_t;
static atomic_val_t atomic_get(const atomic_t *p) { return *p; }
static void atomic_set(atomic_t *p, atomic_val_t v) { *p = v; }
static bool atomic_cas(atomic_t *p, atomic_val_t old, atomic_val_t value)
{ if (*p != old) return false; *p = value; return true; }
#define ARG_UNUSED(x) (void)(x)
#define K_MSEC(x) (x)
#define K_SEM_DEFINE(name, initial, limit) static int name = initial
#define BT_CONN_STATE_CONNECTED 1
struct bt_conn { int state; };
struct bt_conn_info { int state; };
struct command { struct bt_conn *conn; atomic_val_t generation; };
static struct { int attrs[3]; } display_service;
static atomic_t generation;
struct bt_gatt_notify_params {
    const void *attr, *data;
    size_t len;
    void (*func)(struct bt_conn *, void *);
    void *user_data;
};
static int bt_conn_get_info(struct bt_conn *c, struct bt_conn_info *i)
{ i->state = c->state; return 0; }
static int64_t now;
static int64_t k_uptime_get(void) { return now; }
static void k_sem_give(int *s) { *s = 1; }
static void k_sem_reset(int *s) { *s = 0; }
static void k_msleep(int ms) { now += ms; }
static int k_sem_take(int *s, int ms);
static int bt_gatt_notify_cb(struct bt_conn *, struct bt_gatt_notify_params *);
#include "notify-under-test.inc"

static struct bt_conn conn = { BT_CONN_STATE_CONNECTED };
static struct bt_gatt_notify_params queued, stale;
static int calls, failures, error_code;
static bool inline_completion;
static int64_t complete_at, disconnect_at, stale_at;
static int bt_gatt_notify_cb(struct bt_conn *c, struct bt_gatt_notify_params *p)
{
    assert(c == &conn && p->attr == &display_service.attrs[2]);
    calls++;
    if (failures) { failures--; return error_code; }
    queued = *p;
    if (inline_completion) p->func(c, p->user_data);
    return 0;
}
static int k_sem_take(int *s, int ms)
{
    if (*s) { *s = 0; return 0; }
    now += ms;
    if (stale_at && now >= stale_at) {
        stale_at = 0;
        stale.func(&conn, stale.user_data);
        k_sem_give(s); /* Also inject a stale wakeup after ticket invalidation. */
    }
    if (disconnect_at && now >= disconnect_at) {
        disconnect_at = 0;
        conn.state = 0;
        generation++;
    }
    if (complete_at && now >= complete_at) {
        complete_at = 0;
        queued.func(&conn, queued.user_data);
    }
    return 0;
}
static int send(void)
{
    const uint8_t response[] = {0, 0x44};
    struct command command = {&conn, generation};
    return send_wire(&command, response, sizeof(response));
}
static void reset(void)
{
    notify_ticket = 1; notify_generation = 0; notify_done = 0;
    generation = 1; conn.state = BT_CONN_STATE_CONNECTED;
    now = 0; calls = failures = error_code = 0;
    complete_at = disconnect_at = stale_at = 0;
    inline_completion = false;
}
int main(void)
{
    reset(); conn.state = 0;
    assert(send() == -ENOTCONN && calls == 0);
    reset();
    struct command old = {&conn, generation};
    generation++;
    assert(send_wire(&old, (const uint8_t *)"ok", 2) == -ENOTCONN && calls == 0);
    reset(); inline_completion = true;
    assert(send() == 0 && now == 0 && calls == 1);
    reset(); complete_at = 30;
    assert(send() == 0 && now == 30 && calls == 1);

    reset();
    assert(send() == -ETIMEDOUT && now == 2000 && calls == 1);
    assert(send() == -EBUSY && calls == 1); /* Failed disconnect cannot consume a second slot. */
    queued.func(&conn, queued.user_data);
    complete_at = now + 20;
    assert(send() == 0 && calls == 2);

    reset();
    assert(send() == -ETIMEDOUT);
    stale = queued;
    generation += 2; /* Disconnect and reconnect, including a recycled conn pointer. */
    stale_at = now + 10; complete_at = now + 40;
    assert(send() == 0 && now == 2040 && calls == 2);

    reset(); disconnect_at = 20;
    assert(send() == -ENOTCONN && now == 20);
    stale = queued;
    conn.state = BT_CONN_STATE_CONNECTED; generation++;
    stale_at = now + 10; complete_at = now + 30;
    assert(send() == 0 && now == 50 && calls == 2);

    for (int i = 0; i < 2; i++) {
        reset(); failures = 2; error_code = i ? -EAGAIN : -ENOMEM; complete_at = 40;
        assert(send() == 0 && calls == 3 && now == 40);
    }
    reset(); failures = 1; error_code = -EINVAL;
    assert(send() == -EINVAL && now == 0);
    complete_at = 10;
    assert(send() == 0 && calls == 2);

    reset(); failures = 1000; error_code = -ENOMEM;
    assert(send() == -ETIMEDOUT && now == 2000);
    assert(notify_ticket & 1); /* Allocation failure owns no notification. */
    failures = 0; complete_at = now + 10;
    assert(send() == 0);

    reset(); notify_ticket = (atomic_val_t)UINT32_MAX; complete_at = 10;
    assert(send() == 0 && notify_ticket == 1); /* Wrap uses unsigned arithmetic. */
    puts("Notification adapter passed: completion, timeout ownership, disconnect, stale callbacks/wakeups, allocation retries and ticket wrap");
}
