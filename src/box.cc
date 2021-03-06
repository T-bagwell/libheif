/*
 * HEIF codec.
 * Copyright (c) 2017 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "box.h"

#include <sstream>
#include <iomanip>
#include <utility>

#include <iostream>


using namespace heif;

static const size_t MAX_CHILDREN_PER_BOX = 1024;
static const int MAX_ILOC_ITEMS = 1024;
static const int MAX_ILOC_EXTENTS_PER_ITEM = 32;
static const int MAX_MEMORY_BLOCK_SIZE = 50*1024*1024; // 50 MB

heif::Error heif::Error::Ok(heif_error_Ok);



Fraction Fraction::operator+(const Fraction& b) const
{
  if (denominator == b.denominator) {
    return Fraction { numerator + b.numerator, denominator };
  }
  else {
    return Fraction { numerator * b.denominator + b.numerator * denominator,
        denominator * b.denominator };
  }
}

Fraction Fraction::operator-(const Fraction& b) const
{
  if (denominator == b.denominator) {
    return Fraction { numerator - b.numerator, denominator };
  }
  else {
    return Fraction { numerator * b.denominator - b.numerator * denominator,
        denominator * b.denominator };
  }
}

Fraction Fraction::operator-(int v) const
{
  return Fraction { numerator - v * denominator, denominator };
}

Fraction Fraction::operator/(int v) const
{
  return Fraction { numerator, denominator*v };
}

int Fraction::round_down() const
{
  return numerator / denominator;
}

int Fraction::round_up() const
{
  return (numerator + denominator - 1)/denominator;
}

int Fraction::round() const
{
  return (numerator + denominator/2)/denominator;
}


static std::string to_fourcc(uint32_t code)
{
  std::string str("    ");
  str[0] = static_cast<char>((code>>24) & 0xFF);
  str[1] = static_cast<char>((code>>16) & 0xFF);
  str[2] = static_cast<char>((code>> 8) & 0xFF);
  str[3] = static_cast<char>((code>> 0) & 0xFF);

  return str;
}


heif::BoxHeader::BoxHeader()
{
}


std::vector<uint8_t> heif::BoxHeader::get_type() const
{
  if (m_type == fourcc("uuid")) {
    return m_uuid_type;
  }
  else {
    std::vector<uint8_t> type(4);
    type[0] = static_cast<uint8_t>((m_type>>24) & 0xFF);
    type[1] = static_cast<uint8_t>((m_type>>16) & 0xFF);
    type[2] = static_cast<uint8_t>((m_type>> 8) & 0xFF);
    type[3] = static_cast<uint8_t>((m_type>> 0) & 0xFF);
    return type;
  }
}


std::string heif::BoxHeader::get_type_string() const
{
  if (m_type == fourcc("uuid")) {
    // 8-4-4-4-12

    std::ostringstream sstr;
    sstr << std::hex;
    sstr << std::setfill('0');
    sstr << std::setw(2);

    for (int i=0;i<16;i++) {
      if (i==8 || i==12 || i==16 || i==20) {
        sstr << '-';
      }

      sstr << ((int)m_uuid_type[i]);
    }

    return sstr.str();
  }
  else {
    return to_fourcc(m_type);
  }
}


heif::Error heif::BoxHeader::parse(BitstreamRange& range)
{
  m_size = range.read32();
  m_type = range.read32();

  m_header_size = 8;

  if (m_size==1) {
    uint64_t high = range.read32();
    uint64_t low  = range.read32();

    m_size = (high<<32) | low;
    m_header_size += 8;
  }

  if (m_type==fourcc("uuid")) {
    if (range.read(16)) {
      m_uuid_type.resize(16);
      range.get_istream()->read((char*)m_uuid_type.data(), 16);
    }

    m_header_size += 16;
  }

  return range.get_error();
}


heif::Error heif::BoxHeader::write(std::ostream& ostr) const
{
  return Error::Ok;
}


std::string BoxHeader::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << indent << "Box: " << get_type_string() << " -----\n";
  sstr << indent << "size: " << get_box_size() << "   (header size: " << get_header_size() << ")\n";

  if (m_is_full_box) {
    sstr << indent << "version: " << ((int)m_version) << "\n"
         << indent << "flags: " << std::hex << m_flags << "\n";
  }

  return sstr.str();
}


Error Box::parse(BitstreamRange& range)
{
  // skip box

  if (get_box_size() == size_until_end_of_file) {
    range.skip_to_end_of_file();
  }
  else {
    uint64_t content_size = get_box_size() - get_header_size();
    if (range.read(content_size)) {
      range.get_istream()->seekg(get_box_size() - get_header_size(), std::ios_base::cur);
    }
  }

  // Note: seekg() clears the eof flag and it will not be set again afterwards,
  // hence we have to test for the fail flag.

  return range.get_error();
}


Error BoxHeader::parse_full_box_header(BitstreamRange& range)
{
  uint32_t data = range.read32();
  m_version = static_cast<uint8_t>(data >> 24);
  m_flags = data & 0x00FFFFFF;
  m_is_full_box = true;

  m_header_size += 4;

  return range.get_error();
}


Error Box::read(BitstreamRange& range, std::shared_ptr<heif::Box>* result)
{
  BoxHeader hdr;
  hdr.parse(range);
  if (range.error()) {
    return range.get_error();
  }

  std::shared_ptr<Box> box;

  switch (hdr.get_short_type()) {
  case fourcc("ftyp"):
    box = std::make_shared<Box_ftyp>(hdr);
    break;

  case fourcc("meta"):
    box = std::make_shared<Box_meta>(hdr);
    break;

  case fourcc("hdlr"):
    box = std::make_shared<Box_hdlr>(hdr);
    break;

  case fourcc("pitm"):
    box = std::make_shared<Box_pitm>(hdr);
    break;

  case fourcc("iloc"):
    box = std::make_shared<Box_iloc>(hdr);
    break;

  case fourcc("iinf"):
    box = std::make_shared<Box_iinf>(hdr);
    break;

  case fourcc("infe"):
    box = std::make_shared<Box_infe>(hdr);
    break;

  case fourcc("iprp"):
    box = std::make_shared<Box_iprp>(hdr);
    break;

  case fourcc("ipco"):
    box = std::make_shared<Box_ipco>(hdr);
    break;

  case fourcc("ipma"):
    box = std::make_shared<Box_ipma>(hdr);
    break;

  case fourcc("ispe"):
    box = std::make_shared<Box_ispe>(hdr);
    break;

  case fourcc("auxC"):
    box = std::make_shared<Box_auxC>(hdr);
    break;

  case fourcc("irot"):
    box = std::make_shared<Box_irot>(hdr);
    break;

  case fourcc("imir"):
    box = std::make_shared<Box_imir>(hdr);
    break;

  case fourcc("clap"):
    box = std::make_shared<Box_clap>(hdr);
    break;

  case fourcc("iref"):
    box = std::make_shared<Box_iref>(hdr);
    break;

  case fourcc("hvcC"):
    box = std::make_shared<Box_hvcC>(hdr);
    break;

  case fourcc("idat"):
    box = std::make_shared<Box_idat>(hdr);
    break;

  case fourcc("grpl"):
    box = std::make_shared<Box_grpl>(hdr);
    break;

  case fourcc("dinf"):
    box = std::make_shared<Box_dinf>(hdr);
    break;

  case fourcc("dref"):
    box = std::make_shared<Box_dref>(hdr);
    break;

  case fourcc("url "):
    box = std::make_shared<Box_url>(hdr);
    break;

  default:
    box = std::make_shared<Box>(hdr);
    break;
  }

  if (hdr.get_box_size() < hdr.get_header_size()) {
    std::stringstream sstr;
    sstr << "Box size (" << hdr.get_box_size() << " bytes) smaller than header size ("
         << hdr.get_header_size() << " bytes)";

    // Sanity check.
    return Error(heif_error_Invalid_input,
                 heif_suberror_Invalid_box_size,
                 sstr.str());
  }

  BitstreamRange boxrange(range.get_istream(),
                          hdr.get_box_size() - hdr.get_header_size(),
                          &range);

  Error err = box->parse(boxrange);
  if (err == Error::Ok) {
    *result = std::move(box);
  }

  boxrange.skip_to_end_of_box();

  return err;
}


std::string Box::dump(Indent& indent ) const
{
  std::ostringstream sstr;

  sstr << BoxHeader::dump(indent);

  return sstr.str();
}


std::shared_ptr<Box> Box::get_child_box(uint32_t short_type) const
{
  for (auto& box : m_children) {
    if (box->get_short_type()==short_type) {
      return box;
    }
  }

  return nullptr;
}


std::vector<std::shared_ptr<Box>> Box::get_child_boxes(uint32_t short_type) const
{
  std::vector<std::shared_ptr<Box>> result;
  for (auto& box : m_children) {
    if (box->get_short_type()==short_type) {
      result.push_back(box);
    }
  }

  return result;
}


Error Box::read_children(BitstreamRange& range, int max_number)
{
  int count = 0;

  while (!range.eof() && !range.error()) {
    std::shared_ptr<Box> box;
    Error error = Box::read(range, &box);
    if (error != Error::Ok) {
      return error;
    }

    if (m_children.size() > MAX_CHILDREN_PER_BOX) {
      std::stringstream sstr;
      sstr << "Maximum number of child boxes " << MAX_CHILDREN_PER_BOX << " exceeded.";

      // Sanity check.
      return Error(heif_error_Memory_allocation_error,
                   heif_suberror_Security_limit_exceeded,
                   sstr.str());
    }

    m_children.push_back(std::move(box));


    // count the new child and end reading new children when we reached the expected number

    count++;

    if (max_number != READ_CHILDREN_ALL &&
        count == max_number) {
      break;
    }
  }

  return range.get_error();
}


std::string Box::dump_children(Indent& indent) const
{
  std::ostringstream sstr;

  bool first = true;

  indent++;
  for (const auto& childBox : m_children) {
    if (first) {
      first=false;
    }
    else {
      sstr << indent << "\n";
    }

    sstr << childBox->dump(indent);
  }
  indent--;

  return sstr.str();
}


Error Box_ftyp::parse(BitstreamRange& range)
{
  m_major_brand = range.read32();
  m_minor_version = range.read32();

  if (get_box_size() <= get_header_size() + 8) {
    // Sanity check.
    return Error(heif_error_Invalid_input,
                 heif_suberror_Invalid_box_size,
                 "ftyp box too small (less than 8 bytes)");
  }

  uint64_t n_minor_brands = (get_box_size() - get_header_size() - 8) / 4;

  for (uint64_t i=0;i<n_minor_brands && !range.error();i++) {
    m_compatible_brands.push_back( range.read32() );
  }

  return range.get_error();
}


bool Box_ftyp::has_compatible_brand(uint32_t brand) const
{
  for (uint32_t b : m_compatible_brands) {
    if (b==brand) {
      return true;
    }
  }

  return false;
}


std::string Box_ftyp::dump(Indent& indent) const
{
  std::ostringstream sstr;

  sstr << BoxHeader::dump(indent);

  sstr << indent << "major brand: " << to_fourcc(m_major_brand) << "\n"
       << indent << "minor version: " << m_minor_version << "\n"
       << indent << "compatible brands: ";

  bool first=true;
  for (uint32_t brand : m_compatible_brands) {
    if (first) { first=false; }
    else { sstr << ','; }

    sstr << to_fourcc(brand);
  }
  sstr << "\n";

  return sstr.str();
}


Error Box_meta::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  /*
  uint64_t boxSizeLimit;
  if (get_box_size() == BoxHeader::size_until_end_of_file) {
    boxSizeLimit = sizeLimit;
  }
  else {
    boxSizeLimit = get_box_size() - get_header_size();
  }
  */

  return read_children(range);
}


