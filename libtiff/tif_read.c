/* $Id: tif_read.c,v 1.18 2007-03-31 01:04:53 joris Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 * Scanline-oriented Read Support
 */
#include "tiffiop.h"
#include <stdio.h>

int TIFFFillStrip(TIFF* tif, uint32 strip);
int TIFFFillTile(TIFF* tif, uint32 tile);
static int TIFFStartStrip(TIFF* tif, uint32 strip);
static int TIFFStartTile(TIFF* tif, uint32 tile);
static int TIFFCheckRead(TIFF*, int);

#define NOSTRIP ((uint32) -1)       /* undefined state */
#define NOTILE ((ttile_t) -1)         /* undefined state */

/*
 * Seek to a random row+sample in a file.
 */
static int
TIFFSeek(TIFF* tif, uint32 row, uint16 sample)
{
	register TIFFDirectory *td = &tif->tif_dir;
	tstrip_t strip;

	if (row >= td->td_imagelength) {	/* out of range */
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name,
			     "%lu: Row out of range, max %lu",
			     (unsigned long) row,
			     (unsigned long) td->td_imagelength);
		return (0);
	}
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
		if (sample >= td->td_samplesperpixel) {
			TIFFErrorExt(tif->tif_clientdata, tif->tif_name,
			    "%lu: Sample out of range, max %lu",
			    (unsigned long) sample, (unsigned long) td->td_samplesperpixel);
			return (0);
		}
		strip = (uint32)sample*td->td_stripsperimage + row/td->td_rowsperstrip;
	} else
		strip = row / td->td_rowsperstrip;
	if (strip != tif->tif_curstrip) {	/* different strip, refill */
		if (!TIFFFillStrip(tif, strip))
			return (0);
	} else if (row < tif->tif_row) {
		/*
		 * Moving backwards within the same strip: backup
		 * to the start and then decode forward (below).
		 *
		 * NB: If you're planning on lots of random access within a
		 * strip, it's better to just read and decode the entire
		 * strip, and then access the decoded data in a random fashion.
		 */
		if (!TIFFStartStrip(tif, strip))
			return (0);
	}
	if (row != tif->tif_row) {
		/*
		 * Seek forward to the desired row.
		 */
		if (!(*tif->tif_seek)(tif, row - tif->tif_row))
			return (0);
		tif->tif_row = row;
	}
	return (1);
}

int
TIFFReadScanline(TIFF* tif, void* buf, uint32 row, uint16 sample)
{
	int e;

	if (!TIFFCheckRead(tif, 0))
		return (-1);
	if( (e = TIFFSeek(tif, row, sample)) != 0) {
		/*
		 * Decompress desired row into user buffer.
		 */
		e = (*tif->tif_decoderow)
		    (tif, (uint8*) buf, tif->tif_scanlinesize, sample);  

		/* we are now poised at the beginning of the next row */
		tif->tif_row = row + 1;

		if (e)
			(*tif->tif_postdecode)(tif, (uint8*) buf,
			    tif->tif_scanlinesize);
	}
	return (e > 0 ? 1 : -1);
}

/*
 * Read a strip of data and decompress the specified
 * amount into the user-supplied buffer.
 */
