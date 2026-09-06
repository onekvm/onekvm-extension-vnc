SUMMARY = "OneKVM VNC protocol extension"
HOMEPAGE = "https://github.com/onekvm/onekvm-extension-vnc"
LICENSE = "GPL-2.0-only & GPL-3.0-only"
ONEKVM_LIBVNCSERVER_SOURCE ?= "${UNPACKDIR}/libvncserver"
LIC_FILES_CHKSUM = "\
    file://LICENSE;md5=75859989545e37968a99b631ef42722e \
    file://LICENSES/Papirus-GPL-3.0.txt;md5=1ebbd3e34237af26da5dc08a4e440464 \
    file://${ONEKVM_LIBVNCSERVER_SOURCE}/COPYING;md5=361b6b837cad26c6900a926b62aada5f \
"
PR = "r3"

ONEKVM_COMPONENT_GIT_URI ??= "git://github.com/onekvm/onekvm-extension-vnc.git;protocol=https;branch=main"
ONEKVM_COMPONENT_SRCREV ??= ""
SRC_URI = "\
    ${ONEKVM_COMPONENT_GIT_URI};name=extension;destsuffix=extension \
    git://github.com/LibVNC/libvncserver;protocol=https;branch=master;name=libvncserver;destsuffix=libvncserver \
"
SRCREV_extension = "${ONEKVM_COMPONENT_SRCREV}"
SRCREV_libvncserver = "9b54b1ec32731bd23158ca014dc18014db4194c3"
SRCREV_FORMAT = "extension_libvncserver"
S = "${UNPACKDIR}/extension"

inherit cmake pkgconfig onekvm-extension

DEPENDS = "json-c jpeg zlib"
CFLAGS:append = " -ffile-prefix-map=${UNPACKDIR}=/usr/src/debug/${PN}/${PV}"
CXXFLAGS:append = " -ffile-prefix-map=${UNPACKDIR}=/usr/src/debug/${PN}/${PV}"

ONEKVM_EXTENSION_ID = "vnc"
ONEKVM_EXTENSION_API_VERSION = "2"
ONEKVM_EXTENSION_MANIFEST_TEMPLATE = "${S}/manifest.json.in"
RDEPENDS:${PN} += "onekvm-core (>= 0.1.0-r1)"
# jpeg_rgb plus LibVNCServer Tight need libjpeg. The Cube rootfs does not
# ship it, and plugin-manager rejects a libjpeg62 Depends. Bundle the
# shared library inside the extension payload instead.
PRIVATE_LIBS:${PN} = "libjpeg.so.62"
# Keep the bundled jpeg out of a directory named lib, otherwise package.bbclass
# adds an ldconfig postinst and plugin-manager rejects the IPK.
OECMAKE_RPATH = "${ONEKVM_EXTENSION_ROOT}/libjpeg"
INSANE_SKIP:${PN} += "already-stripped"

EXTRA_OECMAKE = "\
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DONEKVM_LIBVNCSERVER_SOURCE=${ONEKVM_LIBVNCSERVER_SOURCE} \
    -DONEKVM_BUILD_TESTS=ON \
    -DCMAKE_INSTALL_PREFIX=${ONEKVM_EXTENSION_ROOT} \
    -DONEKVM_EXTENSION_VERSION=${PV} \
    -DONEKVM_EXTENSION_ARCH=${TARGET_ARCH} \
"

do_install:append() {
    install -d ${D}${ONEKVM_EXTENSION_ROOT}/libjpeg
    jpeg_lib=$(readlink -f ${STAGING_LIBDIR}/libjpeg.so.62)
    install -m 0755 "$jpeg_lib" ${D}${ONEKVM_EXTENSION_ROOT}/libjpeg/$(basename "$jpeg_lib")
    ln -sf "$(basename "$jpeg_lib")" ${D}${ONEKVM_EXTENSION_ROOT}/libjpeg/libjpeg.so.62
}