std::string Box_meta::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_hdlr::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  m_pre_defined = range.read32();
  m_handler_type = range.read32();

  for (int i=0;i<3;i++) {
    m_reserved[i] = range.read32();
  }

  m_name = range.read_string();

  return range.get_error();
}


std::string Box_hdlr::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  sstr << indent << "pre_defined: " << m_pre_defined << "\n"
       << indent << "handler_type: " << to_fourcc(m_handler_type) << "\n"
       << indent << "name: " << m_name << "\n";

  return sstr.str();
}


Error Box_pitm::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  if (get_version()==0) {
    m_item_ID = range.read16();
  }
  else {
    m_item_ID = range.read32();
  }

  return range.get_error();
}


std::string Box_pitm::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  sstr << indent << "item_ID: " << m_item_ID << "\n";

  return sstr.str();
}


Error Box_iloc::parse(BitstreamRange& range)
{
  /*
  printf("box size: %d\n",get_box_size());
  printf("header size: %d\n",get_header_size());
  printf("start limit: %d\n",sizeLimit);
  */

  parse_full_box_header(range);

  uint16_t values4 = range.read16();

  int offset_size = (values4 >> 12) & 0xF;
  int length_size = (values4 >>  8) & 0xF;
  int base_offset_size = (values4 >> 4) & 0xF;
  int index_size = 0;

  if (get_version()>1) {
    index_size = (values4 & 0xF);
  }

  int item_count;
  if (get_version()<2) {
    item_count = range.read16();
  }
  else {
    item_count = range.read32();
  }

  // Sanity check.
  if (item_count > MAX_ILOC_ITEMS) {
    std::stringstream sstr;
    sstr << "iloc box contains " << item_count << " items, which exceeds the security limit of "
         << MAX_ILOC_ITEMS << " items.";

    return Error(heif_error_Memory_allocation_error,
                 heif_suberror_Security_limit_exceeded,
                 sstr.str());
  }

  for (int i=0;i<item_count;i++) {
    Item item;

    if (get_version()<2) {
      item.item_ID = range.read16();
    }
    else {
      item.item_ID = range.read32();
    }

    if (get_version()>=1) {
      values4 = range.read16();
      item.construction_method = (values4 & 0xF);
    }

    item.data_reference_index = range.read16();

    item.base_offset = 0;
    if (base_offset_size==4) {
      item.base_offset = range.read32();
    }
    else if (base_offset_size==8) {
      item.base_offset = ((uint64_t)range.read32()) << 32;
      item.base_offset |= range.read32();
    }

    int extent_count = range.read16();
    // Sanity check.
    if (extent_count > MAX_ILOC_EXTENTS_PER_ITEM) {
      std::stringstream sstr;
      sstr << "Number of extents in iloc box (" << extent_count << ") exceeds security limit ("
           << MAX_ILOC_EXTENTS_PER_ITEM << ")\n";

      return Error(heif_error_Memory_allocation_error,
                   heif_suberror_Security_limit_exceeded,
                   sstr.str());
    }

    for (int e=0;e<extent_count;e++) {
      Extent extent;

      if (get_version()>1 && index_size>0) {
        if (index_size==4) {
          extent.index = range.read32();
        }
        else if (index_size==8) {
          extent.index = ((uint64_t)range.read32()) << 32;
          extent.index |= range.read32();
        }
      }

      extent.offset = 0;
      if (offset_size==4) {
        extent.offset = range.read32();
      }
      else if (offset_size==8) {
        extent.offset = ((uint64_t)range.read32()) << 32;
        extent.offset |= range.read32();
      }


      extent.length = 0;
      if (length_size==4) {
        extent.length = range.read32();
      }
      else if (length_size==8) {
        extent.length = ((uint64_t)range.read32()) << 32;
        extent.length |= range.read32();
      }

      item.extents.push_back(extent);
    }

    if (!range.error()) {
      m_items.push_back(item);
    }
  }

  //printf("end limit: %d\n",sizeLimit);

  return range.get_error();
}