uint64
TIFFReadEncodedStrip(TIFF* tif, uint32 strip, void* buf, uint64 size)
{
	static const char module[] = "TIFFReadEncodedStrip";
	TIFFDirectory *td = &tif->tif_dir;
	uint32 rowsperstrip;
	uint32 stripsperplane;
	uint32 stripinplane;
	uint16 plane;
	uint32 rows;
	uint64 stripsize;
	if (!TIFFCheckRead(tif,0))
		return((uint64)(-1));
	if (strip>=td->td_nstrips)  
	{
		TIFFErrorExt(tif->tif_clientdata,module,
		    "%uld: Strip out of range, max %uld",(unsigned long)strip,
		    (unsigned long)td->td_nstrips);  
		return((uint64)(-1));
	}
	/*
	 * Calculate the strip size according to the number of
	 * rows in the strip (check for truncated last strip on any
	 * of the separations).
	 */
	rowsperstrip=td->td_rowsperstrip;
	if (rowsperstrip>td->td_imagelength)
		rowsperstrip=td->td_imagelength;
	stripsperplane=((td->td_imagelength+rowsperstrip-1)/rowsperstrip);
	stripinplane=(strip%stripsperplane);
	plane=(strip/stripsperplane);
	rows=td->td_imagelength-stripinplane*rowsperstrip;
	if (rows>rowsperstrip)
		rows=rowsperstrip;
	stripsize=TIFFVStripSize(tif,rows);
	if ((size!=(uint64)(-1))&&(size<stripsize))
		stripsize=size;
	if (!TIFFFillStrip(tif,strip))
		return((uint64)(-1));
	if ((*tif->tif_decodestrip)(tif,buf,size,plane)<=0)
		return((uint64)(-1));
	(*tif->tif_postdecode)(tif,buf,size);
	return(size);
}

static uint64
TIFFReadRawStrip1(TIFF* tif, uint32 strip, void* buf, uint64 size,
    const char* module)
{
	TIFFDirectory *td = &tif->tif_dir;

	assert((tif->tif_flags&TIFF_NOREADRAW)==0);
	if (!isMapped(tif)) {
		uint64 cc;

		if (!SeekOK(tif, td->td_stripoffset[strip])) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Seek error at scanline %lu, strip %lu",
			    (unsigned long) tif->tif_row, (unsigned long) strip);
			return (-1);
		}
		cc = TIFFReadFile(tif, buf, size);
		if (cc != size) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Read error at scanline %lu; got %llu bytes, expected %llu",
			    tif->tif_name,
			    (unsigned long) tif->tif_row,
			    (unsigned long long) cc,
			    (unsigned long long) size);
			return (-1);
		}
	} else {
		if (td->td_stripoffset[strip] + size > tif->tif_size) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Read error at scanline %lu, strip %lu; got %llu bytes, expected %llu",
			    (unsigned long) tif->tif_row,
			    (unsigned long) strip,
			    (unsigned long long) tif->tif_size - td->td_stripoffset[strip],
			    (unsigned long long) size);
			return (-1);
		}
		_TIFFmemcpy(buf, tif->tif_base + td->td_stripoffset[strip],
                            size);
	}
	return (size);
}

/*
 * Read a strip of data from the file.
 */
uint64
TIFFReadRawStrip(TIFF* tif, uint32 strip, void* buf, uint64 size)
{
	static const char module[] = "TIFFReadRawStrip";
	TIFFDirectory *td = &tif->tif_dir;
	uint64 bytecount;

	if (!TIFFCheckRead(tif, 0))
		return ((uint64) -1);
	if (strip >= td->td_nstrips) {
		TIFFErrorExt(tif->tif_clientdata, module,
		     "%lu: Strip out of range, max %lu",
		     (unsigned long) strip,
		     (unsigned long) td->td_nstrips);
		return ((tsize_t) -1);
	}
	if (tif->tif_flags&TIFF_NOREADRAW)
	{
		TIFFErrorExt(tif->tif_clientdata, module,
		    "Compression scheme does not support access to raw uncompressed data");
		return ((tsize_t) -1);
	}
	bytecount = td->td_stripbytecount[strip];
	if (bytecount <= 0) {
		TIFFErrorExt(tif->tif_clientdata, module,
		    "%llu: Invalid strip byte count, strip %lu",
		    (unsigned long long) bytecount, (unsigned long) strip);
		return ((uint64) -1);
	}
	if (size != (uint64)-1 && size < bytecount)
		bytecount = size;
	return (TIFFReadRawStrip1(tif, strip, buf, bytecount, module));
}

/*
 * Read the specified strip and setup for decoding. The data buffer is
 * expanded, as necessary, to hold the strip's data.
 */
