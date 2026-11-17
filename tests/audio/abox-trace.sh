#!/bin/sh
# Galaxy S24: trace what the ABOX DSP tells the kernel during a playback (Claude, 2026-11-15).
# Run in the stage-0 shell:  sh abox-trace.sh on|off|show
T=/newroot/sys/kernel/tracing
case "$1" in
on)
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo > $T/kprobe_events 2>/dev/null
	M=snd_soc_samsung_abox
	{
	echo "p:abox/ipc_irq $M:abox_ipc_irq_handler"
	echo "p:abox/sys_ipc $M:abox_ipc_handler id=%x0:s32 type=+8(%x2):s32 p1=+12(%x2):s32 p2=+16(%x2):s32 p3=+20(%x2):s32"
	echo "p:abox/vdma_ipc $M:abox_vdma_ipc_handler id=%x0:s32 type=+8(%x2):s32 ch=+12(%x2):s32 ptr=+16(%x2):u32"
	echo "p:abox/rdma_ipc $M:abox_rdma_ipc_handler id=%x0:s32 type=+8(%x2):s32 ch=+12(%x2):s32 ptr=+16(%x2):u32"
	echo "p:abox/send $M:abox_ipc_send id=+0(%x1):s32 task=+4(%x1):s32 type=+8(%x1):s32 ch=+12(%x1):s32 a=+16(%x1):u32 b=+20(%x1):u32 c=+24(%x1):u32"
	echo "p:abox/send_atomic $M:abox_ipc_send_atomic id=+0(%x1):s32 task=+4(%x1):s32 type=+8(%x1):s32 ch=+12(%x1):s32 a=+16(%x1):u32"
	echo "p:abox/elapsed $M:abox_vdma_period_elapsed ptr=%x2:u64"
	echo "r:abox/vptr $M:abox_vdma_pointer ret=\$retval:u64"
	} > $T/kprobe_events || { cat $T/error_log | tail -5; exit 1; }
	echo 1 > $T/events/abox/enable
	echo 1 > $T/tracing_on
	echo on
	;;
off)
	echo 0 > $T/tracing_on
	echo 0 > $T/events/abox/enable 2>/dev/null
	echo > $T/kprobe_events
	echo off
	;;
show)
	cat $T/trace | grep -v "^#" | sed -E 's/^ +//' | cut -c1-200
	;;
esac