std::string Box_iloc::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  for (const Item& item : m_items) {
    sstr << indent << "item ID: " << item.item_ID << "\n"
         << indent << "  construction method: " << ((int)item.construction_method) << "\n"
         << indent << "  data_reference_index: " << std::hex
         << item.data_reference_index << std::dec << "\n"
         << indent << "  base_offset: " << item.base_offset << "\n";

    sstr << indent << "  extents: ";
    for (const Extent& extent : item.extents) {
      sstr << extent.offset << "," << extent.length;
      if (extent.index != 0) {
        sstr << ";index=" << extent.index;
      }
      sstr << " ";
    }
    sstr << "\n";
  }

  return sstr.str();
}


Error Box_iloc::read_data(const Item& item, std::istream& istr,
                          const std::shared_ptr<Box_idat>& idat,
                          std::vector<uint8_t>* dest) const
{
  istr.clear();

  for (const auto& extent : item.extents) {
    if (item.construction_method == 0) {
      istr.seekg(extent.offset + item.base_offset, std::ios::beg);
      if (istr.eof()) {
        // Out-of-bounds
        dest->clear();

        std::stringstream sstr;
        sstr << "Extent in iloc box references data outside of file bounds "
             << "(points to file position " << extent.offset + item.base_offset << ")\n";

        return Error(heif_error_Invalid_input,
                     heif_suberror_End_of_data,
                     sstr.str());
      }

      size_t old_size = dest->size();
      if (MAX_MEMORY_BLOCK_SIZE - old_size < extent.length) {
        std::stringstream sstr;
        sstr << "iloc box contained " << extent.length << " bytes, total memory size would be "
             << (old_size + extent.length) << " bytes, exceeding the security limit of "
             << MAX_MEMORY_BLOCK_SIZE << " bytes";

        return Error(heif_error_Memory_allocation_error,
                     heif_suberror_Security_limit_exceeded,
                     sstr.str());
      }

      dest->resize(static_cast<size_t>(old_size + extent.length));
      istr.read((char*)dest->data() + old_size, static_cast<size_t>(extent.length));
      if (istr.eof()) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_End_of_data);
      }
    }
    else if (item.construction_method==1) {
      if (!idat) {
        return Error(heif_error_Invalid_input,
                     heif_suberror_No_idat_box,
                     "idat box referenced in iref box is not present in file");
      }

      idat->read_data(istr,
                      extent.offset + item.base_offset,
                      extent.length,
                      *dest);
    }
  }

  return Error::Ok;
}


