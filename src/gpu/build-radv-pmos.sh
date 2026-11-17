#!/bin/sh
# Galaxy S24 postmarketOS: build the Xclipse 940 Vulkan driver (RADV from the radv-xclipse port,
# Mesa 27.0.0-devel) natively on the phone, for musl and Wayland/X11 (Claude, 2026-11-14).
# Same pinned sources and patches as open-android-dex/openandroiddex-linux/radv-xclipse (the glibc
# guest build). Run as root inside the postmarketOS root. Output: /opt/xclipse.
set -eu
W=/root/radvx-build
RADVX_REPO=https://github.com/JimVulkan/radv-xclipse.git
RADVX_COMMIT=269238e8538c971cbe5195ed3e0013dea59f3bf5
MESA_UPSTREAM_COMMIT=c823a83e8f373a5c36c2b9ed1d7dc98a4f3360ee
mkdir -p "$W"
cd "$W"

echo "== packages"
apk add --no-progress build-base git meson ninja python3 py3-mako py3-yaml py3-packaging \
	bison flex glslang linux-headers curl patch pkgconf \
	libdrm-dev wayland-dev wayland-protocols libx11-dev libxcb-dev libxrandr-dev libxext-dev \
	libxshmfence-dev xcb-util-keysyms-dev zlib-dev expat-dev \
	vulkan-loader vulkan-tools

echo "== source"
if [ ! -f radvx/.fetched ]; then
	rm -rf radvx
	git init -q radvx
	git -C radvx fetch -q --depth 1 "$RADVX_REPO" "$RADVX_COMMIT"
	git -C radvx checkout -q FETCH_HEAD
	test "$(git -C radvx rev-parse HEAD)" = "$RADVX_COMMIT"
	(cd radvx && while read -r _sum path; do
		mkdir -p "$(dirname "$path")"
		curl -fsSL --retry 3 --connect-timeout 30 -o "$path" \
			"https://gitlab.freedesktop.org/mesa/mesa/-/raw/${MESA_UPSTREAM_COMMIT}/$path"
	done < "$W/upstream-files.sha256" && sha256sum -c "$W/upstream-files.sha256")
	for p in "$W"/patches/*.patch; do
		echo "patch $(basename "$p")"
		patch -d radvx -p1 --forward < "$p"
	done
	touch radvx/.fetched
fi

echo "== configure"
rm -rf build
meson setup build radvx --prefix /opt/xclipse --buildtype release -Db_ndebug=true \
	-Dplatforms=wayland,x11 \
	-Dvulkan-drivers=amd -Dgallium-drivers= -Dopengl=false \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
	-Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dlmsensors=disabled -Dzstd=disabled -Dvideo-codecs= -Dvulkan-layers= -Dtools= \
	-Dbuild-tests=false -Dspirv-tools=disabled -Ddisplay-info=disabled \
	-Dandroid-libbacktrace=disabled -Dintel-rt=disabled -Dperfetto=false \
	-Dallow-fallback-for=libdrm --force-fallback-for=libdrm \
	-Dlibdrm:default_library=static \
	-Dc_args=-march=armv8.2-a -Dcpp_args=-march=armv8.2-a

echo "== build"
ninja -C build src/amd/vulkan/libvulkan_radeon.so

echo "== install"
mkdir -p /opt/xclipse/lib /usr/share/vulkan/icd.d
strip -o /opt/xclipse/lib/libvulkan_xclipse.so build/src/amd/vulkan/libvulkan_radeon.so
cat > /usr/share/vulkan/icd.d/xclipse_icd.json <<EOF
{
    "ICD": {
        "api_version": "1.4.335",
        "library_path": "/opt/xclipse/lib/libvulkan_xclipse.so"
    },
    "file_format_version": "1.0.1"
}
EOF
ls -la /opt/xclipse/lib
echo "== BUILD-OK"
