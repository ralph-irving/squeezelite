#!/bin/sh

# Define GPIO
GPIO_OUT=18

# Turn on, and turn off functions
turn_on() {
echo "1" > /sys/class/gpio/gpio$GPIO_OUT/value
}          
 
turn_off() {
echo "0" > /sys/class/gpio/gpio$GPIO_OUT/value
}

init_gpio_out() {
#================================================= ==========================
# Initial GPIO OUT setup                                                        
#---------------------------------------------------------------------------
sudo sh -c 'echo '"$GPIO_OUT"' > /sys/class/gpio/export'                        
# Relay is active low, so this reverses the logic                           
sudo sh -c 'echo "1" > /sys/class/gpio/gpio'"$GPIO_OUT"'/active_low'            
sudo sh -c 'echo "out" > /sys/class/gpio/gpio'"$GPIO_OUT"'/direction'           
sudo sh -c 'echo "0" > /sys/class/gpio/gpio'"$GPIO_OUT"'/value'                                  
#---------------------------------------------------------------------------
}

case "${1}" in

# 2 from cmdline is for first initialization commands, this is run once
2)
init_gpio_out
;;

# 0 from cmdline is for Off Commands
0)
        turn_off
;;

# 1 from cmdline is for On Commands
1)
        turn_on
;;
esac
