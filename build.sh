#!/bin/sh


S=`pwd`
builddir=build

imagedir=$S/image

D=$imagedir
sysconfdir=${sysconfdir}
includedir=/usr/include
bindir=/usr/bin

#cleanup builddir
rm -rf $builddir

#cleanup imagedir
rm -rf $imagedir

# configure
meson  --prefix /usr               --buildtype plain               --bindir bin               --sbindir sbin               --datadir share               --libdir lib               --libexecdir libexec               --includedir include               --mandir share/man               --infodir share/info               --sysconfdir /etc               --localstatedir /var               --sharedstatedir /com               --wrap-mode nodownload     -Dhal-compat=false              -Dorc=disabled          -Daccess_group=audio            -Dopenssl=disabled              -Ddatabase=simple               -Dzshcompletiondir=no           -Dudevrulesdir=`pkg-config --variable=udevdir udev`/rules.d             -Dvalgrind=disabled             -Dtests=false           -Drunning-from-build-tree=false  -Dsoxr=disabled -Dfftw=disabled -Dadrian-aec=false  -Davahi=disabled -Dbluez5=disabled -Ddbus=disabled -Dgsettings=disabled -Dgtk=disabled -Dipv6=false -Djack=disabled -Dlirc=disabled -Dman=false -Dbluez5-ofono-headset=false -Dpalm-resampler=true -Dsystemd=disabled -Dwebrtc-aec=disabled -Dx11=disabled $builddir

# compile
ninja -C $builddir

# install image
mkdir -p $imagedir

echo "run ninja install"
cd $builddir
DESTDIR=$imagedir ninja -v -j 8 install 

install -d ${D}${sysconfdir}/pulse
install -d ${D}${sysconfdir}/default/volatiles
install -d ${D}${sysconfdir}/event.d
install -d ${D}${includedir}/pulse
install -d ${D}${sysconfdir}/systemd/system

install -v -m 644 ${S}/palm/starfish_upstart-pulseaudio ${D}${sysconfdir}/event.d/pulseaudio

install -v -m 644 ${S}/src/modules/module-palm-policy-default.h ${D}${includedir}/pulse/module-palm-policy.h
install -v -m 644 ${S}/src/modules/module-palm-policy-tables-default.h ${D}${includedir}/pulse/module-palm-policy-tables.h

install -v -m 0644 volatiles.04_pulse  ${imagedir}/etc/default/volatiles/04_pulse
install -v -d ${imagedir}/etc/systemd/system
install -v -m 644 ${imagedir}/etc/systemd/system/
install -v -m 644 ${S}/src/modules/module-palm-policy-default.h ${imagedir}/usr/include/pulse/module-palm-policy.h
install -v -m 644 ${S}/src/modules/module-palm-policy-tables-default.h ${imagedir}/usr/include/pulse/module-palm-policy-tables.h
install -v -d ${imagedir}/usr/lib/pulse-15.0/modules/ecnr
install -d ${imagedir}/etc/pulse
install -v -m 644 ${S}/palm/starfish_system.pa ${imagedir}/etc/pulse/system.pa
install -v -m 644 ${S}/palm/starfish_asound.conf ${imagedir}/etc/asound.conf

install -d ${imagedir}/etc/event.d
install -v -m 644 ${S}/palm/starfish_upstart-pulseaudio ${imagedir}/etc/event.d/pulseaudio

install -v -m 644 ${S}/src/modules/module-palm-policy-default.h ${imagedir}/usr/include/pulse/module-palm-policy.h
install -v -m 644 ${S}/src/modules/module-palm-policy-tables-default.h ${imagedir}/usr/include/pulse/module-palm-policy-tables.h

chmod -v o-rwx ${D}${bindir}/pulseaudio
