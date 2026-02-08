/*======================================================================

  pi_button_to_kbd

  main.c

  Kevin Boone, CPL v3.0

======================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <signal.h>
#include <errno.h>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <csignal>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <sys/time.h>

#include "utils.h"

// Constants to use when we specify whether to detect the rising edge
//   or falling edge of the GPIO state change
#define EDGE_RISING 0x01
#define EDGE_FALLING 0x02

// CLOCK_ERROR_SECONDS is how long the elapsed time can be between two
//   GPIO events, before we conclude that the system clock has been adjusted,
//   and we need to reset the internal timer. The value needs to be large
//   enough that minor adjustments caused by NTP sync don't cause events to
//   be skipped, but not so large that we don't detect problems. In practce,
//   in a Pi without a real-time clock, when NTP sets the date, it will
//   result in a sudden clock change of at least 30 years, which is easy to
//   detect.
#define SEC_PER_YEAR 31536000

// Constants used in the keyboard mapping table. To indicate a
//   'key up' event, we will OR the keyboard scan code with UP (0);
//   to indicate a keyboard down event, we will OR it with DOWN (1).
//   DOWN needs to be larger than the largest scan code. In practice,
//   all scan codes will fit into the lowest 8 bits. Of course, ORing
//   a number with zero does not change it, but this idiom makes the
//   mapping table easier to read.
#define UP 0x0000
#define DOWN 0x1000

// The usual boolean types
#define BOOL int
#define TRUE 1
#define FALSE 0

// BOUNCE_MSEC is how long to lock out the button change after it has been
//   pressed or released. It should be longer than the longest contact
//   bounce, but short enough to allow reasonably rapid keypresses. Some
//   trial-and-error might be required, to find an optimal value for a
//   particular type of switch.
#define BOUNCE_MSEC 100
#define DOUBLE_CLICK_MSEC 400

// MAX_PINS is the largest number of GPIO pins we will monitor. Using a fixed
//   value makes the memory management less messy.
#define MAX_PINS 16

// Default edge detection. If the switch is active low, then we need the
//   falling edge if we trigger on press. Or the rising edge if we trigger
//   on release
#define EDGE EDGE_RISING

// Set whether to write debug output
#define DEBUG 0

// Define the time discrepancy that we will interpret as a genuine
// clock change  (see above)
#define CLOCK_ERROR_SECONDS SEC_PER_YEAR

// This is the mapping table. Each GPIO pin is associated with an
//   array of key events. The event array ends with pin 0, since there is no
//   GPIO pin zero.
//   The key codes are scan codes, define in input-event-codes.h.
//   To indicate a key press, OR the scan code with DOWN. To indicate a key
//   release, OR it with UP. Actually, UP is 0, but it's easier to read
//   if both press and release are coded the same.

typedef struct _Mapping
{
  int pin;
  unsigned int *keys_single;
  unsigned int *keys_double;
} Mapping;

// Here are the mappings for specific keys...
unsigned int key_r[] = {KEY_R | DOWN, KEY_R | UP, 0};
unsigned int key_g[] = {KEY_G | DOWN, KEY_G | UP, 0};
unsigned int key_left[] = {KEY_LEFT | DOWN, KEY_LEFT | UP, 0};
unsigned int key_right[] = {KEY_RIGHT | DOWN, KEY_RIGHT | UP, 0};
unsigned int key_up[] = {KEY_UP | DOWN, KEY_UP | UP, 0};
unsigned int key_down[] = {KEY_DOWN | DOWN, KEY_DOWN | UP, 0};
unsigned int key_enter[] = {KEY_ENTER | DOWN, KEY_ENTER | UP, 0};

// Ctrl+R
// unsigned int key_ctrl_r[] = {KEY_LEFTCTRL | DOWN, KEY_R | DOWN, KEY_R | UP, KEY_LEFTCTRL | UP, 0};

// ...and here is the mapping from pins to keystrokes.
Mapping mappings_pi[] =
    {
        {24, key_left, NULL},
        {18, key_right, NULL},
        {22, key_up, NULL},
        {27, key_down, NULL},
        {23, key_enter, NULL},
        {17, key_r, key_g},
        {4, key_g, NULL},
        // Add more here if required...
        {0, NULL, NULL}};

//https://docs.radxa.com/en/zero/zero3/hardware-design/hardware-interface
Mapping mappings_radxa[] =
    {
        {98, key_left, NULL},   //Header pin 13
        {101, key_right, NULL}, //Header pin 40
        {105, key_up, NULL},    //Header pin 16
        {106, key_down, NULL},  //Header pin 18
        {97, key_enter, NULL},  //Header pin 11
        {114, key_r, key_g},    //Header pin 32
        {102, key_g, NULL},     //Header pin 38
        // Add more here if required...
        {0, NULL, NULL}};

const Mapping* mappings = mappings_pi;

static BOOL debug = DEBUG;

// quit will be set true in the quit signal handler, ending the program's
//   main loop
static BOOL quit = FALSE;

static int uinput_fd;

static std::thread poll_thread;  

static struct pollfd fdset[MAX_PINS];
static struct pollfd fdset_base[MAX_PINS];
static std::thread polling_thread;
static int npins = 0;
static int pins[MAX_PINS];

/*======================================================================
  dbglog
  Write debug logging to stderr, if debug==TRUE
======================================================================*/
static void dbglog(const char *fmt, ...)
{
  if (!debug)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/*======================================================================
  quit_signal
  Signal handler. Just set quit=TRUE, to end the main loop
======================================================================*/
void quit_signal(int dummy)
{
  quit = TRUE;
}

/*======================================================================
  write_to_file
  Helper function for writing a text string to a file. Note that there
    are no circumstances in this application where a file write could
    fail, but the application continue to do anything useful. So
    this function calls exit() on any failure. This isn't normally a
    very elegant thing to do, but it's OK here.
======================================================================*/
static void write_to_file(const char *file, const char *text)
{
  FILE *f = fopen(file, "w");
  if (f)
  {
    fprintf(f, text);
    fclose(f);
  }
  else
  {
    fprintf(stderr, "Can't write to %s: %s\n", file,
            strerror(errno));
    exit(-1);
  }
}

/*======================================================================
  unexport_pins
  Tidy up GPIO by 'unexporting' any pins that were exported earlier
======================================================================*/
static void unexport_pins(int *pins, int npins)
{
  for (int i = 0; i < npins; i++)
  {
    int pin = pins[i];
    char s[50];
    snprintf(s, sizeof(s), "%d", pin);
    write_to_file("/sys/class/gpio/unexport", s);
  }
}

/*======================================================================
  export_pins
  Prepare the GPIO pins. Set the relevant pins as inputs, and make
    them generate interrupts on both rising and falling transitions.
  With most switches it's hardly worth setting a specific edge, because
    they are so bouncy. Instead, we'll respond to both edges, and
    check the GPIO pin state after it has settled.
======================================================================*/
static void export_pins(int *pins, int npins)
{
  int i;
  for (i = 0; i < npins; i++)
  {
    int pin = pins[i];
    char s[50];
    snprintf(s, sizeof(s), "%d", pin);
    write_to_file("/sys/class/gpio/export", s);
    snprintf(s, sizeof(s), "/sys/class/gpio/gpio%d/direction", pin);
    write_to_file(s, "in");
    snprintf(s, sizeof(s), "/sys/class/gpio/gpio%d/edge", pin);
    write_to_file(s, "both");
  }
}

/*======================================================================
  get_pin_state
  Read the state of the pin from the gpio 'value' psuedo file.
  In principle this function can return -1 if the data read is in
  the wrong format but, in practice, it always seems to read exactly
  two bytes, of which the first is the digit 0 or 1, and the second
  is the EOL. It seems that the read() call will never block (which is,
  I suppose, to be expected)
======================================================================*/
int get_pin_state(int pin)
{
  char s[50];
  char buff[3];
  snprintf(s, sizeof(s), "/sys/class/gpio/gpio%d/value", pin);
  int fd = open(s, O_RDONLY);
  int rc = read(fd, buff, sizeof(buff));
  close(fd);
  if (rc == 2)
    return (buff[0] - '0');
  return -1;
}

/*======================================================================
  open_uinput
  Open and prepare the uinput device. If any of this fails, exit the
    program -- there is nothing useful to be done afterwards.
======================================================================*/
static int open_uinput(void)
{
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd > 0)
  {
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    // We need to export all the key codes in the mapping table.
    // It doesn't hurt to export some more than once
    auto export_mapping = [fd](const Mapping *map)
    {
      int p = 0;
      const Mapping *m = &map[p];
      while (m->pin != 0)
      {
        auto export_sequence = [fd](unsigned int *keystrokes)
        {
          if (keystrokes == NULL) return;
          while (*keystrokes)
          {
            unsigned char raw_keystroke = *keystrokes & 0xFF;
            ioctl(fd, UI_SET_KEYBIT, raw_keystroke);
            keystrokes++;
          }
        };
        export_sequence(m->keys_single);
        export_sequence(m->keys_double);
        p++;
        m = &map[p];
      };
    };

    export_mapping(mappings);

    // Create the dummy input device
    // This will create a new /dev/input/eventXX device, that will
    //   feed into the kernel's input subsystem
    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;  // Dummy
    usetup.id.product = 0x5678; // Dummy
    strcpy(usetup.name, "Dummy input device");
    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);

    return fd;
  }
  else
  {
    fprintf(stderr, "Can't open /dev/uinput: %s\n", strerror(errno));
    exit(-1);
    return -1;
  }
}

