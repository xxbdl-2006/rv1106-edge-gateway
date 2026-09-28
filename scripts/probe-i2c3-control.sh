#!/bin/sh
# Control experiment for probe-i2c3-pins.sh.
#
# The previous run showed gpio70/71 snapping back to 1 immediately after being
# driven low, which suggests an external pull-up. But to trust that we need to
# know what an *unconnected* pin looks like on this SoC - if a bare pin also
# snaps back, the test proves nothing.
#
# gpio72/73 (GPIO2_B0/B1, header pins 33/21) are a reasonable control: they are
# on the same VCCIO2 rail as the i2c3 pins, and nothing is wired to them here.
# gpio96..100 (GPIO3_A0..A4) are on VCCIO4, also unused.

PINS="70 71 72 73 96 97 98 99 100"

for g in $PINS; do
    echo "$g" > /sys/class/gpio/export 2>/dev/null
done

printf '%-6s %-10s %-12s %s\n' GPIO rest driven_low first_read_after_release
for g in $PINS; do
    [ -d /sys/class/gpio/gpio$g ] || continue

    echo in > /sys/class/gpio/gpio$g/direction
    rest=$(cat /sys/class/gpio/gpio$g/value)

    echo out > /sys/class/gpio/gpio$g/direction
    echo 0   > /sys/class/gpio/gpio$g/value
    sleep 0.3
    low=$(cat /sys/class/gpio/gpio$g/value)

    echo in > /sys/class/gpio/gpio$g/direction
    back=$(cat /sys/class/gpio/gpio$g/value)

    printf '%-6s %-10s %-12s %s\n' "$g" "$rest" "$low" "$back"
done

for g in $PINS; do
    echo "$g" > /sys/class/gpio/unexport 2>/dev/null
done
