#include <assert.h>
#include <stdbool.h>
#include <sys/io.h>

static int denied_port;
static int permission_calls;
static int raw_io_calls;
static int data_reads;

static int mock_ioperm(unsigned long from, unsigned long num, int enabled) {
    (void)num;
    permission_calls++;
    return enabled && (int)from == denied_port ? -1 : 0;
}

static unsigned char mock_inb(unsigned short port) {
    static const unsigned char values[] = {60, 1, 2};
    raw_io_calls++;
    if (port == 0x66) return 0;
    assert(data_reads < 3);
    return values[data_reads++];
}

static void mock_outb(unsigned char value, unsigned short port) {
    raw_io_calls++;
    if (port == 0x66) assert(value == 0x80); /* EC read command only. */
}

/* Include the actual implementation, replacing every raw port operation. */
#define ioperm mock_ioperm
#define inb mock_inb
#define outb mock_outb
#define main subscreen_application_main
#include "../src/panel.c"
#undef main

int main(void) {
    unsigned char temperature = 0, rpm = 0;

    denied_port = 0x62;
    read_cpu_ec_telemetry(&temperature, &rpm);
    assert(temperature == 255 && rpm == 255);
    assert(!ec_io_ready && raw_io_calls == 0 && permission_calls == 1);

    denied_port = 0x66;
    permission_calls = 0;
    read_cpu_ec_telemetry(&temperature, &rpm);
    assert(temperature == 255 && rpm == 255);
    assert(!ec_io_ready && raw_io_calls == 0 && permission_calls == 3);
    assert(ec_wait_ready() == -1 && raw_io_calls == 0);

    denied_port = -1;
    permission_calls = 0;
    read_cpu_ec_telemetry(&temperature, &rpm);
    assert(temperature == 60 && rpm == (1 * 0xFF + 2) / 42);
    assert(!ec_io_ready && permission_calls == 4 && data_reads == 3);

    raw_io_calls = 0;
    allow_ec_writes = false;
    assert(guarded_ec_write_byte(0x56, 99) == 0);
    assert(raw_io_calls == 0);
    puts("PASS: denied EC access performs no raw I/O; telemetry uses unknown values; normal reads release permissions; default EC writes stay blocked.");
    return 0;
}