Error Box_infe::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  if (get_version() <= 1) {
    m_item_ID = range.read16();
    m_item_protection_index = range.read16();

    m_item_name = range.read_string();
    m_content_type = range.read_string();
    m_content_encoding = range.read_string();
  }

  if (get_version() >= 2) {
    m_hidden_item = (get_flags() & 1);

    if (get_version() == 2) {
      m_item_ID = range.read16();
    }
    else {
      m_item_ID = range.read32();
    }

    m_item_protection_index = range.read16();
    uint32_t item_type =range.read32();
    if (item_type != 0) {
      m_item_type = to_fourcc(item_type);
    }

    m_item_name = range.read_string();
    if (item_type == fourcc("mime")) {
      m_content_type = range.read_string();
      m_content_encoding = range.read_string();
    }
    else if (item_type == fourcc("uri ")) {
      m_item_uri_type = range.read_string();
    }
  }

  return range.get_error();
}


std::string Box_infe::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "item_ID: " << m_item_ID << "\n"
       << indent << "item_protection_index: " << m_item_protection_index << "\n"
       << indent << "item_type: " << m_item_type << "\n"
       << indent << "item_name: " << m_item_name << "\n"
       << indent << "content_type: " << m_content_type << "\n"
       << indent << "content_encoding: " << m_content_encoding << "\n"
       << indent << "item uri type: " << m_item_uri_type << "\n"
       << indent << "hidden item: " << std::boolalpha << m_hidden_item << "\n";

  return sstr.str();
}


