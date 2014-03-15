/*
    This file is part of Spike Guard.

    Spike Guard is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Spike Guard is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Spike Guard.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pe.h" // Some functions from the PE class (related to resource parsing) have been
				// implemented in this file. I know this isn't standard practice, but pe.cpp
				// was getting way too big. It made sense (at least semantically) to move
				// them here.

#include "resources.h"

namespace bfs = boost::filesystem;

namespace sg 
{

bool PE::read_image_resource_directory(image_resource_directory& dir, FILE* f, unsigned int offset)
{
	if (offset)
	{
		offset = _rva_to_offset(_ioh.directories[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress) + offset;
		if (!offset || fseek(f, offset, SEEK_SET))
		{
			std::cout << "[!] Error: Could not reach an IMAGE_RESOURCE_DIRECTORY." << std::endl;
			return false;
		}
	}

	unsigned int size = 2*sizeof(boost::uint32_t) + 4*sizeof(boost::uint16_t);
	dir.Entries.clear();
	if (size != fread(&dir, 1, size, f))
	{
		std::cout << "[!] Error: Could not read an IMAGE_RESOURCE_DIRECTORY." << std::endl;
		return false;
	}

	for (int i = 0 ; i < dir.NumberOfIdEntries + dir.NumberOfNamedEntries ; ++i)
	{
		pimage_resource_directory_entry entry = pimage_resource_directory_entry(new image_resource_directory_entry);
		size = 2*sizeof(boost::uint32_t);
		memset(entry.get(), 0, size);
		if (size != fread(entry.get(), 1, size, f))
		{
			std::cout << "[!] Error: Could not read an IMAGE_RESOURCE_DIRECTORY_ENTRY." << std::endl;
			return false;
		}

		// For named entries, NameOrId is a RVA to a string: retrieve it and NameOrId has high bit set to 1.
		if (entry->NameOrId & 0x80000000) 
		{
			// The offset of the string is relative 
			unsigned int offset = _rva_to_offset(_ioh.directories[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress) 
				+ (entry->NameOrId & 0x7FFFFFFF);
			if (!offset || !utils::read_string_at_offset(f, offset, entry->NameStr, true))
			{
				std::cout << "[!] Error: Could not read an IMAGE_RESOURCE_DIRECTORY_ENTRY's name." << std::endl;
				return false;
			}
		}

		dir.Entries.push_back(entry);
	}

	return true;
}

// ----------------------------------------------------------------------------

bool PE::_parse_resources(FILE* f)
{
	if (!_reach_directory(f, IMAGE_DIRECTORY_ENTRY_RESOURCE))	{ // No resources.
		return true;
	}

	image_resource_directory root;
	read_image_resource_directory(root, f);

	// Read Type directories
	for (std::vector<pimage_resource_directory_entry>::iterator it = root.Entries.begin() ; it != root.Entries.end() ; ++it)
	{
		image_resource_directory type;
		read_image_resource_directory(type, f, (*it)->OffsetToData & 0x7FFFFFFF);

		// Read Name directory
		for (std::vector<pimage_resource_directory_entry>::iterator it2 = type.Entries.begin() ; it2 != type.Entries.end() ; ++it2)
		{
			image_resource_directory name;
			read_image_resource_directory(name, f, (*it2)->OffsetToData & 0x7FFFFFFF);

			// Read the IMAGE_RESOURCE_DATA_ENTRY
			for (std::vector<pimage_resource_directory_entry>::iterator it3 = name.Entries.begin() ; it3 != name.Entries.end() ; ++it3)
			{
				image_resource_data_entry entry;
				memset(&entry, 0, sizeof(image_resource_data_entry));

				unsigned int offset = _rva_to_offset(_ioh.directories[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress + ((*it3)->OffsetToData & 0x7FFFFFFF));
				if (!offset || fseek(f, offset, SEEK_SET))
				{
					std::cout << "[!] Error: Could not reach an IMAGE_RESOURCE_DATA_ENTRY." << std::endl;
					return false;
				}

				if (sizeof(image_resource_data_entry) != fread(&entry, 1, sizeof(image_resource_data_entry), f))
				{
					std::cout << "[!] Error: Could not read an IMAGE_RESOURCE_DATA_ENTRY." << std::endl;
					return false;
				}

				// Flatten the resource tree.
				std::string name;
				std::string type;
				std::string language;
				int id = 0;

				// Translate resource type.
				if ((*it)->NameOrId & 0x80000000) {// NameOrId is an offset to a string, we already recovered it
					type = (*it)->NameStr;
				}
				else { // Otherwise, it's a MAKERESOURCEINT constant.
					type = nt::translate_to_flag((*it)->NameOrId, nt::RESOURCE_TYPES);
				}

				// Translate resource name
				if ((*it2)->NameOrId & 0x80000000) {
					name = (*it2)->NameStr;
				}
				else {
					id = (*it2)->NameOrId;
				}

				// Translate the language.
				if ((*it3)->NameOrId & 0x80000000) {
					language = (*it3)->NameStr;
				}
				else {
					language = nt::translate_to_flag((*it3)->NameOrId, nt::LANG_IDS);
				}

				offset = _rva_to_offset(entry.OffsetToData);
				pResource res;
				if (name != "")
				{
					res = pResource(new Resource(type,
												 name,
												 language,
												 entry.Codepage,
												 entry.Size,
												 offset,
												 get_path()));
				}
				else { // No name: call the constructor with the resource ID instead.
					res = pResource(new Resource(type,
												 id,
												 language,
												 entry.Codepage,
												 entry.Size,
												 offset,
												 get_path()));
				}

				_resource_table.push_back(res);
			}
		}
	}

	return true;
}

// ----------------------------------------------------------------------------

std::vector<boost::uint8_t> Resource::get_raw_data()
{
	std::vector<boost::uint8_t> res = std::vector<boost::uint8_t>();
	
	FILE* f = _reach_data();
	if (f == NULL) {
		goto END;
	}
	
	res.resize(_size);
	unsigned int read_bytes = fread(&res[0], 1, _size, f);
	if (read_bytes != _size) { // We got less bytes than expected: reduce the vector's size.
		res.resize(read_bytes);
	}

	END:
	if (f != NULL) {
		fclose(f);
	}
	return res;
}

// ----------------------------------------------------------------------------

template<>
std::string Resource::interpret_as()
{
	if (_type != "RT_MANIFEST") {
		return "Resources of type " + _type + " cannot be interpreted as std::strings.";
	}
	std::vector<boost::uint8_t> manifest_bytes = get_raw_data();
	return std::string(manifest_bytes.begin(), manifest_bytes.end());
}

// ----------------------------------------------------------------------------

template<>
std::vector<std::string> Resource::interpret_as()
{
	std::vector<std::string> res;
	if (_type != "RT_STRING") {
		return res;
	}

	FILE* f = _reach_data();
	if (f == NULL) {
		goto END;
	}

	// RT_STRING resources are made of 16 contiguous "unicode" strings.
	for (int i = 0; i < 16; ++i) {
		res.push_back(utils::read_unicode_string(f));
	}

	END:
	if (f != NULL) {
		fclose(f);
	}
	return res;
}

// ----------------------------------------------------------------------------

template<>
pbitmap Resource::interpret_as()
{
	if (_type != "RT_BITMAP") {
		return pbitmap();
	}

	pbitmap res = pbitmap(new bitmap);
	unsigned int header_size = 14;
	res->Magic[0] = 'B';
	res->Magic[1] = 'M';
	res->Reserved1 = 0;
	res->Reserved2 = 0;
	res->data = get_raw_data();
	res->Size = res->data.size() + header_size;

	// Calculate the offset to the raw data.
	if (res->data.size() < 36) { // Not enough bytes to make a valid BMP
		return pbitmap();
	}
	boost::uint32_t dib_header_size = 0;
	boost::uint32_t colors_used = 0;
	memcpy(&dib_header_size, &(res->data[0]), sizeof(boost::uint32_t)); // DIB header size is located at offset 0.
	memcpy(&colors_used, &(res->data[32]), sizeof(boost::uint32_t)); // DIB header size is located at offset 0.

	res->OffsetToData = header_size + dib_header_size + 4*colors_used;
	return res;
}

// ----------------------------------------------------------------------------

template<>
pgroup_icon_directory Resource::interpret_as()
{
	if (_type != "RT_GROUP_ICON" && _type != "RT_GROUP_CURSOR") {
		return pgroup_icon_directory();
	}
	FILE* f = _reach_data();
	if (f == NULL) {
		return pgroup_icon_directory();
	}

	pgroup_icon_directory res = pgroup_icon_directory(new group_icon_directory);
	unsigned int size = sizeof(boost::uint16_t) * 3;
	if (size != fread(res.get(), 1, size, f)) 
	{
		res.reset();
		goto END;
	}

	for (unsigned int i = 0; i < res->Count; ++i)
	{
		pgroup_icon_directory_entry entry = pgroup_icon_directory_entry(new group_icon_directory_entry);

		memset(entry.get(), 0, sizeof(group_icon_directory_entry));

		if (_type == "RT_GROUP_ICON") 
		{
			// sizeof(group_icon_directory_entry) - 2 to compensate the field that was changed to boost::uint32.
			// See the comment in the structure for more information.
			if (sizeof(group_icon_directory_entry)-2 != fread(entry.get(), 1, sizeof(group_icon_directory_entry) - 2, f))
			{
				res.reset();
				goto END;
			}
		}
		else // Cursors have a different structure. Adapt it to a .ico.
		{
			// I know I am casting bytes to shorts here. I'm not proud of it.
			fread(&(entry->Width), 1, sizeof(boost::uint8_t), f);
			fseek(f, 1, SEEK_CUR);
			fread(&(entry->Height), 1, sizeof(boost::uint8_t), f);
			fseek(f, 1, SEEK_CUR);
			fread(&(entry->Planes), 1, sizeof(boost::uint16_t), f);
			fread(&(entry->BitCount), 1, sizeof(boost::uint16_t), f);
			fread(&(entry->BytesInRes), 1, sizeof(boost::uint32_t), f);
			fread(&(entry->Id), 1, sizeof(boost::uint16_t), f);
			if (ferror(f) || feof(f))
			{
				res.reset();
				goto END;
			}
		}
		
		res->Entries.push_back(entry);
	}
	
	END:
	if (f != NULL) {
		fclose(f);
	}
	return res;
}

// ----------------------------------------------------------------------------

template<>
std::vector<boost::uint8_t> Resource::interpret_as() {
	return get_raw_data();
}

// ----------------------------------------------------------------------------

FILE* Resource::_reach_data()
{
	FILE* f = fopen(_path_to_pe.c_str(), "rb");
	if (f == NULL) { // File has moved, or is already in use.
		return NULL;
	}

	if (!_offset_in_file || fseek(f, _offset_in_file, SEEK_SET)) 
	{
		// Offset is invalid
		fclose(f);
		return NULL;
	}

	return f;
}

// ----------------------------------------------------------------------------

std::vector<boost::uint8_t> reconstruct_icon(pgroup_icon_directory directory, const std::vector<pResource>& resources)
{
	std::vector<boost::uint8_t> res;

	if (directory == NULL) {
		return res;
	}

	unsigned int header_size = 3 * sizeof(boost::uint16_t) + directory->Count * sizeof(group_icon_directory_entry);
	res.resize(header_size);
	memcpy(&res[0], directory.get(), 3 * sizeof(boost::uint16_t));

	for (int i = 0; i < directory->Count; ++i)
	{
		// Locate the RT_ICON with a matching ID.
		pResource icon = pResource();
		for (std::vector<pResource>::const_iterator it = resources.begin(); it != resources.end(); ++it)
		{
			if ((*it)->get_id() == directory->Entries[i]->Id) 
			{
				icon = *it;
				break;
			}
		}
		if (icon == NULL)
		{
			std::cout << "Error: Could not locate RT_ICON with ID " << directory->Entries[i]->Id << "!" << std::endl;
			res.clear();
			return res;
		}

		std::vector<boost::uint8_t> icon_bytes = icon->get_raw_data();
		memcpy(&res[3 * sizeof(boost::uint16_t) + i * sizeof(group_icon_directory_entry)],
			   directory->Entries[i].get(),
			   sizeof(group_icon_directory_entry) - sizeof(boost::uint32_t)); // Don't copy the last field.
		// Fix the icon_directory_entry with the offset in the file instead of a RT_ICON id
		unsigned int size_fix = res.size();
		memcpy(&res[3 * sizeof(boost::uint16_t) + (i+1) * sizeof(group_icon_directory_entry) - sizeof(boost::uint32_t)],
			   &size_fix,
			   sizeof(boost::uint32_t));
		// Append the icon bytes at the end of the data
		if (directory->Type == 1) { // General case for icons
			res.insert(res.end(), icon_bytes.begin(), icon_bytes.end());
		}
		else if (icon_bytes.size() > 2 * sizeof(boost::uint16_t)) { // Cursors have a "hotspot" structure that we have to discard to create a valid ico.
			res.insert(res.end(), icon_bytes.begin() + 2 * sizeof(boost::uint16_t), icon_bytes.end());
		}
		else { // Invalid cursor.
			res.clear();
		}
	}

	return res;
}

// ----------------------------------------------------------------------------

bool PE::extract_resources(const std::string& destination_folder)
{
	if (!bfs::exists(destination_folder) && !bfs::create_directory(destination_folder)) 
	{
		std::cout << "Error: Could not create directory " << destination_folder << "." << std::endl;
		return false;
	}

	std::string base = bfs::basename(get_path());
	FILE* f;
	for (std::vector<pResource>::iterator it = _resource_table.begin() ; it != _resource_table.end() ; ++it)
	{
		bfs::path destination_file;
		std::stringstream ss;
		std::vector<boost::uint8_t> data;
		if ((*it)->get_type() == "RT_GROUP_ICON" || (*it)->get_type() == "RT_GROUP_CURSOR")
		{
			ss << base << "_" << (*it)->get_id() << "_" << (*it)->get_type() << ".ico";
			data = reconstruct_icon((*it)->interpret_as<pgroup_icon_directory>(), _resource_table);
		}
		else if ((*it)->get_type() == "RT_MANIFEST") 
		{
			ss << base << "_" << (*it)->get_id() << "_RT_MANIFEST.xml";
			data = (*it)->get_raw_data();
		}
		else if ((*it)->get_type() == "RT_BITMAP")
		{
			ss << base << "_" << (*it)->get_id() << "_RT_BITMAP.bmp";
			unsigned int header_size = 2 * sizeof(boost::uint8_t) + 2 * sizeof(boost::uint16_t) + 2 * sizeof(boost::uint32_t);
			pbitmap bmp = (*it)->interpret_as<pbitmap>();
			if (bmp == NULL)
			{
				std::cout << "Error: Bitmap " << (*it)->get_name() << " is malformed!" << std::endl;
				continue;
			}

			// Copy the BMP header
			data.resize(header_size, 0);
			memcpy(&data[0], bmp.get(), header_size);
			// Copy the image bytes.
			data.insert(data.end(), bmp->data.begin(), bmp->data.end());
		}
		else if ((*it)->get_type() == "RT_ICON" || (*it)->get_type() == "RT_CURSOR") {
			// Ignore the following resource types: we don't want to extract them.
			continue;
		}
		else // General case
		{
			ss << base << "_";
			if ((*it)->get_name() != "") {
				ss << (*it)->get_name();
			}
			else {
				ss << (*it)->get_id();
			}
			ss << "_" << (*it)->get_type() << ".raw";
			data = (*it)->get_raw_data();
		}

		if (data.size() == 0) 
		{
			std::cout << "Warning: Resource " << (*it)->get_name() << " is empty!" << std::endl;
			continue;
		}

		destination_file = bfs::path(destination_folder) / bfs::path(ss.str());
		f = fopen(destination_file.string().c_str(), "wb+");
		if (f == NULL)
		{
			std::cout << "Error: Could not open " << destination_file << "." << std::endl;
			return false;
		}
		if (data.size() != fwrite(&data[0], 1, data.size(), f)) 
		{
			fclose(f);
			std::cout << "Error: Could not write all the bytes for " << destination_file << "." << std::endl; 
			return false;
		}

		fclose(f);
	}
	return true;
}

} // !namespace sg