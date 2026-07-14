#include "SignalStop.h"

#include <csignal>

volatile bool g_devourer_should_stop = false;

static void devourer_stop_handler(int /*sig*/) { g_devourer_should_stop = true; }

void install_devourer_signal_handlers() {
#ifdef _WIN32
  /* Windows has no POSIX sigaction; std::signal covers SIGINT/SIGTERM and is
   * enough for the demos' Ctrl-C / terminate stop semantics. */
  std::signal(SIGINT, devourer_stop_handler);
  std::signal(SIGTERM, devourer_stop_handler);
#else
  struct sigaction sa {};
  sa.sa_handler = devourer_stop_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: let blocking syscalls return so loops re-check */
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif
}
