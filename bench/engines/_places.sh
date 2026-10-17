# Not an engine: the SQL fragments the places statements of the adapters share (sourced by load_engine before every adapter,
# see sql/places_schema.sql for the functions). The typed query reaches a statement as psql variables (steps/places.sh):
#   :q the typed words ("grand ho"), :sq the same words as sift prefixes ("grand* ho*"), :sqb the boosted sift query,
#   :tq the prefix tsquery of the PG-parser engines ('grand':* & 'ho':*), :ng the distinct edge n-grams of the typed words
PLACES_TEXT="place_search_text(name, alternate_names, admin1, country, country_code, feature)"   # every field, one text
PLACES_NAME="place_search_name(name, alternate_names)"                                          # the name field
PLACES_SECT="place_search_sections(name, alternate_names, admin1, country, country_code, feature)"   # name section, then the rest