Error Box_iinf::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  int nEntries_size = (get_version() > 0) ? 4 : 2;

  int item_count;
  if (nEntries_size==2) {
    item_count = range.read16();
  }
  else {
    item_count = range.read32();
  }

  if (item_count == 0) {
    return Error::Ok;
  }

  // TODO: Only try to read "item_count" children.
  return read_children(range);
}


std::string Box_iinf::dump(Indent& indent ) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_iprp::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  return read_children(range);
}


std::string Box_iprp::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_ipco::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  return read_children(range);
}


std::string Box_ipco::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_ipco::get_properties_for_item_ID(uint32_t itemID,
                                           const std::shared_ptr<class Box_ipma>& ipma,
                                           std::vector<Property>& out_properties) const
{
  const std::vector<Box_ipma::PropertyAssociation>* property_assoc = ipma->get_properties_for_item_ID(itemID);
  if (property_assoc == nullptr) {
    std::stringstream sstr;
    sstr << "Item (ID=" << itemID << ") has no properties assigned to it in ipma box";

    return Error(heif_error_Invalid_input,
                 heif_suberror_No_properties_assigned_to_item,
                 sstr.str());
  }

  auto allProperties = get_all_child_boxes();
  for (const  Box_ipma::PropertyAssociation& assoc : *property_assoc) {
    if (assoc.property_index > allProperties.size()) {
      std::stringstream sstr;
      sstr << "Nonexisting property (index=" << assoc.property_index << ") for item "
           << " ID=" << itemID << " referenced in ipma box";

      return Error(heif_error_Invalid_input,
                   heif_suberror_Ipma_box_references_nonexisting_property,
                   sstr.str());
    }

    Property prop;
    prop.essential = assoc.essential;

    if (assoc.property_index > 0) {
      prop.property = allProperties[assoc.property_index - 1];
      out_properties.push_back(prop);
    }
  }

  return Error::Ok;
}


Error Box_ispe::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  m_image_width = range.read32();
  m_image_height = range.read32();

  return range.get_error();
}


std::string Box_ispe::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "image width: " << m_image_width << "\n"
       << indent << "image height: " << m_image_height << "\n";

  return sstr.str();
}


Error Box_ipma::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  int entry_cnt = range.read32();
  for (int i = 0; i < entry_cnt && !range.error() && !range.eof(); i++) {
    Entry entry;
    if (get_version()<1) {
      entry.item_ID = range.read16();
    }
    else {
      entry.item_ID = range.read32();
    }

    int assoc_cnt = range.read8();
    for (int k=0;k<assoc_cnt;k++) {
      PropertyAssociation association;

      uint16_t index;
      if (get_flags() & 1) {
        index = range.read16();
        association.essential = !!(index & 0x8000);
        association.property_index = (index & 0x7fff);
      }
      else {
        index = range.read8();
        association.essential = !!(index & 0x80);
        association.property_index = (index & 0x7f);
      }

      entry.associations.push_back(association);
    }

    m_entries.push_back(entry);
  }

  return range.get_error();
}


const std::vector<Box_ipma::PropertyAssociation>* Box_ipma::get_properties_for_item_ID(uint32_t itemID) const
{
  for (const auto& entry : m_entries) {
    if (entry.item_ID == itemID) {
      return &entry.associations;
    }
  }

  return nullptr;
}


std::string Box_ipma::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  for (const Entry& entry : m_entries) {
    sstr << indent << "associations for item ID: " << entry.item_ID << "\n";
    indent++;
    for (const auto& assoc : entry.associations) {
      sstr << indent << "property index: " << assoc.property_index
           << " (essential: " << std::boolalpha << assoc.essential << ")\n";
    }
    indent--;
  }

  return sstr.str();
}


Error Box_auxC::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  m_aux_type = range.read_string();

  while (!range.eof()) {
    m_aux_subtypes.push_back( range.read8() );
  }

  return range.get_error();
}


