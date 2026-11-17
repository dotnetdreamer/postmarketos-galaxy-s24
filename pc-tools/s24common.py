"""Shared connection settings for the PC tools that reach the phone over SSH (Wi-Fi or the cable).

Set these in your shell before using s24ssh.py / s24put.py / s24pull.py:
  S24_HOST   the phone's address, e.g. 172.16.42.1 on the USB cable or its Wi-Fi address
  S24_USER   the postmarketOS user (default "user")
  S24_PASS   that user's password (asked for if not set; it is also what sudo wants)

postmarketOS on this phone runs dropbear, which has no SFTP: files move through `cat` on an
exec channel instead.
"""
import getpass
import os
import sys

import paramiko


def settings():
    host = os.environ.get("S24_HOST")
    if not host:
        sys.exit("set S24_HOST to the phone's address (172.16.42.1 on the USB cable)")
    user = os.environ.get("S24_USER", "user")
    pw = os.environ.get("S24_PASS") or getpass.getpass("password for %s@%s: " % (user, host))
    return host, user, pw


def connect():
    host, user, pw = settings()
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(host, username=user, password=pw, timeout=10, allow_agent=False, look_for_keys=False)
    return c, pw
