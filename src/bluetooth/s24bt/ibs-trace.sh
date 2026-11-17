#!/bin/sh
# trace hci_qca's in-band-sleep path while s24-bt attaches (run from the stage-0 shell)
T=/newroot/sys/kernel/tracing
cd $T || exit 1
echo 0 > tracing_on
echo > trace
for p in 'p:s24/enq qca_enqueue' 'p:s24/deq qca_dequeue' 'r:s24/deqret qca_dequeue ret=$retval' \
	'p:s24/awdev qca_wq_awake_device' 'p:s24/awrx qca_wq_awake_rx' 'p:s24/retrans hci_ibs_wake_retrans_timeout' \
	'p:s24/wakeind qca_ibs_wake_ind' 'p:s24/wakeack qca_ibs_wake_ack' 'p:s24/sleepind qca_ibs_sleep_ind' \
	'p:s24/recv qca_recv count=%x2' 'p:s24/ttyrx hci_uart_tty_receive count=%x3'; do
	echo "$p" >> kprobe_events || echo "failed: $p"
done
echo 1 > events/s24/enable
echo 1 > tracing_on
pkill -f 's24-bt up'; sleep 1
(chroot /newroot setsid /root/s24-bt up > /newroot/tmp/s24-bt.log 2>&1 &)
for i in $(seq 1 30); do grep -q -E 'attached as|HCIUART|failed|silent' /newroot/tmp/s24-bt.log 2>/dev/null && break; sleep 1; done
sleep 4
echo 0 > tracing_on
echo 0 > events/s24/enable
grep -v '^#' trace | sed -E 's/^ *//' | awk '{$1=$1; print}' | cut -c1-150 | head -${1:-80}
for e in enq deq deqret awdev awrx retrans wakeind wakeack sleepind recv ttyrx; do echo "-:s24/$e" >> kprobe_events; done
