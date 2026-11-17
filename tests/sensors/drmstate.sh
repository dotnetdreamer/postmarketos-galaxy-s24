#!/bin/sh
# print exynos-crtc-0 enable/active/self_refresh_active
awk '/^crtc\[/{c=$2} c=="exynos-crtc-0" && /enable=|active=|self_refresh_active=/{printf "%s ", $1} /^crtc\[/ && c!="exynos-crtc-0"{if(p)exit} c=="exynos-crtc-0"{p=1}' /newroot/sys/kernel/debug/dri/1/state; echo
