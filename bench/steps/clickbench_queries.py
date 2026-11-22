"""The 43 ClickBench queries, Q0-Q42, in the PostgreSQL dialect of the official `postgresql/queries.sql`, with what an exact comparison needs:
`verify` (the query with a total ORDER BY, so a LIMIT keeps the same rows on every engine), `floats` (the output columns that hold an AVG and are
compared to 9 significant digits) and the rendering for each engine. `queries.sql` of bench/sql/clickbench is generated from here."""
import re

SUM90 = ', '.join('SUM(ResolutionWidth)' if i == 0 else 'SUM(ResolutionWidth + %d)' % i for i in range(90))
G = "URL LIKE '%google%'"
C62 = "CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31'"


class Query:
    def __init__(self, n, sql, order=None, floats=(), ordered=False):
        self.n, self.id, self.sql, self.floats = n, 'Q%d' % n, sql, tuple(floats)
        self.verify_sql = sql if order is None else with_order(sql, order)
        self.ordered = ordered or order is not None

    def render(self, engine, table='hits'):
        return dialect(re.sub(r'\bhits\b', table, self.sql), engine)

    def render_verify(self, engine, table='hits'):
        return dialect(re.sub(r'\bhits\b', table, self.verify_sql), engine)


def with_order(sql, order):
    m = re.match(r'^(.*?)(?: ORDER BY .*?)?( LIMIT \d+(?: OFFSET \d+)?)?$', sql, re.S)
    return '%s ORDER BY %s%s' % (m.group(1), order, m.group(2) or '')


def dialect(sql, engine):
    if engine.startswith('ch'):
        return sql.replace('length(', 'lengthUTF8(')
    return sql