int
TIFFFillStrip(TIFF* tif, tstrip_t strip)
{
	static const char module[] = "TIFFFillStrip";
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags&TIFF_NOREADRAW)==0)
	{
		uint64 bytecount = td->td_stripbytecount[strip];  
		if (bytecount <= 0) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Invalid strip byte count %llu, strip %lu",
			    (unsigned long) bytecount,
			    (unsigned long) strip);
			return (0);
		}
		if (isMapped(tif) &&
		    (isFillOrder(tif, td->td_fillorder)
		    || (tif->tif_flags & TIFF_NOBITREV))) {
			/*
			 * The image is mapped into memory and we either don't
			 * need to flip bits or the compression routine is
			 * going to handle this operation itself.  In this
			 * case, avoid copying the raw data and instead just
			 * reference the data from the memory mapped file
			 * image.  This assumes that the decompression
			 * routines do not modify the contents of the raw data
			 * buffer (if they try to, the application will get a
			 * fault since the file is mapped read-only).
			 */
			if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata)
				_TIFFfree(tif->tif_rawdata);
			tif->tif_flags &= ~TIFF_MYBUFFER;
			/*
			 * We must check for overflow, potentially causing
			 * an OOB read. Instead of simple
			 *
			 *  td->td_stripoffset[strip]+bytecount > tif->tif_size
			 *
			 * comparison (which can overflow) we do the following
			 * two comparisons:
			 */
			if (bytecount > tif->tif_size ||
			    td->td_stripoffset[strip] > tif->tif_size - bytecount) {
				/*
				 * This error message might seem strange, but
				 * it's what would happen if a read were done
				 * instead.
				 */
				TIFFErrorExt(tif->tif_clientdata, module,

					"Read error on strip %lu; "
					"got %llu bytes, expected %llu",
					(unsigned long) strip,
					(unsigned long long) tif->tif_size - td->td_stripoffset[strip],
					(unsigned long long) bytecount);
				tif->tif_curstrip = NOSTRIP;
				return (0);
			}
			tif->tif_rawdatasize = bytecount;
			tif->tif_rawdata = tif->tif_base + td->td_stripoffset[strip];
		} else {
			/*
			 * Expand raw data buffer, if needed, to hold data
			 * strip coming from file (perhaps should set upper
			 * bound on the size of a buffer we'll use?).
			 */
			if (bytecount > tif->tif_rawdatasize) {
				tif->tif_curstrip = NOSTRIP;
				if ((tif->tif_flags & TIFF_MYBUFFER) == 0) {
					TIFFErrorExt(tif->tif_clientdata, module,
					    "Data buffer too small to hold strip %lu",
					    (unsigned long) strip);
					return (0);
				}
				if (!TIFFReadBufferSetup(tif, 0,
				    TIFFroundup_64(bytecount, 1024)))
					return (0);
			}
			if (TIFFReadRawStrip1(tif, strip, tif->tif_rawdata,
				bytecount, module) != bytecount)
				return (0);
			if (!isFillOrder(tif, td->td_fillorder) &&
			    (tif->tif_flags & TIFF_NOBITREV) == 0)
				TIFFReverseBits(tif->tif_rawdata, bytecount);
		}
	}
	return (TIFFStartStrip(tif, strip));
}

/*
 * Tile-oriented Read Support
 * Contributed by Nancy Cam (Silicon Graphics).
 */

/*
 * Read and decompress a tile of data.  The
 * tile is selected by the (x,y,z,s) coordinates.
 */
uint64
TIFFReadTile(TIFF* tif, void* buf, uint32 x, uint32 y, uint32 z, uint16 s)
{
	if (!TIFFCheckRead(tif, 1) || !TIFFCheckTile(tif, x, y, z, s))
		return (-1);
	return (TIFFReadEncodedTile(tif,
	    TIFFComputeTile(tif, x, y, z, s), buf, (uint64) -1));
}

