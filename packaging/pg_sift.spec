# PGDG layout (RHEL, Rocky, Alma). Build: rpmbuild -bb --define "pgmajor N" --define "pkg_version X.Y.Z" --define "_sourcedir DIR" (scripts/package.sh).
%if 0%{?amzn}
# Amazon Linux: the distribution's own postgresqlN packages (/usr/bin, /usr/lib64/pgsql), one major installed at a time
%global pgbin   %{_bindir}
%global pglib   %{_libdir}/pgsql
%global pgshare %{_datadir}/pgsql
%global pgdevel postgresql%{pgmajor}-server-devel
%else
%global pgbin   /usr/pgsql-%{pgmajor}/bin
%global pglib   /usr/pgsql-%{pgmajor}/lib
%global pgshare /usr/pgsql-%{pgmajor}/share
%global pgdevel postgresql%{pgmajor}-devel
%endif
%global debug_package %{nil}
%global _build_id_links none

Name:           pg_sift_%{pgmajor}
Version:        %{pkg_version}
Release:        1%{?dist}
Summary:        Fast analytics for PostgreSQL: just add an index, plus BM25 full-text search
License:        PostgreSQL
URL:            https://github.com/brauliobo/pg_sift
Source0:        pg_sift-%{version}.tar.gz
ExclusiveArch:  x86_64 aarch64
BuildRequires:  gcc, make, redhat-rpm-config, %{pgdevel}, lz4-devel, libzstd-devel, libicu-devel
Requires:       postgresql%{pgmajor}-server, lz4-libs, libzstd

%description
pg_sift answers counts, sums, distinct counts, group-bys and top-N from compressed columns, with the queries you already have: a
sift_col index on a table, or the sift_table table access method, is picked by the planner through the SiftAgg aggregate scan, and a
query the planner cannot prove equal keeps PostgreSQL's own plan.

It also adds BM25 full-text search: a sift index answers boolean, phrase, prefix, wildcard, regex and fuzzy queries with exact
ranking, through its own query syntax.

One CREATE EXTENSION, no shared_preload_libraries. For PostgreSQL 17, 18 and 19.

%prep
%autosetup -n pg_sift-%{version}

%build
make USE_PGXS=1 SIFT_LTO=1 PG_CONFIG=%{pgbin}/pg_config %{?_smp_mflags}

%install
make USE_PGXS=1 SIFT_LTO=1 PG_CONFIG=%{pgbin}/pg_config DESTDIR=%{buildroot} install

%files
%license LICENSE NOTICE
%doc README.md docs/*.md
%{pglib}/pg_sift.so
%{pgshare}/extension/pg_sift*

%changelog
