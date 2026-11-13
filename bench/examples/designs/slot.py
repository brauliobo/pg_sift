"""connected:slot: the DDL of an example's own connected.sql (the slot a prototype fills for that example). A file that holds only notes
means the example has no connected variant yet and the engine is the unconnected one."""


def ddl(ex):
    p = ex.file('connected.sql')
    return p if any(l.strip() and not l.lstrip().startswith('--') for l in open(p)) else None


def session_sql():
    return []