/*
 * Read a tile of data and decompress the specified
 * amount into the user-supplied buffer.
 */
uint64
TIFFReadEncodedTile(TIFF* tif, uint32 tile, void* buf, uint64 size)
{
	static const char module[] = "TIFFReadEncodedTile";
	TIFFDirectory *td = &tif->tif_dir;
	uint64 tilesize = tif->tif_tilesize;

	if (!TIFFCheckRead(tif, 1))
		return (-1);
	if (tile >= td->td_nstrips) {
		TIFFErrorExt(tif->tif_clientdata, module,
		    "%lud: Tile out of range, max %lud",
		    (unsigned long) tile, (unsigned long) td->td_nstrips);
		return (-1);
	}
	if (size == (uint64) -1)
		size = tilesize;
	else if (size > tilesize)
		size = tilesize;
	if (TIFFFillTile(tif, tile) && (*tif->tif_decodetile)(tif,
	    (uint8*) buf, size, (uint16)(tile/td->td_stripsperimage))) {
		(*tif->tif_postdecode)(tif, (uint8*) buf, size);
		return (size);
	} else
		return (-1);
}

static uint64
TIFFReadRawTile1(TIFF* tif, uint32 tile, void* buf, uint64 size, const char* module)
{
	TIFFDirectory *td = &tif->tif_dir;

	assert((tif->tif_flags&TIFF_NOREADRAW)==0);
	if (!isMapped(tif)) {
		uint64 cc;

		if (!SeekOK(tif, td->td_stripoffset[tile])) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Seek error at row %lud, col %lud, tile %lud",
			    (unsigned long) tif->tif_row,
			    (unsigned long) tif->tif_col,
			    (unsigned long) tile);
			return ((uint64) -1);
		}
		cc = TIFFReadFile(tif, buf, size);
		if (cc != size) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Read error at row %lud, col %lud; got %llu bytes, expected %llu",
			    (unsigned long) tif->tif_row,
			    (unsigned long) tif->tif_col,
			    (unsigned long long) cc,
			    (unsigned long long) size);
			return ((uint64) -1);
		}
	} else {
		if (td->td_stripoffset[tile] + size > tif->tif_size) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "Read error at row %lud, col %lud, tile %lud; got %llu bytes, expected %llu",
			    (unsigned long) tif->tif_row,
			    (unsigned long) tif->tif_col,
			    (unsigned long) tile,
			    (unsigned long long) tif->tif_size - td->td_stripoffset[tile],
			    (unsigned long long) size);
			return ((uint64) -1);
		}
		_TIFFmemcpy(buf, tif->tif_base + td->td_stripoffset[tile], size);  
	}
	return (size);
}

/*
 * Read a tile of data from the file.
 */
uint64
TIFFReadRawTile(TIFF* tif, uint32 tile, void* buf, uint64 size)
{
	static const char module[] = "TIFFReadRawTile";
	TIFFDirectory *td = &tif->tif_dir;
	uint64 bytecount;

	if (!TIFFCheckRead(tif, 1))
		return ((uint64) -1);
	if (tile >= td->td_nstrips) {
		TIFFErrorExt(tif->tif_clientdata, module,
		    "%lu: Tile out of range, max %lu",
		    (unsigned long) tile, (unsigned long) td->td_nstrips);
		return ((uint64) -1);
	}
	if (tif->tif_flags&TIFF_NOREADRAW)
	{
		TIFFErrorExt(tif->tif_clientdata, module,
		"Compression scheme does not support access to raw uncompressed data");
		return ((uint64) -1);
	}
	bytecount = td->td_stripbytecount[tile];
	if (size != (uint64) -1 && size < bytecount)
		bytecount = size;
	return (TIFFReadRawTile1(tif, tile, buf, bytecount, module));
}

/*
 * Read the specified tile and setup for decoding. The data buffer is
 * expanded, as necessary, to hold the tile's data.
 */
