#ifndef SIGNAL_STOP_H
#define SIGNAL_STOP_H

/* Process-global "please stop" flag, shared by the demos' signal handlers and
 * the device RX/TX loops. Without this, a SIGTERM (e.g. from the test harness's
 * `timeout`) or SIGINT killed the process instantly: the read/write loop never
 * broke, the chip was never powered down, and an abandoned adapter (RX DMA still
 * running, firmware still up) overflowed its RX FIFO and hung its USB core —
 * after which the device fails to re-enumerate (kernel logs error -71). The
 * kernel driver avoids this by running a clean card-disable on unbind; setting
 * this flag lets devourer do the same. async-signal-safe: a plain volatile bool
 * written by the handler and polled by the loops. */
extern volatile bool g_devourer_should_stop;

/* Install SIGINT + SIGTERM handlers that set g_devourer_should_stop. Idempotent;
 * call once early in main(). */
void install_devourer_signal_handlers();

#endif /* SIGNAL_STOP_H */
