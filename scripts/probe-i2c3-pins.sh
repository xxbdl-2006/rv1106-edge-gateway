#!/bin/sh
# Probe the electrical state of the I2C3_M0 pins (GPIO2_A6 = gpio70 = header
# pin 24, GPIO2_A7 = gpio71 = header pin 14) without enabling the i2c3
# controller.
#
# Method: drive the pin low as an output, then release it back to input and
# immediately read. A pin with an external pull-up (an MPU6050 breakout ships
# with 4.7k to its own VCC) snaps back to 1 at once. A pin with nothing
# attached only creeps back as the internal bias and leakage overcome the
# trace capacitance, so it sits at 0 for a while.

GPIO_A6=70
GPIO_A7=71

for g in $GPIO_A6 $GPIO_A7; do
    echo "$g" > /sys/class/gpio/export 2>/dev/null
    [ -d /sys/class/gpio/gpio$g ] || { echo "cannot export gpio$g"; exit 1; }
done

echo "--- step 1: release and read the resting level ---"
for g in $GPIO_A6 $GPIO_A7; do
    echo in > /sys/class/gpio/gpio$g/direction
    echo "  gpio$g resting = $(cat /sys/class/gpio/gpio$g/value)"
done

echo "--- step 2: drive both low as outputs ---"
for g in $GPIO_A6 $GPIO_A7; do
    echo out > /sys/class/gpio/gpio$g/direction
    echo 0   > /sys/class/gpio/gpio$g/value
done
sleep 1
for g in $GPIO_A6 $GPIO_A7; do
    echo "  gpio$g driven low = $(cat /sys/class/gpio/gpio$g/value)"
done

echo "--- step 3: release to input and sample immediately ---"
for g in $GPIO_A6 $GPIO_A7; do
    echo in > /sys/class/gpio/gpio$g/direction
done

i=0
while [ $i -lt 6 ]; do
    a=$(cat /sys/class/gpio/gpio$GPIO_A6/value)
    b=$(cat /sys/class/gpio/gpio$GPIO_A7/value)
    echo "  sample $i: SCL(gpio$GPIO_A6)=$a  SDA(gpio$GPIO_A7)=$b"
    i=$((i + 1))
done

# Leave the pins as inputs so the i2c controller can claim them cleanly later.
for g in $GPIO_A6 $GPIO_A7; do
    echo "$g" > /sys/class/gpio/unexport 2>/dev/null
done
