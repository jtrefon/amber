Name:           amber-agent
Version:        0.8.0
Release:        1%{?dist}
Summary:        AI agent harness with a terminal interface and a headless CLI

License:        Apache-2.0
URL:            https://github.com/jtrefon/amber
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  libcurl-devel
BuildRequires:  ncurses-devel
BuildRequires:  pkgconfig

# The pre-repository packages were built with fpm and called `amber`. The
# version floor keeps a distro's own unrelated `amber` (if one ever appears)
# out of the way: only the fpm builds are below it, and this package's own
# version is not, so it never obsoletes itself.
Obsoletes:      amber < 0.8.0
Conflicts:      amber < 0.8.0

%description
amber runs an agent loop against any OpenAI-compatible endpoint: the model
reads and writes files, searches the project and runs approval-gated commands
while the session, context and tool output are rendered in place.

This package carries the runtime: the full-screen client (amber), the headless
client (amber-cli), the bundled plugins and the data tree they read at startup.

%package devel
Summary:        Development files for the amber agent harness
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
Headers, static libraries and the pkg-config file needed to build a tool, a
plugin or an embedding application against the amber core.

Build with: pkg-config --cflags --libs amber-agent

%package bench
Summary:        Benchmark and KPI harness for the amber agent
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description bench
amber-bench runs scenario suites against the harness and scores the result.
Hermetic runs use a deterministic fake model, so they are safe in CI; live runs
target any OpenAI-compatible endpoint.

The package ships the scenario corpus and the model profiles, which the harness
resolves from the install prefix rather than the working directory.

# The trust anchor is architecture-independent, so it lives in its own spec
# (amber-agent-release.spec) with BuildArch: noarch rather than inheriting this
# one's architecture for a 1 KB payload.
%prep
%autosetup -n %{name}-%{version}

%build
%configure \
    --libdir=%{_libdir} \
    --libexecdir=%{_libexecdir} \
    --includedir=%{_includedir} \
    --datarootdir=%{_datadir} \
    --mandir=%{_mandir}
%make_build

%install
%make_install
# The Makefile installs its own copies of the licence and README into
# share/doc; rpm owns that directory through the licence and documentation
# macros, and two owners for one file is a conflict on upgrade.
rm -rf %{buildroot}%{_datadir}/doc/%{name}
# find-debuginfo does not look inside a static archive, so a -g build leaves
# the debug sections in the library: 444 MB of libagent_core.a against 24 MB
# once they are gone. dh_strip does this for the Debian package, and both
# packages have to carry the same thing.
%{__strip} --strip-debug %{buildroot}%{_libdir}/libagent_core.a \
                          %{buildroot}%{_libdir}/libagent_tools.a

%files
%license LICENSE NOTICE THIRD_PARTY_LICENSES
%doc README.md
%{_bindir}/amber
%{_bindir}/amber-cli
%{_libexecdir}/amber/plugins
%{_datadir}/amber
%{_mandir}/man1/amber.1*
%{_mandir}/man1/amber-cli.1*

%files devel
%doc README.md
%{_includedir}/amber-agent
%{_libdir}/libagent_core.a
%{_libdir}/libagent_tools.a
%{_libdir}/pkgconfig/amber-agent.pc

%files bench
%{_bindir}/amber-bench
%{_datadir}/amber-bench
%{_mandir}/man1/amber-bench.1*

%changelog
* Fri Oct 30 2026 Jacek Trefon <hello@trefon.com> - 0.8.0-1
- First release published from the signed apt and rpm repositories.