int
TIFFFillTile(TIFF* tif, uint32 tile)
{
	static const char module[] = "TIFFFillTile";
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags&TIFF_NOREADRAW)==0)
	{
		uint64 bytecount = td->td_stripbytecount[tile];  
		if (bytecount <= 0) {
			TIFFErrorExt(tif->tif_clientdata, module,
			    "%llu: Invalid tile byte count, tile %lu",
			    (unsigned long long) bytecount, (unsigned long) tile);
			return (0);
		}
		if (isMapped(tif) &&
		    (isFillOrder(tif, td->td_fillorder)
		     || (tif->tif_flags & TIFF_NOBITREV))) {
			/*
			 * The image is mapped into memory and we either don't
			 * need to flip bits or the compression routine is
			 * going to handle this operation itself.  In this
			 * case, avoid copying the raw data and instead just
			 * reference the data from the memory mapped file
			 * image.  This assumes that the decompression
			 * routines do not modify the contents of the raw data
			 * buffer (if they try to, the application will get a
			 * fault since the file is mapped read-only).
			 */
			if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata)
				_TIFFfree(tif->tif_rawdata);
			tif->tif_flags &= ~TIFF_MYBUFFER;
			/*
			 * We must check for overflow, potentially causing
			 * an OOB read. Instead of simple
			 *
			 *  td->td_stripoffset[tile]+bytecount > tif->tif_size
			 *
			 * comparison (which can overflow) we do the following
			 * two comparisons:
			 */
			if (bytecount > tif->tif_size ||
			    td->td_stripoffset[tile] > tif->tif_size - bytecount) {  
				tif->tif_curtile = NOTILE;
				return (0);
			}
			tif->tif_rawdatasize = bytecount;
			tif->tif_rawdata =
				tif->tif_base + td->td_stripoffset[tile];
		} else {
			/*
			 * Expand raw data buffer, if needed, to hold data
			 * tile coming from file (perhaps should set upper
			 * bound on the size of a buffer we'll use?).
			 */
			if (bytecount > tif->tif_rawdatasize) {  
				tif->tif_curtile = NOTILE;
				if ((tif->tif_flags & TIFF_MYBUFFER) == 0) {
					TIFFErrorExt(tif->tif_clientdata,
					    module,
					    "Data buffer too small to hold tile %lud",
					    (unsigned long) tile);
					return (0);
				}
				if (!TIFFReadBufferSetup(tif, 0,
				    TIFFroundup_64(bytecount, 1024)))  
					return (0);
			}
			if (TIFFReadRawTile1(tif, tile,
			    tif->tif_rawdata, bytecount, module) != bytecount)
				return (0);
			if (!isFillOrder(tif, td->td_fillorder) &&
			    (tif->tif_flags & TIFF_NOBITREV) == 0)
				TIFFReverseBits(tif->tif_rawdata, bytecount);
		}
	}
	return (TIFFStartTile(tif, tile));
}

/*
 * Setup the raw data buffer in preparation for
 * reading a strip of raw data.  If the buffer
 * is specified as zero, then a buffer of appropriate
 * size is allocated by the library.  Otherwise,
 * the client must guarantee that the buffer is
 * large enough to hold any individual strip of
 * raw data.
 */
int
TIFFReadBufferSetup(TIFF* tif, void* bp, uint64 size)
{
	static const char module[] = "TIFFReadBufferSetup";

	assert((tif->tif_flags&TIFF_NOREADRAW)==0);
	if (tif->tif_rawdata) {
		if (tif->tif_flags & TIFF_MYBUFFER)
			_TIFFfree(tif->tif_rawdata);
		tif->tif_rawdata = NULL;
	}
	if (bp) {
		tif->tif_rawdatasize = size;
		tif->tif_rawdata = (uint8*) bp;
		tif->tif_flags &= ~TIFF_MYBUFFER;
	} else {
		tif->tif_rawdatasize = TIFFroundup_64(size, 1024);
		tif->tif_rawdata = (uint8*) _TIFFmalloc(tif->tif_rawdatasize);
		tif->tif_flags |= TIFF_MYBUFFER;
	}
	if (tif->tif_rawdata == NULL) {
		TIFFErrorExt(tif->tif_clientdata, module,
		    "No space for data buffer at scanline %lud",
		    (unsigned long) tif->tif_row);
		tif->tif_rawdatasize = 0;
		return (0);
	}
	return (1);
}

