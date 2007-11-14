### begin RPM spec
%define name openisr
%define version 0.8.4

Summary: 	OpenISR Internet Suspend-Resume client
Name: 		%name
Version: 	%version
Release: 	2%{?redhatvers:.%{redhatvers}}
Group: 		Applications/Internet
License:	Eclipse Public License	
BuildRequires: 	curl-devel
Requires: 	openssh, rsync, pv, dkms
BuildRoot: 	/var/tmp/%{name}-buildroot
Packager:	Matt Toups <mtoups@cs.cmu.edu>

URL:		http://isr.cmu.edu
Source: 	http://isr.cmu.edu/software/openisr-%{version}.tar.gz
# line below is working around an annoying rpm "feature"
Patch0:		dkms.patch
Patch1:		openisr-config.patch
Provides:	perl(IsrRevision)

%description
 OpenISR is the latest implementation of Internet Suspend/Resume, which
 combines a virtual machine with distributed storage to provide the user
 with both mobility and consistent state without the need for mobile hardware.
 This package contains a client (isr), a parcel manager (Vulpes), a wrapper
 library for VMware (libvdisk), and the source to the openisr kernel module
 (Nexus).  A virtual machine monitor (VMware, Xen, KVM, etc) is not included
 in this package and should also be installed.  OpenISR is developed at 
 Carnegie Mellon University.

%prep
%setup -q
%patch0
%patch1

%build
./configure --enable-client --disable-modules --prefix=/usr --sysconfdir=/etc && make DESTDIR=%{buildroot}
 make dist

%install
make install DESTDIR=%{buildroot}
mkdir -p %{buildroot}/usr/src && cd %{buildroot}/usr/src && tar zxvf ../share/openisr/openisr.tar.gz && chmod 0755 openisr-%{version}

%clean
rm -rf %{buildroot}

%pre

%post
/sbin/ldconfig
dkms add -m openisr -v %{version}
echo To complete installation, run openisr-config as root.

%preun
/etc/init.d/openisr stop
dkms remove -m openisr -v %{version} --all

%postun
/sbin/ldconfig

%files
%dir /etc/openisr
%dir /usr/share/openisr
%dir /usr/lib/openisr
/usr/src/openisr-%{version}
/usr/bin/isr
/usr/sbin/openisr-config
/usr/lib/openisr/vulpes
/usr/lib/openisr/readstats
/usr/lib/openisr/nexus_debug
/usr/share/man/man1/isr.1.gz
/usr/share/man/man8/openisr-config.8.gz
/usr/share/openisr/openisr.tar.gz
/usr/share/openisr/config
/usr/share/openisr/HTTPSSH.pm
/usr/share/openisr/IsrConfigTie.pm
/usr/share/openisr/Isr.pm
/usr/share/openisr/IsrRevision.pm
/usr/lib/libvdisk.so.0
%config /etc/udev/openisr.rules
%config /etc/udev/rules.d/openisr.rules
%doc README CHANGES LICENSE.*
%defattr(4644,root,root)
/usr/lib/libvdisk.so.0.0.0
%defattr(0755,root,root)
/etc/init.d/openisr


%changelog
* Tue Nov 13 2007 Matt Toups <mtoups@cs.cmu.edu> 0.8.4-0pre0
- soon to be new upstream release
- DKMS support (thanks to Adam Goode)

* Wed Jul 11 2007 Matt Toups <mtoups@cs.cmu.edu> 0.8.3-1
- New upstream release

* Mon Apr 16 2007 Benjamin Gilbert <bgilbert@cs.cmu.edu> 0.8.2-1
- New upstream release

* Tue Apr 10 2007 Matt Toups <mtoups@cs.cmu.edu> 0.8.1-2
- fix spec file bugs
- fix permissions on doc files

* Wed Mar 28 2007 Matt Toups <mtoups@cs.cmu.edu> 0.8.1-1
- improve spec file
- patch to work around broken system headers on fc5/fc6
- only build client stuff for this package

* Fri Feb 23 2007 Matt Toups <mtoups@cs.cmu.edu> 0.8-1
- starting to RPM-ify

### eof