QUERIES = [Query(n, *a, **k) for n, (a, k) in enumerate([
    (("SELECT COUNT(*) FROM hits",), {}),
    (("SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0",), {}),
    (("SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits",), dict(floats=[2])),
    (("SELECT AVG(UserID) FROM hits",), dict(floats=[0])),
    (("SELECT COUNT(DISTINCT UserID) FROM hits",), {}),
    (("SELECT COUNT(DISTINCT SearchPhrase) FROM hits",), {}),
    (("SELECT MIN(EventDate), MAX(EventDate) FROM hits",), {}),
    (("SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",), {}),
    (("SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DESC LIMIT 10", "u DESC, RegionID"), {}),
    (("SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM hits GROUP BY RegionID "
      "ORDER BY c DESC LIMIT 10", "c DESC, RegionID"), dict(floats=[3])),
    (("SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel "
      "ORDER BY u DESC LIMIT 10", "u DESC, MobilePhoneModel"), {}),
    (("SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' "
      "GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10", "u DESC, MobilePhone, MobilePhoneModel"), {}),
    (("SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
      "c DESC, SearchPhrase"), {}),
    (("SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase "
      "ORDER BY u DESC LIMIT 10", "u DESC, SearchPhrase"), {}),
    (("SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase "
      "ORDER BY c DESC LIMIT 10", "c DESC, SearchEngineID, SearchPhrase"), {}),
    (("SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10", "COUNT(*) DESC, UserID"), {}),
    (("SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10",
      "COUNT(*) DESC, UserID, SearchPhrase"), {}),
    (("SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase LIMIT 10", "UserID, SearchPhrase"), {}),
    (("SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, m, SearchPhrase "
      "ORDER BY COUNT(*) DESC LIMIT 10", "COUNT(*) DESC, UserID, m, SearchPhrase"), {}),
    (("SELECT UserID FROM hits WHERE UserID = 435090932899640449",), {}),
    (("SELECT COUNT(*) FROM hits WHERE %s" % G,), {}),
    (("SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM hits WHERE %s AND SearchPhrase <> '' GROUP BY SearchPhrase "
      "ORDER BY c DESC LIMIT 10" % G, "c DESC, SearchPhrase"), {}),
    (("SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM hits WHERE Title LIKE '%Google%' "
      "AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10", "c DESC, SearchPhrase"), {}),
    (("SELECT * FROM hits WHERE %s ORDER BY EventTime LIMIT 10" % G, "EventTime, WatchID, UserID, URL"), {}),
    (("SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10", "EventTime, WatchID, SearchPhrase"), {}),
    (("SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10",), dict(ordered=True)),
    (("SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase LIMIT 10",), dict(ordered=True)),
    (("SELECT CounterID, AVG(length(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 "
      "ORDER BY l DESC LIMIT 25", "l DESC, CounterID"), dict(floats=[1])),
    (("SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\\.)?([^/]+)/.*$', '\\1') AS k, AVG(length(Referer)) AS l, COUNT(*) AS c, "
      "MIN(Referer) FROM hits WHERE Referer <> '' GROUP BY k HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25", "l DESC, k"),
     dict(floats=[1])),
    (("SELECT %s FROM hits" % SUM90,), {}),
    (("SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits WHERE SearchPhrase <> '' "
      "GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10", "c DESC, SearchEngineID, ClientIP"), dict(floats=[4])),
    (("SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits WHERE SearchPhrase <> '' "
      "GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10", "c DESC, WatchID, ClientIP"), dict(floats=[4])),
    (("SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits GROUP BY WatchID, ClientIP "
      "ORDER BY c DESC LIMIT 10", "c DESC, WatchID, ClientIP"), dict(floats=[4])),
    (("SELECT URL, COUNT(*) AS c FROM hits GROUP BY URL ORDER BY c DESC LIMIT 10", "c DESC, URL"), {}),
    (("SELECT 1, URL, COUNT(*) AS c FROM hits GROUP BY 1, URL ORDER BY c DESC LIMIT 10", "c DESC, URL"), {}),
    (("SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM hits GROUP BY ClientIP, ClientIP - 1, "
      "ClientIP - 2, ClientIP - 3 ORDER BY c DESC LIMIT 10", "c DESC, ClientIP"), {}),
    (("SELECT URL, COUNT(*) AS PageViews FROM hits WHERE %s AND DontCountHits = 0 AND IsRefresh = 0 AND URL <> '' GROUP BY URL "
      "ORDER BY PageViews DESC LIMIT 10" % C62, "PageViews DESC, URL"), {}),
    (("SELECT Title, COUNT(*) AS PageViews FROM hits WHERE %s AND DontCountHits = 0 AND IsRefresh = 0 AND Title <> '' GROUP BY Title "
      "ORDER BY PageViews DESC LIMIT 10" % C62, "PageViews DESC, Title"), {}),
    (("SELECT URL, COUNT(*) AS PageViews FROM hits WHERE %s AND IsRefresh = 0 AND IsLink <> 0 AND IsDownload = 0 GROUP BY URL "
      "ORDER BY PageViews DESC LIMIT 10 OFFSET 1000" % C62, "PageViews DESC, URL"), {}),
    (("SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END "
      "AS Src, URL AS Dst, COUNT(*) AS PageViews FROM hits WHERE %s AND IsRefresh = 0 GROUP BY TraficSourceID, SearchEngineID, "
      "AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 10 OFFSET 1000" % C62,
      "PageViews DESC, TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst"), {}),
    (("SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM hits WHERE %s AND IsRefresh = 0 AND TraficSourceID IN (-1, 6) "
      "AND RefererHash = 3594120000172545465 GROUP BY URLHash, EventDate ORDER BY PageViews DESC LIMIT 10 OFFSET 100" % C62,
      "PageViews DESC, URLHash, EventDate"), {}),
    (("SELECT WindowClientWidth, WindowClientHeight, COUNT(*) AS PageViews FROM hits WHERE %s AND IsRefresh = 0 AND DontCountHits = 0 "
      "AND URLHash = 2868770270353813622 GROUP BY WindowClientWidth, WindowClientHeight ORDER BY PageViews DESC LIMIT 10 OFFSET 10000" % C62,
      "PageViews DESC, WindowClientWidth, WindowClientHeight"), {}),
    (("SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013-07-14' "
      "AND EventDate <= '2013-07-15' AND IsRefresh = 0 AND DontCountHits = 0 GROUP BY DATE_TRUNC('minute', EventTime) "
      "ORDER BY DATE_TRUNC('minute', EventTime) LIMIT 10 OFFSET 1000",), dict(ordered=True)),
])]

assert [q.n for q in QUERIES] == list(range(43))


def queries_sql():
    return ''.join(q.sql + ';\n' for q in QUERIES)
