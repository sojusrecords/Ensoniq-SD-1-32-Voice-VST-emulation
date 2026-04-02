// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert
/*********************************************************************

    formats/esq16_dsk.cpp

    Formats for 16-bit Ensoniq synthesizers and samplers

    Disk is PC MFM, 80 tracks, double-sided, with 10 sectors per track
    
    [PATCHED] Fixed sector indexing for 0-9 based sectors to resolve 
    corrupted disk images on save operations. 

*********************************************************************/

#include "esq16_dsk.h"

#include "ioprocs.h"
#include <cstring>
#include <vector>


const floppy_image_format_t::desc_e esqimg_format::esq_10_desc[] = {
	{ MFM, 0x4e, 80 },
	{ MFM, 0x00, 12 },
	{ RAW, 0x5224, 3 },
	{ MFM, 0xfc, 1 },
	{ MFM, 0x4e, 50 },
	{ MFM, 0x00, 12 },
	{ SECTOR_LOOP_START, 0, 9 },
	{   CRC_CCITT_START, 1 },
	{     RAW, 0x4489, 3 },
	{     MFM, 0xfe, 1 },
	{     TRACK_ID },
	{     HEAD_ID },
	{     SECTOR_ID },
	{     SIZE_ID },
	{   CRC_END, 1 },
	{   CRC, 1 },
	{   MFM, 0x4e, 22 },
	{   MFM, 0x00, 12 },
	{   CRC_CCITT_START, 2 },
	{     RAW, 0x4489, 3 },
	{     MFM, 0xfb, 1 },
	{     SECTOR_DATA, -1 },
	{   CRC_END, 2 },
	{   CRC, 2 },
	{   MFM, 0x4e, 84 },
	{   MFM, 0x00, 12 },
	{ SECTOR_LOOP_END },
	{ MFM, 0x4e, 170 },
	{ END }
};

esqimg_format::esqimg_format()
{
}

const char *esqimg_format::name() const noexcept
{
	return "esq16";
}

const char *esqimg_format::description() const noexcept
{
	return "Ensoniq VFX-SD/SD-1/EPS-16 floppy disk image";
}

const char *esqimg_format::extensions() const noexcept
{
	return "img";
}

bool esqimg_format::supports_save() const noexcept
{
	return true;
}

void esqimg_format::find_size(util::random_read &io, uint8_t &track_count, uint8_t &head_count, uint8_t &sector_count)
{
	uint64_t size;
	if(!io.length(size)) {
		track_count = 80;
		head_count = 2;
		sector_count = 10;

		uint32_t expected_size = 512 * track_count*head_count*sector_count;
		if(size == expected_size) {
			return;
		}
	}
	track_count = head_count = sector_count = 0;
}

int esqimg_format::identify(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants) const
{
	uint8_t track_count, head_count, sector_count;
	find_size(io, track_count, head_count, sector_count);

	if(track_count)
		return FIFID_SIZE;
	return 0;
}

bool esqimg_format::load(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants, floppy_image &image) const
{
	uint8_t track_count, head_count, sector_count;
	find_size(io, track_count, head_count, sector_count);

	uint8_t sectdata[10*512];
	desc_s sectors[10];
	for(int i=0; i<sector_count; i++) {
		sectors[i].data = sectdata + 512*i;
		sectors[i].size = 512;
		sectors[i].sector_id = i;
	}

	int track_size = sector_count*512;
	for(int track=0; track < track_count; track++) {
		for(int head=0; head < head_count; head++) {
			/*auto const [err, actual] =*/ read_at(io, (track*head_count + head)*track_size, sectdata, track_size); // FIXME: check for errors and premature EOF
			generate_track(esq_10_desc, track, head, sectors, sector_count, 110528, image);
		}
	}

	image.set_variant(floppy_image::DSDD);

	return true;
}

bool esqimg_format::save(util::random_read_write &io, const std::vector<uint32_t> &variants, const floppy_image &image) const
{
	int track_count, head_count, sector_count;
	get_geometry_mfm_pc(image, 2000, track_count, head_count, sector_count);

	if(track_count != 80)
		track_count = 80;

	// Happens for a fully unformatted floppy
	if(!head_count)
		head_count = 2;

	if(sector_count != 10)
		sector_count = 10;

	uint8_t sectdata[10*512]; 
	int track_size = sector_count*512;

	for(int track=0; track < track_count; track++) {
		for(int head=0; head < head_count; head++) {
			
			// 1. Generate the MFM bitstream for the channel using the new vector API
			std::vector<bool> bitstream = generate_bitstream_from_track(track, head, 2000, image);
			
			// 2. Retrieve all valid sectors
            // Return value: std::vector<std::vector<uint8_t>>
            // Where the index of the outer vector = the sector's logical ID!
			std::vector<std::vector<uint8_t>> extracted_sectors = extract_sectors_from_bitstream_mfm_pc(bitstream);
			
			// Resetting the buffer to its default state to avoid memory leaks
			std::memset(sectdata, 0, sizeof(sectdata));

			// 3. Mapping sectors based on their explicit IDs (0–9)
			for(size_t id = 0; id < extracted_sectors.size(); id++) {
				if(id < (size_t)sector_count && extracted_sectors[id].size() >= 512) {
					// Csak az érvényes, meglévő szektorokat másoljuk be a megfelelő offsetre
					std::memcpy(sectdata + (id * 512), extracted_sectors[id].data(), 512);
				}
			}

			/*auto const [err, actual] =*/ write_at(io, (track*head_count + head)*track_size, sectdata, track_size); // FIXME: check for errors
		}
	}

	return true;
}

const esqimg_format FLOPPY_ESQIMG_FORMAT;