std::string Box_auxC::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "aux type: " << m_aux_type << "\n"
       << indent << "aux subtypes: ";
  for (uint8_t subtype : m_aux_subtypes) {
    sstr << std::hex << std::setw(2) << std::setfill('0') << ((int)subtype) << " ";
  }

  sstr << "\n";

  return sstr.str();
}


Error Box_irot::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  uint16_t rotation = range.read8();
  rotation &= 0x03;

  m_rotation = rotation * 90;

  return range.get_error();
}


std::string Box_irot::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "rotation: " << m_rotation << " degrees (CCW)\n";

  return sstr.str();
}


Error Box_imir::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  uint16_t axis = range.read8();
  if (axis & 1) {
    m_axis = MirrorAxis::Horizontal;
  }
  else {
    m_axis = MirrorAxis::Vertical;
  }

  return range.get_error();
}


std::string Box_imir::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "mirror axis: ";
  switch (m_axis) {
  case MirrorAxis::Vertical:   sstr << "vertical\n"; break;
  case MirrorAxis::Horizontal: sstr << "horizontal\n"; break;
  }

  return sstr.str();
}


Error Box_clap::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  m_clean_aperture_width.numerator   = range.read32();
  m_clean_aperture_width.denominator = range.read32();
  m_clean_aperture_height.numerator   = range.read32();
  m_clean_aperture_height.denominator = range.read32();
  m_horizontal_offset.numerator   = range.read32();
  m_horizontal_offset.denominator = range.read32();
  m_vertical_offset.numerator   = range.read32();
  m_vertical_offset.denominator = range.read32();

  return range.get_error();
}


std::string Box_clap::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "clean_aperture: " << m_clean_aperture_width.numerator
       << "/" << m_clean_aperture_width.denominator << " x "
       << m_clean_aperture_height.numerator << "/"
       << m_clean_aperture_height.denominator << "\n";
  sstr << indent << "offset: " << m_horizontal_offset.numerator << "/"
       << m_horizontal_offset.denominator << " ; "
       << m_vertical_offset.numerator << "/"
       << m_vertical_offset.denominator << "\n";

  return sstr.str();
}


int Box_clap::left_rounded(int image_width) const
{
  // pcX = horizOff + (width  - 1)/2
  // pcX ± (cleanApertureWidth - 1)/2

  // left = horizOff + (width-1)/2 - (clapWidth-1)/2

  Fraction pcX  = m_horizontal_offset + Fraction(image_width-1, 2);
  Fraction left = pcX - (m_clean_aperture_width-1)/2;

  return left.round();
}

int Box_clap::right_rounded(int image_width) const
{
  Fraction pcX  = m_horizontal_offset + Fraction(image_width-1, 2);
  Fraction right = pcX + (m_clean_aperture_width-1)/2;

  return right.round();
}

int Box_clap::top_rounded(int image_height) const
{
  Fraction pcY  = m_vertical_offset + Fraction(image_height-1, 2);
  Fraction top = pcY - (m_clean_aperture_height-1)/2;

  return top.round();
}

int Box_clap::bottom_rounded(int image_height) const
{
  Fraction pcY  = m_vertical_offset + Fraction(image_height-1, 2);
  Fraction bottom = pcY + (m_clean_aperture_height-1)/2;

  return bottom.round();
}

int Box_clap::get_width_rounded() const
{
  int left  = (Fraction(0,1)-(m_clean_aperture_width-1)/2).round();
  int right = (  (m_clean_aperture_width-1)/2).round();

  return right+1-left;
}

int Box_clap::get_height_rounded() const
{
  int top    = (Fraction(0,1)-(m_clean_aperture_height-1)/2).round();
  int bottom = ( (m_clean_aperture_height-1)/2).round();

  return bottom+1-top;
}



Error Box_iref::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  while (!range.eof()) {
    Reference ref;

    Error err = ref.header.parse(range);
    if (err != Error::Ok) {
      return err;
    }

    if (get_version()==0) {
      ref.from_item_ID = range.read16();
      int nRefs = range.read16();
      for (int i=0;i<nRefs;i++) {
        ref.to_item_ID.push_back( range.read16() );
        if (range.eof()) {
          break;
        }
      }
    }
    else {
      ref.from_item_ID = range.read32();
      int nRefs = range.read16();
      for (int i=0;i<nRefs;i++) {
        ref.to_item_ID.push_back( range.read32() );
        if (range.eof()) {
          break;
        }
      }
    }

    m_references.push_back(ref);
  }

  return range.get_error();
}


