The Nifty88 is a custom designed two-digit Vacuum Fluorescent Display (VFD) driver and clock.
Powered by an STM32F103 MCU. 
Uses a 12V boost converter and high-side source drivers (TBD62783) to drive the VFD segments and grid, alongside a 1.5V linear regulator for the display filament.
Also includes an LDR for ambient light reactivity and compensation, 2 user buttons, USB C for power, USB 2.0 FS support (pullup PA10 to enable).
Included firmware for a basic clock program, Features automatic and manual brightness adjustment, multiple time display formats.
The firmware was written in C using the HAL. Program uses a non-blocking state machine.
![Nifty88 real image, hero shot](/images/1.jpg)
![Nifty88 real image, hero shot](/images/2.jpg)