/*======================================================================
  close_uinput
======================================================================*/
static void close_uinput(int fd)
{
  // Easy -- nothing else to do in this current implementation
  close(fd);
}

/*======================================================================
  get_mapping
  Get the entry in the mapping table that corresponds to a specific
    GPIO pin. Returns NULL if the pin has no entry in the mapping
    table, but that should never happen unless there is a really
    weird internal error.
======================================================================*/
const Mapping *get_mapping(int pin)
{
  Mapping *ret = NULL;
  int p = 0;
  const Mapping *m = &mappings[p];
  while (m->pin != 0)
  {
    if (pin == m->pin)
      return m;
    p++;
    m = &mappings[p];
  };

  return ret;
}

/*======================================================================
  emit_event
  Use uinput to send an event with a specific type, code, and value
======================================================================*/
void emit_event(int uinput_fd, int type, int code, int val)
{
  struct input_event ie;
  ie.type = type;
  ie.code = code;
  ie.value = val;
  // I don't think it matters, in practice, whether we set the
  //   keystroke timestamp.
  ie.time.tv_sec = 0;
  ie.time.tv_usec = 0;
  write(uinput_fd, &ie, sizeof(ie));
}

/*======================================================================
  emit_keystroke
  Send a keystroke event, using the 'key' entry from the mapping
    table. The event may be a key up or key down; the MASK
    bitmap is used to distinguish the two.
======================================================================*/
static void emit_keystroke(int uinput_fd, unsigned int key)
{
  if (key & DOWN)
  {
    emit_event(uinput_fd, EV_KEY, key & 0xFF, 1);
    emit_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
  }
  else
  {
    emit_event(uinput_fd, EV_KEY, key & 0xFF, 0);
    emit_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
  }
}

