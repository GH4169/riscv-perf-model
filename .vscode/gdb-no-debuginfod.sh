#!/bin/sh

unset DEBUGINFOD_URLS
exec /usr/bin/gdb "$@"
