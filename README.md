# About
PODFBV is a tool to exchange and transcode MIDI messages between the *Line 6 FBV Express MKII* floor-board and the *Line 6 Pocket POD* guitar multi-effect processor. The program is executed on a Linux-based host computer (e.g. *Raspberry PI*) with both *Line 6*-devices connected via USB.

Currently, only basic functionality of the floor-board is covered, i.e. bank switching, tapping, volume and wah-wah. Enable or disable the tuner is not supported.
The floor-board requires to be programmed according to the scheme <a href=https://github.com/kurzlo/podfbv/blob/master/misc/fbv_settings.fbv>fbv_setting.fbv</a>.
Respective *Line 6* tools are referenced below.

The program can either be executed as a command-line application or as a Linux daemon.

# Build and execute

## Build executable
Execute GNU makefile \
**$ make all** \
on the target machine to build the executable.

A cross-compile prefix (e.g. for ARM 64 bit engines) can also be provided \
**$ CROSS_COMPILE=aarch64-linux-gnu- make all** \
to build the program on a remote machine.

## Command line application
Run \
**$ make run** \
to execute on terminal.

Per default the program scans the "/dev/snd/by-id/" folder for respective device soft-links (i.e. "usb-Line_6_FBV_Express_Mk_II-00" and "usb-Line_6_Line_6_Pocket_POD-00") and opens respective MIDI interfaces one level above.

Soft-links are specified manually by using command-line switches "--fbv_id \<name>" and "--pod_id \<name>": \
**$ ARGS="--fbv_id usb-Line_6_FBV_Express_Mk_II-00 --pod_id usb-Line_6_Line_6_Pocket_POD-00" make run**

Absolute paths without resolving softlinks can be provided, e.g. \
**$ ARGS="--fbv_dev /dev/midi1 --pod_dev /dev/midi2" make run**

## Daemon
Run \
**$ ARGS=-d make run** \
to run as daemon.

A "sytemctl" service script is available at <a href=https://github.com/kurzlo/podfbv/blob/master/podfbv.service>podfpv.service</a>.
The script can be installed with the following command: \
**$ systemctl enable <path_to_script_file>/podfbv.service**

Afterwards the service can be started, stopped and monitored using \
**$ service podfpv [start|stop|status]**

# Disclaimer
The software is distributed in the hope that it will be useful but **WITHOUT ANY WARRANTY**;
without even the implied warranty of **MERCHANTABILITY** or **FITNESS FOR A PARTICULAR PURPOSE**.

# References
* <a href=https://line6.com/data/l/0a06000f1344c45ecbcd6e0293/application/pdf/MIDI%20Continuous%20Controller%20Reference%20>*Line 6* MIDI CC-Reference</a>
* <a href=https://de.line6.com/software/index.html>*Line 6* Software downloads</a>
  * *Line 6 FBV Control* is required to configure the FBV board.
  * *Vyzex Editor* is a helpful tool to control the POD and observe MIDI transactions exchanged with a host PC.
