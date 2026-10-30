# The mode of a run: whether the fact tables are sift_tables (--table), set once by main.
# TABLE_SKIP lists the families a sift_table does not run, with the reason.


TABLE = False  # --table: the fact tables are sift_tables
# families not run on a sift_table, with the reason
TABLE_SKIP = {
    'txnmix': 'the rows a transaction buffers before they reach the write buffer of a sift_col index',
    'point': 'the heap-address directory (point_tid_columns) is an option of the sift_col index',
    'pointagg': 'the heap-address directory (point_tid_columns) is an option of the sift_col index',
    'keycut': 'key_cut is an option of the sift_col index',
    'window': 'the sorted runs (order_columns) are an option of the sift_col index',
    'trail': 'trf is read through a sift_col index of its own, which the mode drops',
    'gorder': 'the global order (order_array_columns) is an option of the sift_col index',
    'zonekey': 'zkf is read through its sift_col index only',
    'keytable': 'ktf is read through its sift_col index only',
    'dictcount': 'dcf is read through its sift_col index only',
    'flatkeys': 'the key tables are read through a sift_col index',
    'keylist': 'the facts are read through a sift_col index',
    'ctecopies': 'the documents are read through a sift_col index',
    'arrlen': 'the lengths of an array are two lanes of the sift_col index (array_lengths); a sift_table keeps the image alone',
}
