# pi\_button\_to\_kdb

Version 0.0.1, December 2023

## What is this

`pi_button_to_kdb` is a demonstration program for the Raspberry Pi that
monitors for GPIO pin events (e.g., buttons being pushed), and issues keyboard
events. That is, it maps GPIO pushbuttons to lists of keystrokes. 
It is written in C, and designed to be compiled using GCC.

The purpose of this program is to aid in using GPIO pushbuttons with 
programs that would otherwise use keyboard or terminal input.

This is not a general-purpose program -- it just demonstrates an approach that
might be taken. The keyboard mappings and GPIO pins are hard-coded in the
program, and would need to be modified to suit a particular application. I
think this would be relatively easy to to, but does require some C programming
experience. The program could be made general-purpose by having it read a list
of keyboard mappings from a file. However, I have no need for such a facility,
and no time to implement it.

For the record, the program in its unmodified state generates a 'space'
keyboard event when GPIO 20 goes low, and a 'ctrl-R' event when GPIO 21 goes
low. Of course it's possible to choose different GPIO pins, or detect the
rising rather than falling edge, with code changes.

Don't forget that in recent Pi models, GPIO pins 9 and higher are configured to
pull down at boot time. If you want to use the falling edge, which generally I
prefer (not sure why), you'll need to attach a pull-up resistor of, say, 5k to
3.3V. Alternatively, you can change the pull direction in the `config.txt` file
in the boot partition: 

    gpio=20,21=pu

This program uses the <code>uinput</code> kernel module, which is probably
not loaded by default on a Raspberry Pi. So you'll need to arrange
for this to be loaded:

    $ sudo modprobe uinput

The spoofed keyboard input is fed directly into the kernel's event-handling
machinery. They keycodes will go to wherever they would go if a user were
typing on an attached keyboard.  This method of keyboard spoofing does not
provide any way to target keystrokes at a specific application.

Please note that the keycodes used in this application are *scan codes*, not
ASCII codes. If you want to generate a multi-key sequence with shift, ctrl,
alt, etc., you'll need to spoof the key-down and key-up scan codes for all
these keys, in the right order. 

## Building and installing

Just the usual:

    $ make
    $ sudo make install

Of course, you'll need to edit the code to suit your specific application's
requirements.

## Notes

This program almost certainly needs to run with `root` permissions. 

There is a list of keyboard scan codes in
`/usr/include/linux/input-event-codes.h`

GPIO state changes are detected using interrupts, not polling. This program
should use essentially zero CPU.

The program uses the archaic `/sys/class/gpio` interface. It still works fine,
and is easier to use that any of the modern alternatives.

The program proudces no console output in normal circumstances. To get
debugging output, change the value of `DEBUG`, around line 74 in `main.c`.

## Author and legal

This example program is maintained by Kevin Boone, and released under
the terms of the GPL v3.0 in the hope that it is useful. There is, of
course, no warranty of any kind.

## Revision history

0.0.1 December 2023
- First working release


