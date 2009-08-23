#!/bin/sh
. ${srcdir:-.}/common.sh
infile="$srcdir/images/palette-1c-4b.tiff"
outfile="o-tiffcrop-extractz14-palette-1c-4b.tiff"
f_test_convert "$TIFFCROP -E left -Z1:4,2:4" $infile $outfile
f_tiffinfo_validate $outfile
