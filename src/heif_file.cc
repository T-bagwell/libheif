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

#include "heif_file.h"
#include "heif_image.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

using namespace heif;


HeifFile::HeifFile()
{
}


HeifFile::~HeifFile()
{
}


std::vector<heif_image_id> HeifFile::get_item_IDs() const
{
  std::vector<heif_image_id> IDs;

  for (const auto& image : m_images) {
    IDs.push_back(image.second.m_infe_box->get_item_ID());
  }

  return IDs;
}


Error HeifFile::read_from_file(const char* input_filename)
{
  m_input_stream = std::unique_ptr<std::istream>(new std::ifstream(input_filename));

  uint64_t maxSize = std::numeric_limits<uint64_t>::max();
  heif::BitstreamRange range(m_input_stream.get(), maxSize);


  Error error = parse_heif_file(range);
  return error;
}



Error HeifFile::read_from_memory(const void* data, size_t size)
{
  // TODO: Work on passed memory directly instead of creating a copy here.
  // Note: we cannot use basic_streambuf for this, because it does not support seeking
  std::string s(static_cast<const char*>(data), size);

  m_input_stream = std::unique_ptr<std::istream>(new std::istringstream(std::move(s)));

  heif::BitstreamRange range(m_input_stream.get(), size);

  Error error = parse_heif_file(range);
  return error;
}


std::string HeifFile::debug_dump_boxes() const
{
  std::stringstream sstr;

  bool first=true;

  for (const auto& box : m_top_level_boxes) {
    // dump box content for debugging

    if (first) {
      first = false;
    }
    else {
      sstr << "\n";
    }

    heif::Indent indent;
    sstr << box->dump(indent);
  }

  return sstr.str();
}


Error HeifFile::parse_heif_file(BitstreamRange& range)
{
  // --- read all top-level boxes

  for (;;) {
    std::shared_ptr<Box> box;
    Error error = Box::read(range, &box);
    if (error != Error::Ok || range.error() || range.eof()) {
      break;
    }

    m_top_level_boxes.push_back(box);


    // extract relevant boxes (ftyp, meta)

    if (box->get_short_type() == fourcc("meta")) {
      m_meta_box = std::dynamic_pointer_cast<Box_meta>(box);
    }

    if (box->get_short_type() == fourcc("ftyp")) {
      m_ftyp_box = std::dynamic_pointer_cast<Box_ftyp>(box);
    }
  }



  // --- check whether this is a HEIF file and its structural format

  if (!m_ftyp_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ftyp_box);
  }

  if (!m_ftyp_box->has_compatible_brand(fourcc("heic"))) {
    std::stringstream sstr;
    sstr << "File does not support the 'heic' brand.\n";

    return Error(heif_error_Unsupported_filetype,
                 heif_suberror_Unspecified,
                 sstr.str());
  }

  if (!m_meta_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_meta_box);
  }


  auto hdlr_box = std::dynamic_pointer_cast<Box_hdlr>(m_meta_box->get_child_box(fourcc("hdlr")));
  if (!hdlr_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_hdlr_box);
  }

  if (hdlr_box->get_handler_type() != fourcc("pict")) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_pict_handler);
  }


  // --- find mandatory boxes needed for image decoding

  auto pitm_box = std::dynamic_pointer_cast<Box_pitm>(m_meta_box->get_child_box(fourcc("pitm")));
  if (!pitm_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_pitm_box);
  }

  std::shared_ptr<Box> iprp_box = m_meta_box->get_child_box(fourcc("iprp"));
  if (!iprp_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iprp_box);
  }

  m_ipco_box = std::dynamic_pointer_cast<Box_ipco>(iprp_box->get_child_box(fourcc("ipco")));
  if (!m_ipco_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipco_box);
  }

  m_ipma_box = std::dynamic_pointer_cast<Box_ipma>(iprp_box->get_child_box(fourcc("ipma")));
  if (!m_ipma_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipma_box);
  }

  m_iloc_box = std::dynamic_pointer_cast<Box_iloc>(m_meta_box->get_child_box(fourcc("iloc")));
  if (!m_iloc_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iloc_box);
  }

  m_idat_box = std::dynamic_pointer_cast<Box_idat>(m_meta_box->get_child_box(fourcc("idat")));

  m_iref_box = std::dynamic_pointer_cast<Box_iref>(m_meta_box->get_child_box(fourcc("iref")));

  std::shared_ptr<Box> iinf_box = m_meta_box->get_child_box(fourcc("iinf"));
  if (!iinf_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iinf_box);
  }



  // --- build list of images

  m_primary_image_ID = pitm_box->get_item_ID();

  std::vector<std::shared_ptr<Box>> infe_boxes = iinf_box->get_child_boxes(fourcc("infe"));

  for (auto& box : infe_boxes) {
    std::shared_ptr<Box_infe> infe_box = std::dynamic_pointer_cast<Box_infe>(box);
    if (!infe_box) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_infe_box);
    }

    Image img;
    img.m_infe_box = infe_box;

    m_images.insert( std::make_pair(infe_box->get_item_ID(), img) );
  }

  return Error::Ok;
}


