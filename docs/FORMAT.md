# BM25 segment format 6: the length of each field

A BM25 index built with `fields` (`sift.sections(...)`) scores a leaf of a field (`title:word`, `field_query`) with the length of that field in the document and the average of that length (BM25F), not with the length of the whole text. Format 6 (`SIFT_SEGFMT_FIELDDL`) is what keeps those lengths. Formats 1 to 5 are unchanged and read and merge as before; an index without `fields` still writes the format it wrote.

## The length of a field

`field_dl[f]` of a document is the sum of the stored tf (saturated at 255, as any tf) of the words of field `f` that are terms of their own (`SIFT_FIELD_BYTE(f)` followed by the word; the prefix terms of `prefix_index` are not counted), saturated at 65,535. It is the number of tokens of the section, but for a word that occurs more than 255 times in it, or one too long to be a term. `sift_field_dl()` computes it from the bag of terms of the document, so the build, the write buffer, the promotion and the merge of format 1 to 5 segments (from the tf of the field words of the postings) give the same number.

## Where it is stored

`SiftSegHeaderData` and the sections are unchanged. The lengths are in `DOCMAP`, after the INCLUDE records, and their directory ends the section, before the footer of format 5 (`u64 recs_end, u32 ncols, u32 nsb`, where `ncols` may be 0):

```
SiftDocSBRef[nsb]  SiftAttrRef[nsb][ncols]  SiftFieldDlRef[nsb][nfields]  SiftFieldDlTrailer  footer
```

```c
typedef struct SiftFieldDlRef       /* one per SB and field; 16 bytes (StaticAssertDecl) */
{
    uint64  off;        /* stream offset of the record */
    uint32  len;        /* its bytes, 0: no doc of the SB has a word in the field */
    uint8   dlcode;     /* width of the rows, SIFT_DL_CODE_BITS: 0 = 16, 1 = 8, 2 = 4 bits */
    uint8   pad[3];
} SiftFieldDlRef;

typedef struct SiftFieldDlTrailer   /* 272 bytes (StaticAssertDecl) */
{
    uint32  magic;                          /* "FDL2", SIFT_FIELDDL_MAGIC */
    uint32  nfields;                        /* 1..SIFT_MAX_FIELDS */
    uint64  fdir_off;                       /* stream offset of SiftFieldDlRef[nsb][nfields] */
    uint64  sum_dl[SIFT_MAX_BM25_FIELDS];   /* the length of each field over all the docs, vacuumed ones included */
    uint64  ndocs_nz[SIFT_MAX_BM25_FIELDS]; /* the docs that have a word in the field */
} SiftFieldDlTrailer;
```

A record is the `dl` rows of the SB's present pages one after the other, in the layout and width codes of the docmap's own `dl` rows (the row of a page has as many slots as its highest present offset), taken from the longest length of the field in the SB. The records follow the INCLUDE records, SB by SB and field by field. `sift.verify` checks every length against the tf of the field words of the postings and the trailer against the rows.

## Write buffer

`SIFT_WBUF_FIELDDL` (`0x0004`) in `SiftWbufDocData.flags`: `uint16 field_dl[nfields]`, `nfields` in `SiftWbufDocData.reserved`, padded to 4 bytes, follows the 16-byte header and precedes `hash[]`. A document written without it (an earlier build) is scored with the length of the whole text.

## Scores and bounds

- The average of a field is the sum of its lengths over the live documents of the segments of format 6 and of the write buffers, divided by their number. A vacuumed document leaves both sums.
- A segment of format 5 or older does not keep the lengths: its documents score the length of the whole text with the average of the whole text, as before. A merge writes format 6 for an index with fields, taking the lengths of an older segment from the tf of the field words of its postings.
- The stored bound of a posting of a field term is the bound of a document of length 0 (it does not depend on any length or average), so it stays above every score whatever the document, the field or the average: WAND and block-max bounds are unchanged in kind and need no scaling when documents are vacuumed.