std::string Box_iref::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  for (const auto& ref : m_references) {
    sstr << indent << "reference with type '" << ref.header.get_type_string() << "'"
         << " from ID: " << ref.from_item_ID
         << " to IDs: ";
    for (uint32_t id : ref.to_item_ID) {
      sstr << id << " ";
    }
    sstr << "\n";
  }

  return sstr.str();
}


bool Box_iref::has_references(uint32_t itemID) const
{
  for (const Reference& ref : m_references) {
    if (ref.from_item_ID == itemID) {
      return true;
    }
  }

  return false;
}


uint32_t Box_iref::get_reference_type(uint32_t itemID) const
{
  for (const Reference& ref : m_references) {
    if (ref.from_item_ID == itemID) {
      return ref.header.get_short_type();
    }
  }

  return 0;
}


std::vector<uint32_t> Box_iref::get_references(uint32_t itemID) const
{
  for (const Reference& ref : m_references) {
    if (ref.from_item_ID == itemID) {
      return ref.to_item_ID;
    }
  }

  return std::vector<uint32_t>();
}


Error Box_hvcC::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  uint8_t byte;

  m_configuration_version = range.read8();
  byte = range.read8();
  m_general_profile_space = (byte>>6) & 3;
  m_general_tier_flag = (byte>>5) & 1;
  m_general_profile_idc = (byte & 0x1F);

  m_general_profile_compatibility_flags = range.read32();

  for (int i=0; i<6; i++)
    {
      byte = range.read8();

      for (int b=0;b<8;b++) {
        m_general_constraint_indicator_flags[i*8+b] = (byte >> (7-b)) & 1;
      }
    }

  m_general_level_idc = range.read8();
  m_min_spatial_segmentation_idc = range.read16() & 0x0FFF;
  m_parallelism_type = range.read8() & 0x03;
  m_chroma_format = range.read8() & 0x03;
  m_bit_depth_luma = static_cast<uint8_t>((range.read8() & 0x07) + 8);
  m_bit_depth_chroma = static_cast<uint8_t>((range.read8() & 0x07) + 8);
  m_avg_frame_rate = range.read16();

  byte = range.read8();
  m_constant_frame_rate = (byte >> 6) & 0x03;
  m_num_temporal_layers = (byte >> 3) & 0x07;
  m_temporal_id_nested = (byte >> 2) & 1;
  m_length_size = static_cast<uint8_t>((byte & 0x03) + 1);

  int nArrays = range.read8();

  for (int i=0; i<nArrays && !range.error(); i++)
    {
      byte = range.read8();

      NalArray array;

      array.m_array_completeness = (byte >> 6) & 1;
      array.m_NAL_unit_type = (byte & 0x3F);

      int nUnits = range.read16();
      for (int u=0; u<nUnits && !range.error(); u++) {

        std::vector<uint8_t> nal_unit;
        int size = range.read16();
        if (!size) {
          // Ignore empty NAL units.
          continue;
        }

        if (range.read(size)) {
          nal_unit.resize(size);
          range.get_istream()->read((char*)nal_unit.data(), size);
        }

        array.m_nal_units.push_back( std::move(nal_unit) );
      }

      m_nal_array.push_back( std::move(array) );
    }

  range.skip_to_end_of_box();

  return range.get_error();
}


std::string Box_hvcC::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "configuration_version: " << ((int)m_configuration_version) << "\n"
       << indent << "general_profile_space: " << ((int)m_general_profile_space) << "\n"
       << indent << "general_tier_flag: " << m_general_tier_flag << "\n"
       << indent << "general_profile_idc: " << ((int)m_general_profile_idc) << "\n";

  sstr << indent << "general_profile_compatibility_flags: ";
  for (int i=0;i<32;i++) {
    sstr << ((m_general_profile_compatibility_flags>>(31-i))&1);
    if ((i%8)==7) sstr << ' ';
    else if ((i%4)==3) sstr << '.';
  }
  sstr << "\n";

  sstr << indent << "general_constraint_indicator_flags: ";
  int cnt=0;
  for (bool b : m_general_constraint_indicator_flags) {
    sstr << (b ? 1:0);
    cnt++;
    if ((cnt%8)==0)
      sstr << ' ';
  }
  sstr << "\n";

  sstr << indent << "general_level_idc: " << ((int)m_general_level_idc) << "\n"
       << indent << "min_spatial_segmentation_idc: " << m_min_spatial_segmentation_idc << "\n"
       << indent << "parallelism_type: " << ((int)m_parallelism_type) << "\n"
       << indent << "chroma_format: " << ((int)m_chroma_format) << "\n"
       << indent << "bit_depth_luma: " << ((int)m_bit_depth_luma) << "\n"
       << indent << "bit_depth_chroma: " << ((int)m_bit_depth_chroma) << "\n"
       << indent << "avg_frame_rate: " << m_avg_frame_rate << "\n"
       << indent << "constant_frame_rate: " << ((int)m_constant_frame_rate) << "\n"
       << indent << "num_temporal_layers: " << ((int)m_num_temporal_layers) << "\n"
       << indent << "temporal_id_nested: " << ((int)m_temporal_id_nested) << "\n"
       << indent << "length_size: " << ((int)m_length_size) << "\n";

  for (const auto& array : m_nal_array) {
    sstr << indent << "<array>\n";

    indent++;
    sstr << indent << "array_completeness: " << ((int)array.m_array_completeness) << "\n"
         << indent << "NAL_unit_type: " << ((int)array.m_NAL_unit_type) << "\n";

    for (const auto& unit : array.m_nal_units) {
      //sstr << "  unit with " << unit.size() << " bytes of data\n";
      sstr << indent;
      for (uint8_t b : unit) {
        sstr << std::setfill('0') << std::setw(2) << std::hex << ((int)b) << " ";
      }
      sstr << "\n";
      sstr << std::dec;
    }

    indent--;
  }

  return sstr.str();
}


