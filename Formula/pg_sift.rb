class PgSift < Formula
  desc "Full-text BM25 search and column analytics for PostgreSQL"
  homepage "https://github.com/brauliobo/pg_sift"
  url "https://github.com/brauliobo/pg_sift/archive/refs/tags/v3.6.0.tar.gz"
  sha256 "c5440207262ce064fc97cdfbdbc62e6425f5f49f04be72c51ae477a6f83c060c"
  license "PostgreSQL"
  head "https://github.com/brauliobo/pg_sift.git", branch: "main"

  depends_on "postgresql@17" => [:build, :test]
  depends_on "postgresql@18" => [:build, :test]
  # utils/pg_locale.h, which src/core/compat.h includes, includes unicode/ucol.h; icu4c is keg-only, and Homebrew puts
  # the include directory of a declared dependency on the compiler's path
  depends_on "icu4c"
  depends_on "lz4"
  depends_on "zstd"

  def postgresqls
    deps.map(&:to_formula).select { |formula| formula.name.start_with?("postgresql@") }.sort_by(&:version)
  end

  def install
    postgresqls.each do |postgresql|
      args = ["USE_PGXS=1", "PG_CONFIG=#{postgresql.opt_bin}/pg_config"]
      system "make", *args
      system "make", "install", *args, "pkglibdir=#{lib/postgresql.name}",
                                      "datadir=#{share/postgresql.name}",
                                      "pkgincludedir=#{include/postgresql.name}"
      system "make", "clean", *args
    end
    pkgshare.install "packaging/smoke.sql"
  end

  def caveats
    <<~EOS
      In each PostgreSQL database, enable pg_sift with:
        CREATE EXTENSION pg_sift;
      The stable formula builds the release tag; `brew install --HEAD` builds the main branch.
    EOS
  end

  test do
    ENV["LC_ALL"] = "C"
    postgresqls.each do |postgresql|
      pg_ctl = postgresql.opt_bin/"pg_ctl"
      psql = postgresql.opt_bin/"psql"
      port = free_port
      datadir = testpath/postgresql.name
      system pg_ctl, "initdb", "-D", datadir
      (datadir/"postgresql.conf").write <<~EOS, mode: "a+"
        port = #{port}
        listen_addresses = ''
        unix_socket_directories = '#{testpath}'
      EOS
      system pg_ctl, "start", "-D", datadir, "-l", testpath/"log-#{postgresql.name}"
      begin
        args = ["-X", "-v", "ON_ERROR_STOP=1", "-h", testpath, "-p", port.to_s, "-d", "postgres"]
        control = (share/postgresql.name/"extension/pg_sift.control").read
        extension_version = control.match(/default_version = '([^']+)'/)[1]
        system psql, *args, "-v", "from=3.0", "-v", "to=#{extension_version}", "-f", pkgshare/"smoke.sql"
        column_sql = testpath/"column-#{postgresql.name}.sql"
        column_sql.write <<~SQL
          LOAD 'pg_sift';
          CREATE FUNCTION pg_temp.explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
          DECLARE l text;
          BEGIN FOR l IN EXECUTE 'EXPLAIN ' || q LOOP RETURN NEXT l; END LOOP; END $$;
          CREATE TABLE metrics (category int, amount int);
          INSERT INTO metrics SELECT i % 3, i FROM generate_series(1, 1000) i;
          CREATE INDEX metrics_col ON metrics USING sift_col (category, amount);
          VACUUM (ANALYZE) metrics;
          SET sift.enable_agg = on;
          SET enable_seqscan = off;
          EXPLAIN (COSTS OFF) SELECT sum(amount) FROM metrics WHERE category = 1;
          SELECT sum(amount) FROM metrics WHERE category = 1;
          DO $$
          DECLARE plan text;
          BEGIN
            SELECT string_agg(l, E'\\n') INTO plan FROM
              pg_temp.explain('SELECT sum(amount) FROM metrics WHERE category = 1') l;
            ASSERT plan LIKE '%SiftAgg%', plan;
            ASSERT (SELECT sum(amount) FROM metrics WHERE category = 1) = 167167;
          END $$;
        SQL
        system psql, *args, "-f", column_sql
      ensure
        system pg_ctl, "stop", "-D", datadir
      end
    end
  end
end