/*
 * Set state to appear as if a
 * strip has just been read in.
 */
static int
TIFFStartStrip(TIFF* tif, uint32 strip)
{
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupdecode)(tif))
			return (0);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
	tif->tif_curstrip = strip;
	tif->tif_row = (strip % td->td_stripsperimage) * td->td_rowsperstrip;
	if (tif->tif_flags&TIFF_NOREADRAW)
	{
		tif->tif_rawcp = NULL;
		tif->tif_rawcc = 0;  
	}
	else
	{
		tif->tif_rawcp = tif->tif_rawdata;
		tif->tif_rawcc = td->td_stripbytecount[strip];
	}
	return ((*tif->tif_predecode)(tif,
			(uint16)(strip / td->td_stripsperimage)));
}

/*
 * Set state to appear as if a
 * tile has just been read in.
 */
static int
TIFFStartTile(TIFF* tif, uint32 tile)
{
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupdecode)(tif))
			return (0);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
	tif->tif_curtile = tile;
	tif->tif_row =
	    (tile % TIFFhowmany_32(td->td_imagewidth, td->td_tilewidth)) *
		td->td_tilelength;
	tif->tif_col =
	    (tile % TIFFhowmany_32(td->td_imagelength, td->td_tilelength)) * 
		td->td_tilewidth;
	if (tif->tif_flags&TIFF_NOREADRAW)
	{
		tif->tif_rawcp = NULL;
		tif->tif_rawcc = 0;
	}
	else
	{
		tif->tif_rawcp = tif->tif_rawdata;
		tif->tif_rawcc = td->td_stripbytecount[tile];  
	}
	return ((*tif->tif_predecode)(tif,
			(uint16)(tile/td->td_stripsperimage)));
}

static int
TIFFCheckRead(TIFF* tif, int tiles)
{
	if (tif->tif_mode == O_WRONLY) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name, "File not open for reading");
		return (0);
	}
	if (tiles ^ isTiled(tif)) {
		TIFFErrorExt(tif->tif_clientdata, tif->tif_name, tiles ?
		    "Can not read tiles from a stripped image" :
		    "Can not read scanlines from a tiled image");
		return (0);
	}
	return (1);
}

void
_TIFFNoPostDecode(TIFF* tif, uint8* buf, uint64 cc)
{
    (void) tif; (void) buf; (void) cc;
}

void
_TIFFSwab16BitData(TIFF* tif, uint8* buf, uint64 cc)
{
    (void) tif;
    assert((cc & 1) == 0);
    TIFFSwabArrayOfShort((uint16*) buf, cc/2);
}

void
_TIFFSwab24BitData(TIFF* tif, uint8* buf, uint64 cc)
{
    (void) tif;
    assert((cc % 3) == 0);
    TIFFSwabArrayOfTriples((uint8*) buf, cc/3);    
}

void
_TIFFSwab32BitData(TIFF* tif, uint8* buf, uint64 cc)
{
    (void) tif;
    assert((cc & 3) == 0);
    TIFFSwabArrayOfLong((uint32*) buf, cc/4);          
}

void
_TIFFSwab64BitData(TIFF* tif, uint8* buf, uint64 cc)
{
    (void) tif;
    assert((cc & 7) == 0);
    TIFFSwabArrayOfDouble((double*) buf, cc/8);  
}

/* vim: set ts=8 sts=8 sw=8 noet: */
