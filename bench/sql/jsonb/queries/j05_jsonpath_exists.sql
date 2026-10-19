SELECT count(*) FROM docs d WHERE jsonb_path_exists(d.items, '$[*] ? (@.kind == "c3" && @.flag == "fa")')
