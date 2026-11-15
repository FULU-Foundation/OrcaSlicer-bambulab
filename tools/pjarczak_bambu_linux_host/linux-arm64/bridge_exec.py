#!/usr/bin/env python3
"""Exec a command with every inherited descriptor above stderr closed.

OrcaSlicer opens its log file without close-on-exec, so the log descriptor
leaks into the bridge wrapper and from there into muvm/passt.  SELinux then
denies passt_t writing to config_home_t, which can make passt bail out before
the guest ever starts.  Dropping those descriptors keeps muvm clean.
"""
import os
import sys

for fd in range(3, 256):
    try:
        os.close(fd)
    except OSError:
        pass

os.execvp(sys.argv[1], sys.argv[1:])