bool Box_hvcC::get_headers(std::vector<uint8_t>* dest) const
{
  for (const auto& array : m_nal_array) {
    for (const auto& unit : array.m_nal_units) {

      dest->push_back( (unit.size()>>24) & 0xFF );
      dest->push_back( (unit.size()>>16) & 0xFF );
      dest->push_back( (unit.size()>> 8) & 0xFF );
      dest->push_back( (unit.size()>> 0) & 0xFF );

      /*
      dest->push_back(0);
      dest->push_back(0);
      dest->push_back(1);
      */

      dest->insert(dest->end(), unit.begin(), unit.end());
    }
  }

  return true;
}


Error Box_idat::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  m_data_start_pos = range.get_istream()->tellg();

  return range.get_error();
}


std::string Box_idat::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "number of data bytes: " << get_box_size() - get_header_size() << "\n";

  return sstr.str();
}


Error Box_idat::read_data(std::istream& istr, uint64_t start, uint64_t length,
                          std::vector<uint8_t>& out_data) const
{
  // move to start of data
  istr.seekg(m_data_start_pos + (std::streampos)start, std::ios_base::beg);

  // reserve space for the data in the output array
  auto curr_size = out_data.size();

  if (MAX_MEMORY_BLOCK_SIZE - curr_size < length) {
    std::stringstream sstr;
    sstr << "idat box contained " << length << " bytes, total memory size would be "
         << (curr_size + length) << " bytes, exceeding the security limit of "
         << MAX_MEMORY_BLOCK_SIZE << " bytes";

    return Error(heif_error_Memory_allocation_error,
                 heif_suberror_Security_limit_exceeded,
                 sstr.str());
  }

  out_data.resize(static_cast<size_t>(curr_size + length));
  uint8_t* data = &out_data[curr_size];

  istr.read((char*)data, static_cast<size_t>(length));

  return Error::Ok;
}


Error Box_grpl::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  //return read_children(range);

  while (!range.eof()) {
    EntityGroup group;
    Error err = group.header.parse(range);
    if (err != Error::Ok) {
      return err;
    }

    err = group.header.parse_full_box_header(range);
    if (err != Error::Ok) {
      return err;
    }

    group.group_id = range.read32();
    int nEntities = range.read32();
    for (int i=0;i<nEntities;i++) {
      if (range.eof()) {
        break;
      }

      group.entity_ids.push_back( range.read32() );
    }

    m_entity_groups.push_back(group);
  }

  return range.get_error();
}


std::string Box_grpl::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  for (const auto& group : m_entity_groups) {
    sstr << indent << "group type: " << group.header.get_type_string() << "\n"
         << indent << "| group id: " << group.group_id << "\n"
         << indent << "| entity IDs: ";

    for (uint32_t id : group.entity_ids) {
      sstr << id << " ";
    }

    sstr << "\n";
  }

  return sstr.str();
}


Error Box_dinf::parse(BitstreamRange& range)
{
  //parse_full_box_header(range);

  return read_children(range);
}


std::string Box_dinf::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_dref::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  int nEntities = range.read32();

  /*
  for (int i=0;i<nEntities;i++) {
    if (range.eof()) {
      break;
    }
  }
  */

  Error err = read_children(range, nEntities);
  if (err) {
    return err;
  }

  if ((int)m_children.size() != nEntities) {
    // TODO return Error(
  }

  return err;
}


std::string Box_dref::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  sstr << dump_children(indent);

  return sstr.str();
}


Error Box_url::parse(BitstreamRange& range)
{
  parse_full_box_header(range);

  m_location = range.read_string();

  return range.get_error();
}


std::string Box_url::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);
  //sstr << dump_children(indent);

  sstr << indent << "location: " << m_location << "\n";

  return sstr.str();
}
