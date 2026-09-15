# Maintainer: Davide Del Grande (ddg)
# Fork of the official Fedora ckb-next package (src.fedoraproject.org/rpms/ckb-next)
# Upstream: https://github.com/ckb-next/ckb-next
# Fork:     https://github.com/davidedg/ckb-next-ddg
#
# Local build/test (in a disposable Fedora container, e.g. via podman):
#   podman run --rm -v "$PWD/packaging/fedora:/src/fedora:Z" registry.fedoraproject.org/fedora:latest bash -c '
#     dnf -y builddep /src/fedora/ckb-next-ddg.spec &&
#     rpmbuild -bb /src/fedora/ckb-next-ddg.spec --define "_sourcedir /src/fedora" --define "_topdir /tmp/rpmbuild"'

# Upstream base version this fork tracks -- bump alongside Version, but only
# when the ".ddg.N" part of Version does not touch it.
%global base_version 0.6.2

Name:           ckb-next-ddg
Version:        0.6.2.ddg.4
Release:        1%{?dist}
Summary:        Corsair Keyboard and Mouse RGB Driver -- ddg fork (device-scoped profile/mode CLI, WITH_ENV_VARS)

License:        GPL-2.0-only

URL:            https://github.com/davidedg/ckb-next-ddg
# Repo name now matches the package name, so the GitHub release archive and
# its root directory follow the same name-version pattern as the RPM default.
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

# Upstream (and the official Fedora package) provide none of the following files
Source1:        ckb-next.appdata.xml
Source2:        ckb-next.1
Source3:        99-ckb-next.preset

# Build against the system kissfft instead of the bundled copy in
# src/libs/kissfft (used by the mviz animation) -- same as the official
# Fedora package. Regenerated against this fork's tree: the original Fedora
# patch no longer applies cleanly to src/libs/CMakeLists.txt because of the
# WITH_HWSENSOR/cjson block added downstream of it.
Patch1:         0001-unbundle-kissfft.patch

BuildRequires:  cmake
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  libappstream-glib

BuildRequires:  cmake(kissfft)
BuildRequires:  cmake(Qt6)
BuildRequires:  cmake(Qt6LinguistTools)
BuildRequires:  cmake(QuaZip-Qt6)
BuildRequires:  cmake(zlib)

BuildRequires:  libappindicator-devel
BuildRequires:  libgudev-devel
BuildRequires:  libxcb-devel
BuildRequires:  pulseaudio-libs-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  xcb-util-devel
BuildRequires:  xcb-util-wm-devel

BuildRequires:  systemd-devel
%{?systemd_requires}

Requires:       qt6-qtbase
Requires:       hicolor-icon-theme

Provides:       ckb-next = %{base_version}
Conflicts:      ckb-next


%description
ckb-next is an open-source driver for Corsair keyboards and mice. It aims to
bring the features of their proprietary CUE software to the Linux operating
system. This ddg fork adds a device-scoped profile/mode CLI and an optional
WITH_ENV_VARS build option exposing CKBNEXT_* environment variables to
programs launched from key bindings.


%prep
%autosetup -p1

# Remove the bundled copy now that we build against system kissfft (Patch1)
rm -rf src/libs/kissfft

# Fedora uses /usr/libexec for daemons; the daemon target itself always
# installs to bindir regardless of CMAKE_INSTALL_LIBEXECDIR (see %%install).
sed -e '/^ExecStart/cExecStart=%{_libexecdir}/ckb-next-daemon' -i linux/systemd/ckb-next-daemon.service.in

# git-describe-based version detection needs a .git checkout (see
# cmake/modules/CkbNextDetermineVersion.cmake); we build from a plain
# tarball, so pin the version explicitly instead of showing "-unknown".
sed -e 's/^set(ckb-next_VERSION "[^"]*")/set(ckb-next_VERSION "%{version}")/' \
    -e 's/^set(ckb-next_VERSION_IS_RELEASE FALSE)/set(ckb-next_VERSION_IS_RELEASE TRUE)/' \
    -i VERSION.cmake


%build
%cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_INSTALL_LIBEXECDIR=libexec \
  -DUDEV_RULE_DIRECTORY=%{_udevrulesdir} \
  -DDISABLE_UPDATER=1 \
  -DFORCE_INIT_SYSTEM=systemd \
  -DSAFE_INSTALL=OFF \
  -DSAFE_UNINSTALL=OFF \
  -DPREFER_QT6=ON \
  -DWITH_ENV_VARS=ON \

%cmake_build


%install
%cmake_install

# The daemon target installs to bindir regardless of CMAKE_INSTALL_LIBEXECDIR
mv %{buildroot}%{_bindir}/ckb-next-daemon %{buildroot}%{_libexecdir}/ckb-next-daemon

install -Dp -m 0644 %{SOURCE3}  %{buildroot}%{_presetdir}/99-ckb-next.preset
install -Dp -m 0644 %{SOURCE1}  %{buildroot}%{_datadir}/metainfo/ckb-next.appdata.xml
install -Dp -m 0644 %{SOURCE2}  %{buildroot}%{_mandir}/man1/ckb-next.1


%check
desktop-file-validate %{buildroot}%{_datadir}/applications/ckb-next.desktop
appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/ckb-next.appdata.xml


%post
%systemd_post ckb-next-daemon.service
if [ $1 -eq 1 ]; then
    systemctl start ckb-next-daemon.service >/dev/null 2>&1 || :
fi
udevadm control --reload-rules 2>&1 > /dev/null || :


%preun
%systemd_preun ckb-next-daemon.service


%postun
%systemd_postun_with_restart ckb-next-daemon.service
udevadm control --reload-rules 2>&1 > /dev/null || :


%files
%license LICENSE
%doc CHANGELOG.md FIRMWARE README.md
%{_bindir}/ckb-next
%{_bindir}/ckb-next-dev-detect
%{_libexecdir}/ckb-next-daemon
%{_libexecdir}/ckb-next-sinfo
%{_libexecdir}/ckb-next-animations/
%{_libdir}/cmake/ckb-next/
%{_datadir}/applications/ckb-next.desktop
%{_sysconfdir}/xdg/autostart/ckb-next.desktop
%{_datadir}/metainfo/ckb-next.appdata.xml
%{_datadir}/icons/hicolor/**/apps/ckb-next.png
%{_datadir}/icons/hicolor/**/apps/ckb-next-monochrome.png
%{_datadir}/icons/hicolor/**/status/ckb-next_battery*.png
%{_mandir}/man1/ckb-next.1*
%{_presetdir}/99-ckb-next.preset
%{_udevrulesdir}/*.rules
%{_unitdir}/ckb-next-daemon.service


%changelog
* Tue Sep 15 2026 Davide Del Grande <delgrande.davide@gmail.com> - 0.6.2.ddg.4-1
- Initial ddg fork RPM package, based on the official Fedora ckb-next.spec (0.6.2-7)
