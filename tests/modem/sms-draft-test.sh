#!/bin/sh
# create a draft SMS through mmcli (never sent), look at it, delete it
mmcli -m 0 --messaging-create-sms="number='+15555550100',text='test draft, never sent: Zürich ✓'" 2>&1
p=$(mmcli -m 0 --messaging-list-sms 2>/dev/null | grep -o '/org/freedesktop/ModemManager1/SMS/[0-9]*' | head -1)
echo "draft: $p"
mmcli -s "$p" 2>&1 | grep -E 'number|text|state|pdu type|storage'
mmcli -m 0 --messaging-delete-sms="$p" 2>&1
mmcli -m 0 --messaging-list-sms 2>&1