bool HeifFile::image_exists(heif_image_id ID) const
{
  auto image_iter = m_images.find(ID);
  return image_iter != m_images.end();
}


bool HeifFile::get_image_info(heif_image_id ID, const HeifFile::Image** image) const
{
  // --- get the image from the list of all images

  auto image_iter = m_images.find(ID);
  if (image_iter == m_images.end()) {
    return false;
  }

  *image = &image_iter->second;
  return true;
}


std::string HeifFile::get_item_type(heif_image_id ID) const
{
  const Image* img;
  if (!get_image_info(ID, &img)) {
    return "";
  }

  return img->m_infe_box->get_item_type();
}


Error HeifFile::get_properties(heif_image_id imageID,
                               std::vector<Box_ipco::Property>& properties) const
{
  if (!m_ipco_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipco_box);
  } else if (!m_ipma_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipma_box);
  }

  return m_ipco_box->get_properties_for_item_ID(imageID, m_ipma_box, properties);
}


Error HeifFile::get_compressed_image_data(heif_image_id ID, std::vector<uint8_t>* data) const {

  if (!image_exists(ID)) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Nonexisting_image_referenced);
  }

  const Image* image;
  if (!get_image_info(ID, &image)) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Nonexisting_image_referenced);
  }


  std::string item_type = image->m_infe_box->get_item_type();

  // --- get coded image data pointers

  auto items = m_iloc_box->get_items();
  const Box_iloc::Item* item = nullptr;
  for (const auto& i : items) {
    if (i.item_ID == ID) {
      item = &i;
      break;
    }
  }
  if (!item) {
    std::stringstream sstr;
    sstr << "Item with ID " << ID << " has no compressed data";

    return Error(heif_error_Invalid_input,
                 heif_suberror_No_item_data,
                 sstr.str());
  }

  Error error = Error(heif_error_Unsupported_feature,
                      heif_suberror_Unsupported_codec);
  if (item_type == "hvc1") {
    // --- --- --- HEVC

    // --- get properties for this image

    std::vector<Box_ipco::Property> properties;
    Error err = m_ipco_box->get_properties_for_item_ID(ID, m_ipma_box, properties);
    if (err) {
      return err;
    }

    // --- get codec configuration

    std::shared_ptr<Box_hvcC> hvcC_box;
    for (auto& prop : properties) {
      if (prop.property->get_short_type() == fourcc("hvcC")) {
        hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(prop.property);
        if (hvcC_box) {
          break;
        }
      }
    }

    if (!hvcC_box) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_hvcC_box);
    } else if (!hvcC_box->get_headers(data)) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_item_data);
    }

    error = m_iloc_box->read_data(*item, *m_input_stream.get(), m_idat_box, data);
  } else if (item_type == "grid" ||
             item_type == "iovl" ||
             item_type == "Exif") {
    error = m_iloc_box->read_data(*item, *m_input_stream.get(), m_idat_box, data);
  }

  if (error != Error::Ok) {
    return error;
  }

  return Error::Ok;
}
