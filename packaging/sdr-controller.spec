%global _version %{getenv:PACKAGE_VERSION}

Name:           sdr-controller
Version:        %{_version}
Release:        1%{?dist}
Summary:        OpenRFStack SdrResourceManager — SDR hardware arbitration and task scheduler
License:        Proprietary
URL:            https://github.com/OpenRFStack/SdrResourceManager

BuildRequires:  cmake >= 3.20
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(SoapySDR)
BuildRequires:  qpid-proton-cpp-devel
BuildRequires:  tinyxml2-devel
BuildRequires:  fftw-devel
BuildRequires:  spdlog-devel
BuildRequires:  fmt-devel
BuildRequires:  libuuid-devel
BuildRequires:  git

Requires:       SoapySDR
Requires:       qpid-proton-cpp
Requires:       tinyxml2
Requires:       fftw
Requires:       spdlog
Requires:       fmt
Requires:       libuuid

%description
SdrResourceManager is the hardware arbitration layer for OpenRFStack.
It owns all USB SDR devices on a host, accepts SCAN/NARROWBAND/DF task
requests over AMQP 1.0, enforces priority preemption, and streams raw
CF32 IQ data to clients over UDP.

Supports: RTL-SDR, PlutoSDR (Ethernet + USB), LimeSDR, HackRF, USRP B210.

%build
cmake -B %{_builddir}/build -S %{_sourcedir} -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build %{_builddir}/build --parallel

%install
DESTDIR=%{buildroot} cmake --install %{_builddir}/build

%files
%license LICENSE
/usr/bin/sdr_controller

%changelog
* Thu Jan 01 2026 OpenRFStack <noreply@github.com> - %{_version}-1
- Packaged by CI