static void emit_sequence(int uinput_fd, unsigned int *keys)
{
  while (*keys)
  {
    dbglog("Emit keystroke %04X\n", *keys);
    emit_keystroke(uinput_fd, *keys);
    keys++;
  }
}

/*======================================================================
  button_pressed
  Called by the main loop whenever a GPIO state change is detected.
  We find the keyboard mapping that corresponds to the detected
    pin, and output the keystrokes from that mapping.
======================================================================*/
static void button_pressed(int uinput_fd, int pin, int state, bool double_click)
{
  const Mapping *m = get_mapping(pin);
  if (m)
  {
    unsigned int *keys = double_click ? m->keys_double : m->keys_single;
    if (keys != NULL)
    {
      emit_sequence(uinput_fd, keys);
    }
  }
  else
  {
    fprintf(stderr, "Inernal error: pin %d with no mapping\n", pin);
  }
}

//======================================================================
//======================================================================
void polling_thread_func()
{
  time_t start = time(NULL);
  int bounce_time = BOUNCE_MSEC;
  int ticks[MAX_PINS] = {0};
  int pending_click_msec[MAX_PINS] = {0};

  dbglog("Starting GPIO poll in thread\n");
  while (!quit)
  {
    memcpy(&fdset, &fdset_base, sizeof(fdset));
    poll(fdset, npins, 3000);

    if (std::abs(time(NULL) - start) > CLOCK_ERROR_SECONDS)
    {
      dbglog("System time has changed: correcting\n");
      start = time(NULL);
      memset(ticks, 0, sizeof(ticks));
      memset(pending_click_msec, 0, sizeof(pending_click_msec));
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    int now_msec = (tv_now.tv_sec - start) * 1000 + (tv_now.tv_usec / 1000);

    for (int i = 0; i < npins; i++)
    {
      if (pending_click_msec[i] > 0 && now_msec - pending_click_msec[i] > DOUBLE_CLICK_MSEC)
      {
        button_pressed(uinput_fd, pins[i], 0, false);
        pending_click_msec[i] = 0;
      }
    }

    for (int i = 0; i < npins; i++)
    {
      if (fdset[i].revents & POLLPRI)
      {
        int pin = pins[i];
        char buff[50];
        read(fdset[i].fd, buff, sizeof(buff));

        if (now_msec - ticks[i] > bounce_time && now_msec > 1000)
        {
          usleep(2000);
          int state = get_pin_state(pin);
          if ((state == 0 && (EDGE & EDGE_FALLING)) || (state == 1 && (EDGE & EDGE_RISING)))
          {
            dbglog("GPIO state change: pin %d, state %d\n", pin, state);
            const Mapping *m = get_mapping(pin);
            if (m != NULL && m->keys_double != NULL)
            {
              if (pending_click_msec[i] > 0 && now_msec - pending_click_msec[i] <= DOUBLE_CLICK_MSEC)
              {
                button_pressed(uinput_fd, pin, state, true);
                pending_click_msec[i] = 0;
              }
              else
              {
                pending_click_msec[i] = now_msec;
              }
            }
            else
            {
              button_pressed(uinput_fd, pin, state, false);
            }
          }
          ticks[i] = now_msec;
        }
      }
    }
  }
}

//======================================================================
//======================================================================
void gpio_buttons_start()
{
  int pin = 0;

  if ( isRadxaZero3() )
  {
    mappings = mappings_radxa;
  }

  const Mapping *m = &mappings[pin];
  while (m->pin != 0)
  {
    pins[npins++] = m->pin;
    pin++;
    m = &mappings[pin];
  }

  export_pins(pins, npins);

  std::signal(SIGQUIT, quit_signal);
  std::signal(SIGTERM, quit_signal);
  std::signal(SIGHUP, quit_signal);
  std::signal(SIGINT, quit_signal);

  uinput_fd = open_uinput();

  for (int i = 0; i < npins; i++)
  {
    char s[50];
    snprintf(s, sizeof(s), "/sys/class/gpio/gpio%d/value", pins[i]);
    int gpio_fd = open(s, O_RDONLY | O_NONBLOCK);
    if (gpio_fd < 0)
    {
      std::cerr << "Can't open GPIO device " << s << std::endl;
      exit(-1);
    }
    fdset_base[i].fd = gpio_fd;
    fdset_base[i].events = POLLPRI;
  }

  dbglog("Creating GPIO polling thread\n");
  polling_thread = std::thread(polling_thread_func);

  polling_thread.detach();
}

//======================================================================
//======================================================================
void gpio_buttons_stop()
{
  polling_thread.join();
  dbglog("GPIO Cleaning up\n");
  unexport_pins(pins, npins);
  close_uinput(uinput_fd);
}

/*======================================================================
  main
======================================================================*/
/*
int main (int argc, char **argv)
  {
  dbglog ("%s version " VERSION "starting\n", argv[0]);
  int pins[MAX_PINS];
  int npins = 0;
  int edge = EDGE;

  int pin = 0;
  Mapping *m = &mappings[pin];
  while (m->pin != 0)
    {
    pins[npins++] = m->pin;
    pin++;
    m = &mappings[pin];
    };

  dbglog ("Exporting pins\n");
  export_pins (pins, npins);

  // Enable the quit signal handler as soon as anything has been done on
  //   the GPIO: we don't want to leave the GPIO in an odd state
  signal (SIGQUIT, quit_signal);
  signal (SIGTERM, quit_signal);
  signal (SIGHUP, quit_signal);
  signal (SIGINT, quit_signal);

  dbglog ("Opening uinput device\n");
  int uinput_fd = open_uinput(); // Don't need to check return

  struct pollfd fdset[MAX_PINS];
  struct pollfd fdset_base[MAX_PINS];

  // Set up poll FD array for each pin's 'value' pseudo-file
  for (int i = 0; i < npins; i++)
    {
    int pin = pins[i];
    char s[50]; // should be large enough
    snprintf (s, sizeof(s), "/sys/class/gpio/gpio%d/value", pin);
    int gpio_fd = open (s, O_RDONLY|O_NONBLOCK);
    if (gpio_fd < 0)
      {
      fprintf (stderr, "Can't open GPIO device %s\n", s);
      exit(-1);
      }
    fdset_base[i].fd = gpio_fd;
    fdset_base[i].events = POLLPRI;
    }

  time_t start = time(NULL);
  // The type of edge we will detect. Since all switches are rather bouncy,
  //   'both' is probably safest. The debounce mechanism will prevent
  int bounce_time = BOUNCE_MSEC;
  int ticks[MAX_PINS]; // Time of last button press
  memset (ticks, 0, sizeof (int) * MAX_PINS);

  dbglog ("Starting poll\n");
  while (!quit)
    {
    memcpy (&fdset, &fdset_base, sizeof (fdset));
    poll (fdset, npins, 3000);

    for (int i = 0; i < npins; i++)
      {
      if (fdset[i].revents & POLLPRI)
        {
        // For each pin, check for interrupt events
        int pin = pins[i];
        char buff[50];
        // In practice, I've never seen more than two bytes
        //   delivered per interrupt, however many
        //   switch bounces there are
        read (fdset[i].fd, buff, sizeof(buff));

        // If the discrepancy between start and now is too
        //   great, assume that the clock has been fiddled
        //   with
        // If you have a real-time-clock, this test can probably
        //   be removed
        if (abs (time (NULL) - start) > CLOCK_ERROR_SECONDS)
          {
          dbglog ("System time has changed: correcting\n");
          start = time (NULL);
          }
        else
          {
          struct timeval tv;
          gettimeofday (&tv, NULL);
          int msec = tv.tv_usec / 1000;
          int total_msec = (tv.tv_sec  - start) * 1000.0 + msec;
          //printf ("tick %d %d %d\n", pins[i], total_msec, state);
          // The test for total > 1000 is to prevent spurious events
          //   when the program first starts up
          if (total_msec - ticks[i] > bounce_time && total_msec > 1000)
            {
            // We need a small delay here. Even though the last interrupt
            //   received should have been for the desired edge, in practice
            //   it seems that we need to wait a little while for the
            //   sysfs state to settle. I am not sure whether the figure
            //   I have chosen is universally applicable, or whether it
            //   needs to be tweaked.
            usleep (2000);
            int state = get_pin_state (pin);
            if ((state == 0 && (edge & EDGE_FALLING))
                 || (state == 1 && (edge & EDGE_RISING)))
              {
              dbglog ("GPIO state change: pin %d, state %d\n", pin, state);
              button_pressed (uinput_fd, pin, state);
              }
            ticks[i] = total_msec;
            }
          }
        }
      }
    }
  // We only get here if a quit signal has been caught
  dbglog ("Cleaning up\n");
  unexport_pins (pins, npins);
  close_uinput (uinput_fd);
  }
*/
