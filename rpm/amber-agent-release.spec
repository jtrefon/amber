Name:           amber-agent-release
Version:        0.8.0
Release:        1%{?dist}
Summary:        Signed package repository configuration for amber-agent
License:        Apache-2.0
URL:            https://github.com/jtrefon/amber
# Architecture-independent by definition: it is two text files and a key. Its
# own spec exists so it can be noarch while the binaries are not.
BuildArch:      noarch

Source0:        amber-agent-archive-keyring.asc
Source1:        amber-agent.repo
Source2:        LICENSE

%description
Installs the archive signing key and the repository definition, with both
package and metadata signature checks enabled.

Installing this package and running a package upgrade is equivalent to what the
published install script does, without fetching a script first.

%prep
%build
%install
install -D -m 644 %{SOURCE0} %{buildroot}%{_sysconfdir}/pki/rpm-gpg/RPM-GPG-KEY-amber-agent
install -D -m 644 %{SOURCE1} %{buildroot}%{_sysconfdir}/yum.repos.d/amber-agent.repo
install -D -m 644 %{SOURCE2} %{buildroot}%{_docdir}/%{name}/LICENSE

%files
%license %{_docdir}/%{name}/LICENSE
%config(noreplace) %{_sysconfdir}/yum.repos.d/amber-agent.repo
%config(noreplace) %{_sysconfdir}/pki/rpm-gpg/RPM-GPG-KEY-amber-agent

%changelog
* Fri Oct 30 2026 Jacek Trefon <hello@trefon.com> - 0.8.0-1
- First release published from the signed apt and rpm repositories.
