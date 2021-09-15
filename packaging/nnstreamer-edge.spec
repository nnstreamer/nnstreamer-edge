%define     sensor_test 0

Name:       nnstreamer-edge
Summary:    Common library set for nnstreamer-edge
Version:    0.0.1
Release:    1
Group:      Machine Learning/ML Framework
Packager:   Sangjung Woo <sangjung.woo@samsung.com>
License:    Apache-2.0
Source0:    %{name}-%{version}.tar.gz
Source1001: nnstreamer-edge.manifest

BuildRequires:  cmake
BuildRequires:  pkgconfig(paho-mqtt-c)
%if 0%{?sensor_test}
BuildRequires:  gtest-devel
%endif

%description
nnstreamer-edge provides remote source nodes for NNStreamer pipelines without GStreamer dependencies.
It also contains communicaton library for sharing server node information & status

%package sensor
Summary: communication library for edge sensor
%description sensor
It is a communication library for edge sensor devices.
This library supports publishing the sensor data to the GStreamer pipeline without GStreamer / Glib dependency.

%package sensor-test
Summary: test program for nnstreamer-edge-sensor library
%description sensor-test
It is a test program for nnstreamer-edge-sensor library.
It read the jpeg data and publishes it as "TestTopic" topic name 10 times.
If data is successfully received, then the image is shown on the server-side.

%package sensor-devel
Summary: development package for nnstreamer-edge-sensor
Requires: nnstreamer-edge = %{version}-%{release}
%description sensor-devel
It is a development package for nnstreamer-edge-sensor.

%define enable_sensor_test -DENABLE_TEST=OFF
%if 0%{?sensor_test}
%define enable_sensor_test -DENABLE_TEST=ON
%endif

%prep
%setup -q
cp %{SOURCE1001} .

%build
mkdir -p build
pushd build
%cmake .. \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DVERSION=%{version} %{enable_sensor_test}

make %{?jobs:-j%jobs}
popd

%install
rm -rf %{buildroot}
pushd build
%make_install
popd

%clean
rm -rf %{buildroot}

%files sensor
%manifest nnstreamer-edge.manifest
%defattr(-,root,root,-)
%{_libdir}/libedge-sensor.so*

%if 0%{?sensor_test}
%files sensor-test
%manifest nnstreamer-edge.manifest
%defattr(-,root,root,-)
%{_bindir}/test_edge_sensor
%endif

%files sensor-devel
%{_includedir}/edge_sensor.h
%{_libdir}/pkgconfig/nnstreamer-edge-sensor.pc
