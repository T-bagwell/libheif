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

#include <assert.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>
#include <math.h>

#include "heif_context.h"
#include "heif_file.h"
#include "heif_image.h"
#include "heif_api_structs.h"

#if HAVE_LIBDE265
#include "heif_decoder_libde265.h"
#endif


using namespace heif;


static int32_t readvec_signed(const std::vector<uint8_t>& data,int& ptr,int len)
{
  const uint32_t high_bit = 0x80<<((len-1)*8);

  uint32_t val=0;
  while (len--) {
    val <<= 8;
    val |= data[ptr++];
  }

  bool negative = (val & high_bit) != 0;
  val &= ~high_bit;

  if (negative) {
    return -(high_bit-val);
  }
  else {
    return val;
  }

  return val;
}


static uint32_t readvec(const std::vector<uint8_t>& data,int& ptr,int len)
{
  uint32_t val=0;
  while (len--) {
    val <<= 8;
    val |= data[ptr++];
  }

  return val;
}


class ImageGrid
{
public:
  Error parse(const std::vector<uint8_t>& data);

  std::string dump() const;

  uint32_t get_width() const { return m_output_width; }
  uint32_t get_height() const { return m_output_height; }
  uint16_t get_rows() const { return m_rows; }
  uint16_t get_columns() const { return m_columns; }

private:
  uint16_t m_rows;
  uint16_t m_columns;
  uint32_t m_output_width;
  uint32_t m_output_height;
};


Error ImageGrid::parse(const std::vector<uint8_t>& data)
{
  if (data.size() < 8) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Invalid_grid_data,
                 "Less than 8 bytes of data");
  }

  uint8_t version = data[0];
  (void)version; // version is unused

  uint8_t flags = data[1];
  int field_size = ((flags & 1) ? 32 : 16);

  m_rows    = static_cast<uint16_t>(data[2] +1);
  m_columns = static_cast<uint16_t>(data[3] +1);

  if (field_size == 32) {
    if (data.size() < 12) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_Invalid_grid_data,
                   "Grid image data incomplete");
    }

    m_output_width = ((data[4] << 24) |
                      (data[5] << 16) |
                      (data[6] <<  8) |
                      (data[7]));

    m_output_height = ((data[ 8] << 24) |
                       (data[ 9] << 16) |
                       (data[10] <<  8) |
                       (data[11]));
  }
  else {
    m_output_width = ((data[4] << 8) |
                      (data[5]));

    m_output_height = ((data[ 6] << 8) |
                       (data[ 7]));
  }

  return Error::Ok;
}


std::string ImageGrid::dump() const
{
  std::ostringstream sstr;

  sstr << "rows: " << m_rows << "\n"
       << "columns: " << m_columns << "\n"
       << "output width: " << m_output_width << "\n"
       << "output height: " << m_output_height << "\n";

  return sstr.str();
}



class ImageOverlay
{
public:
  Error parse(size_t num_images, const std::vector<uint8_t>& data);

  std::string dump() const;

  void get_background_color(uint16_t col[4]) const;

  uint32_t get_canvas_width() const { return m_width; }
  uint32_t get_canvas_height() const { return m_height; }

  size_t get_num_offsets() const { return m_offsets.size(); }
  void get_offset(size_t image_index, int32_t* x, int32_t* y) const;

private:
  uint8_t  m_version;
  uint8_t  m_flags;
  uint16_t m_background_color[4];
  uint32_t m_width;
  uint32_t m_height;

  struct Offset {
    int32_t x,y;
  };

  std::vector<Offset> m_offsets;
};


Error ImageOverlay::parse(size_t num_images, const std::vector<uint8_t>& data)
{
  Error eofError(heif_error_Invalid_input,
                 heif_suberror_Invalid_grid_data,
                 "Overlay image data incomplete");

  if (data.size() < 2 + 4*2) {
    return eofError;
  }

  m_version = data[0];
  m_flags = data[1];

  if (m_version != 0) {
    std::stringstream sstr;
    sstr << "Overlay image data version " << m_version << " is not implemented yet";

    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unsupported_data_version,
                 sstr.str());
  }

  int field_len = ((m_flags & 1) ? 4 : 2);
  int ptr=2;

  if (ptr + 4*2 + 2*field_len + num_images*2*field_len > data.size()) {
    return eofError;
  }

  for (int i=0;i<4;i++) {
    uint16_t color = static_cast<uint16_t>(readvec(data,ptr,2));
    m_background_color[i] = color;
  }

  m_width  = readvec(data,ptr,field_len);
  m_height = readvec(data,ptr,field_len);

  m_offsets.resize(num_images);

  for (size_t i=0;i<num_images;i++) {
    m_offsets[i].x = readvec_signed(data,ptr,field_len);
    m_offsets[i].y = readvec_signed(data,ptr,field_len);
  }

  return Error::Ok;
}


std::string ImageOverlay::dump() const
{
  std::stringstream sstr;

  sstr << "version: " << ((int)m_version) << "\n"
       << "flags: " << ((int)m_flags) << "\n"
       << "background color: " << m_background_color[0]
       << ";" << m_background_color[1]
       << ";" << m_background_color[2]
       << ";" << m_background_color[3] << "\n"
       << "canvas size: " << m_width << "x" << m_height << "\n"
       << "offsets: ";

  for (const Offset& offset : m_offsets) {
    sstr << offset.x << ";" << offset.y << " ";
  }
  sstr << "\n";

  return sstr.str();
}


void ImageOverlay::get_background_color(uint16_t col[4]) const
{
  for (int i=0;i<4;i++) {
    col[i] = m_background_color[i];
  }
}


void ImageOverlay::get_offset(size_t image_index, int32_t* x, int32_t* y) const
{
  assert(image_index>=0 && image_index<m_offsets.size());
  assert(x && y);

  *x = m_offsets[image_index].x;
  *y = m_offsets[image_index].y;
}





HeifContext::HeifContext()
{
#if HAVE_LIBDE265
  register_decoder(get_decoder_plugin_libde265());
#endif
}

HeifContext::~HeifContext()
{
}

Error HeifContext::read_from_file(const char* input_filename)
{
  m_heif_file = std::make_shared<HeifFile>();
  Error err = m_heif_file->read_from_file(input_filename);
  if (err) {
    return err;
  }

  return interpret_heif_file();
}

Error HeifContext::read_from_memory(const void* data, size_t size)
{
  m_heif_file = std::make_shared<HeifFile>();
  Error err = m_heif_file->read_from_memory(data,size);
  if (err) {
    return err;
  }

  return interpret_heif_file();
}

std::string HeifContext::debug_dump_boxes() const
{
  return m_heif_file->debug_dump_boxes();
}

void HeifContext::register_decoder(const heif_decoder_plugin* decoder_plugin)
{
  m_decoder_plugins.insert(decoder_plugin);
}

const struct heif_decoder_plugin* HeifContext::get_decoder(uint32_t type) const
{
  int highest_priority = 0;
  const struct heif_decoder_plugin* best_plugin = nullptr;

  for (const auto* plugin : m_decoder_plugins) {
    int priority = plugin->does_support_format(type);
    if (priority > highest_priority) {
      highest_priority = priority;
      best_plugin = plugin;
    }
  }

  return best_plugin;
}


static bool item_type_is_image(const std::string& item_type)
{
  return (item_type=="hvc1" ||
          item_type=="grid" ||
          item_type=="iden" ||
          item_type=="iovl");
}


void HeifContext::remove_top_level_image(std::shared_ptr<Image> image)
{
  std::vector<std::shared_ptr<Image>> new_list;

  for (auto img : m_top_level_images) {
    if (img != image) {
      new_list.push_back(img);
    }
  }

  m_top_level_images = new_list;
}


class SEIMessage
{
public:
  virtual ~SEIMessage() { }
};


class SEIMessage_depth_representation_info : public SEIMessage,
                                             public heif_depth_representation_info
{
public:
};


double read_depth_rep_info_element(BitReader& reader)
{
  int sign_flag = reader.get_bits(1);
  int exponent  = reader.get_bits(7);
  int mantissa_len = reader.get_bits(5)+1;
  if (mantissa_len<1 || mantissa_len>32) {
    // TODO err
  }

  if (exponent==127) {
    // TODO value unspecified
  }

  int mantissa = reader.get_bits(mantissa_len);
  double value;

  //printf("sign:%d exponent:%d mantissa_len:%d mantissa:%d\n",sign_flag,exponent,mantissa_len,mantissa);

  if (exponent > 0) {
    value = pow(2, exponent-31) * (1.0 + mantissa / pow(2,mantissa_len));
  }
  else {
    value = pow(2, -(30+mantissa_len)) * mantissa;
  }

  if (sign_flag) {
    value = -value;
  }

  return value;
}


std::shared_ptr<SEIMessage> read_depth_representation_info(BitReader& reader)
{
  auto msg = std::make_shared<SEIMessage_depth_representation_info>();


  // default values

  msg->version = 1;

  msg->disparity_reference_view = 0;
  msg->depth_nonlinear_representation_model_size = 0;
  msg->depth_nonlinear_representation_model = nullptr;


  // read header

  msg->has_z_near = (uint8_t)reader.get_bits(1);
  msg->has_z_far  = (uint8_t)reader.get_bits(1);
  msg->has_d_min  = (uint8_t)reader.get_bits(1);
  msg->has_d_max  = (uint8_t)reader.get_bits(1);

  int rep_type;
  if (!reader.get_uvlc(&rep_type)) {
    // TODO error
  }
  // TODO: check rep_type range
  msg->depth_representation_type = (enum heif_depth_representation_type)rep_type;

  //printf("flags: %d %d %d %d\n",msg->has_z_near,msg->has_z_far,msg->has_d_min,msg->has_d_max);
  //printf("type: %d\n",rep_type);

  if (msg->has_d_min || msg->has_d_max) {
    int ref_view;
    if (!reader.get_uvlc(&ref_view)) {
      // TODO error
    }
    msg->disparity_reference_view = ref_view;

    //printf("ref_view: %d\n",msg->disparity_reference_view);
  }

  if (msg->has_z_near) msg->z_near = read_depth_rep_info_element(reader);
  if (msg->has_z_far ) msg->z_far  = read_depth_rep_info_element(reader);
  if (msg->has_d_min ) msg->d_min  = read_depth_rep_info_element(reader);
  if (msg->has_d_max ) msg->d_max  = read_depth_rep_info_element(reader);

  /*
  printf("z_near: %f\n",msg->z_near);
  printf("z_far: %f\n",msg->z_far);
  printf("dmin: %f\n",msg->d_min);
  printf("dmax: %f\n",msg->d_max);
  */

  if (msg->depth_representation_type == heif_depth_representation_type_nonuniform_disparity) {
    // TODO: load non-uniform response curve
  }

  return msg;
}


// aux subtypes: 00 00 00 11 / 00 00 00 0d / 4e 01 / b1 09 / 35 1e 78 c8 01 03 c5 d0 20

Error decode_hevc_aux_sei_messages(const std::vector<uint8_t>& data,
                                   std::vector<std::shared_ptr<SEIMessage>>& msgs)
{
  // TODO: we probably do not need a full BitReader just for the array size.
  // Read this and the NAL size directly on the array data.

  BitReader reader(data.data(), (int)data.size());
  uint32_t len = (uint32_t)reader.get_bits(32);

  if (len > data.size()-4) {
    // ERROR: read past end of data
  }

  while (reader.get_current_byte_index() < (int)len) {
    int currPos = reader.get_current_byte_index();
    BitReader sei_reader(data.data() + currPos, (int)data.size()-currPos);

    uint32_t nal_size = (uint32_t)sei_reader.get_bits(32);
    (void)nal_size;

    uint8_t nal_type = (uint8_t)(sei_reader.get_bits(8) >> 1);
    sei_reader.skip_bits(8);

    // SEI

    if (nal_type == 39 ||
        nal_type == 40) {

      // TODO: loading of multi-byte sei headers
      uint8_t payload_id = (uint8_t)(sei_reader.get_bits(8));
      uint8_t payload_size = (uint8_t)(sei_reader.get_bits(8));
      (void)payload_size;

      switch (payload_id) {
      case 177: // depth_representation_info
        std::shared_ptr<SEIMessage> sei = read_depth_representation_info(sei_reader);
        msgs.push_back(sei);
        break;
      }
    }

    break; // TODO: read next SEI
  }


  return Error::Ok;
}


Error HeifContext::interpret_heif_file()
{
  m_all_images.clear();
  m_top_level_images.clear();
  m_primary_image.reset();


  // --- reference all non-hidden images

  std::vector<heif_image_id> image_IDs = m_heif_file->get_item_IDs();

  for (heif_image_id id : image_IDs) {
    auto infe_box = m_heif_file->get_infe_box(id);
    if (!infe_box) {
      // TODO(farindk): Should we return an error instead of skipping the invalid id?
      continue;
    }

    if (item_type_is_image(infe_box->get_item_type())) {
      auto image = std::make_shared<Image>(this, id);
      m_all_images.insert(std::make_pair(id, image));

      if (!infe_box->is_hidden_item()) {
        if (id==m_heif_file->get_primary_image_ID()) {
          image->set_primary(true);
          m_primary_image = image;
        }

        m_top_level_images.push_back(image);
      }
    }
  }


  if (!m_primary_image) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Nonexisting_image_referenced,
                 "'pitm' box references a non-existing image");
  }


  // --- remove thumbnails from top-level images and assign to their respective image

  auto iref_box = m_heif_file->get_iref_box();
  if (iref_box) {
    // m_top_level_images.clear();

    for (auto& pair : m_all_images) {
      auto& image = pair.second;

      uint32_t type = iref_box->get_reference_type(image->get_id());

      if (type==fourcc("thmb")) {
        // --- this is a thumbnail image, attach to the main image

        std::vector<heif_image_id> refs = iref_box->get_references(image->get_id());
        if (refs.size() != 1) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_Unspecified,
                       "Too many thumbnail references");
        }

        image->set_is_thumbnail_of(refs[0]);

        auto master_iter = m_all_images.find(refs[0]);
        if (master_iter == m_all_images.end()) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_Nonexisting_image_referenced,
                       "Thumbnail references a non-existing image");
        }

        if (master_iter->second->is_thumbnail()) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_Nonexisting_image_referenced,
                       "Thumbnail references another thumbnail");
        }

        master_iter->second->add_thumbnail(image);

        remove_top_level_image(image);
      }
      else if (type==fourcc("auxl")) {

        // --- this is an auxiliary image
        //     check whether it is an alpha channel and attach to the main image if yes

        std::vector<Box_ipco::Property> properties;
        Error err = m_heif_file->get_properties(image->get_id(), properties);
        if (err) {
          return err;
        }

        std::shared_ptr<Box_auxC> auxC_property;
        for (const auto& property : properties) {
          auto auxC = std::dynamic_pointer_cast<Box_auxC>(property.property);
          if (auxC) {
            auxC_property = auxC;
          }
        }

        if (!auxC_property) {
          std::stringstream sstr;
          sstr << "No auxC property for image " << image->get_id();
          return Error(heif_error_Invalid_input,
                       heif_suberror_Auxiliary_image_type_unspecified,
                       sstr.str());
        }

        std::vector<heif_image_id> refs = iref_box->get_references(image->get_id());
        if (refs.size() != 1) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_Unspecified,
                       "Too many auxiliary image references");
        }


        // alpha channel

        if (auxC_property->get_aux_type() == "urn:mpeg:avc:2015:auxid:1" ||
            auxC_property->get_aux_type() == "urn:mpeg:hevc:2015:auxid:1") {
          image->set_is_alpha_channel_of(refs[0]);

          auto master_iter = m_all_images.find(refs[0]);
          master_iter->second->set_alpha_channel(image);
        }


        // depth channel

        if (auxC_property->get_aux_type() == "urn:mpeg:hevc:2015:auxid:2") {
          image->set_is_depth_channel_of(refs[0]);

          auto master_iter = m_all_images.find(refs[0]);
          master_iter->second->set_depth_channel(image);

          auto subtypes = auxC_property->get_subtypes();

          std::vector<std::shared_ptr<SEIMessage>> sei_messages;
          Error err = decode_hevc_aux_sei_messages(subtypes, sei_messages);

          for (auto& msg : sei_messages) {
            auto depth_msg = std::dynamic_pointer_cast<SEIMessage_depth_representation_info>(msg);
            if (depth_msg) {
              image->set_depth_representation_info(*depth_msg);
            }
          }
        }

        remove_top_level_image(image);
      }
      else {
        // 'image' is a normal image, keep it as a top-level image
      }
    }
  }


  // --- read through properties for each image and extract image resolutions

  for (auto& pair : m_all_images) {
    auto& image = pair.second;

    std::vector<Box_ipco::Property> properties;

    Error err = m_heif_file->get_properties(pair.first, properties);
    if (err) {
      return err;
    }

    bool ispe_read = false;
    for (const auto& prop : properties) {
      auto ispe = std::dynamic_pointer_cast<Box_ispe>(prop.property);
      if (ispe) {
        uint32_t width = ispe->get_width();
        uint32_t height = ispe->get_height();


        // --- check whether the image size is "too large"

        if (width  >= static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            height >= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
          std::stringstream sstr;
          sstr << "Image size " << width << "x" << height << " exceeds the maximum image size "
               << std::numeric_limits<int>::max() << "x"
               << std::numeric_limits<int>::max() << "\n";

          return Error(heif_error_Memory_allocation_error,
                       heif_suberror_Security_limit_exceeded,
                       sstr.str());
        }

        image->set_resolution(width, height);
        ispe_read = true;
      }

      if (ispe_read) {
        auto clap = std::dynamic_pointer_cast<Box_clap>(prop.property);
        if (clap) {
          image->set_resolution( clap->get_width_rounded(),
                                 clap->get_height_rounded() );
        }

        auto irot = std::dynamic_pointer_cast<Box_irot>(prop.property);
        if (irot) {
          if (irot->get_rotation()==90 ||
              irot->get_rotation()==270) {
            // swap width and height
            image->set_resolution( image->get_height(),
                                   image->get_width() );
          }
        }
      }
    }
  }



  // --- read metadata and assign to image

  for (uint32_t id : image_IDs) {
    std::string item_type = m_heif_file->get_item_type(id);
    if (item_type == "Exif") {
      std::shared_ptr<ImageMetadata> metadata = std::make_shared<ImageMetadata>();
      metadata->item_type = item_type;

      Error err = m_heif_file->get_compressed_image_data(id, &(metadata->m_data));
      if (err) {
        return err;
      }

      //std::cerr.write((const char*)data.data(), data.size());


      // --- assign metadata to the image

      if (iref_box) {
        uint32_t type = iref_box->get_reference_type(id);
        if (type == fourcc("cdsc")) {
          std::vector<uint32_t> refs = iref_box->get_references(id);
          if (refs.size() != 1) {
            return Error(heif_error_Invalid_input,
                         heif_suberror_Unspecified,
                         "Exif data not correctly assigned to image");
          }

          uint32_t exif_image_id = refs[0];
          auto img_iter = m_all_images.find(exif_image_id);
          if (img_iter == m_all_images.end()) {
            return Error(heif_error_Invalid_input,
                         heif_suberror_Nonexisting_image_referenced,
                         "Exif data assigned to non-existing image");
          }

          img_iter->second->add_metadata(metadata);
        }
      }
    }
  }

  return Error::Ok;
}


HeifContext::Image::Image(HeifContext* context, heif_image_id id)
  : m_heif_context(context),
    m_id(id)
{
}

HeifContext::Image::~Image()
{
}

Error HeifContext::Image::decode_image(std::shared_ptr<HeifPixelImage>& img,
                                       heif_colorspace colorspace,
                                       heif_chroma chroma,
                                       const struct heif_decoding_options* options) const
{
  Error err = m_heif_context->decode_image(m_id, img, options);
  if (err) {
    return err;
  }

  heif_chroma target_chroma = (chroma == heif_chroma_undefined ?
                               img->get_chroma_format() :
                               chroma);
  heif_colorspace target_colorspace = (colorspace == heif_colorspace_undefined ?
                                       img->get_colorspace() :
                                       colorspace);

  bool different_chroma = (target_chroma != img->get_chroma_format());
  bool different_colorspace = (target_colorspace != img->get_colorspace());

  if (different_chroma || different_colorspace) {
    img = img->convert_colorspace(target_colorspace, target_chroma);
    if (!img) {
      return Error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion);
    }
  }

  return err;
}


Error HeifContext::decode_image(heif_image_id ID,
                                std::shared_ptr<HeifPixelImage>& img,
                                const struct heif_decoding_options* options) const
{
  std::string image_type = m_heif_file->get_item_type(ID);

  Error error;


  // --- decode image, depending on its type

  if (image_type == "hvc1") {
    const struct heif_decoder_plugin* decoder_plugin = get_decoder(heif_compression_HEVC);
    if (!decoder_plugin) {
      return Error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec);
    }

    std::vector<uint8_t> data;
    error = m_heif_file->get_compressed_image_data(ID, &data);
    if (error) {
      return error;
    }

    void* decoder;
    struct heif_error err = decoder_plugin->new_decoder(&decoder);
    if (err.code != heif_error_Ok) {
      return Error(err.code, err.subcode, err.message);
    }

    err = decoder_plugin->push_data(decoder, data.data(), data.size());
    if (err.code != heif_error_Ok) {
      decoder_plugin->free_decoder(decoder);
      return Error(err.code, err.subcode, err.message);
    }

    //std::shared_ptr<HeifPixelImage>* decoded_img;

    heif_image* decoded_img = nullptr;
    err = decoder_plugin->decode_image(decoder, &decoded_img);
    if (err.code != heif_error_Ok) {
      decoder_plugin->free_decoder(decoder);
      return Error(err.code, err.subcode, err.message);
    }

    if (!decoded_img) {
      // TODO(farindk): The plugin should return an error in this case.
      decoder_plugin->free_decoder(decoder);
      return Error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified);
    }

    img = std::move(decoded_img->image);
    heif_image_release(decoded_img);

    decoder_plugin->free_decoder(decoder);

#if 0
    FILE* fh = fopen("out.bin", "wb");
    fwrite(data.data(), 1, data.size(), fh);
    fclose(fh);
#endif
  }
  else if (image_type == "grid") {
    std::vector<uint8_t> data;
    error = m_heif_file->get_compressed_image_data(ID, &data);
    if (error) {
      return error;
    }

    error = decode_full_grid_image(ID, img, data);
    if (error) {
      return error;
    }
  }
  else if (image_type == "iden") {
    error = decode_derived_image(ID, img);
    if (error) {
      return error;
    }
  }
  else if (image_type == "iovl") {
    std::vector<uint8_t> data;
    error = m_heif_file->get_compressed_image_data(ID, &data);
    if (error) {
      return error;
    }

    error = decode_overlay_image(ID, img, data);
    if (error) {
      return error;
    }
  }
  else {
    // Should not reach this, was already rejected by "get_image_data".
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unsupported_image_type);
  }



  // --- add alpha channel, if available

  // TODO: this if statement is probably wrong. When we have a tiled image with alpha
  // channel, then the alpha images should be associated with their respective tiles.
  // However, the tile images are not part of the m_all_images list.
  // Fix this, when we have a test image available.
  if (m_all_images.find(ID) != m_all_images.end()) {
    const auto imginfo = m_all_images.find(ID)->second;

    std::shared_ptr<Image> alpha_image = imginfo->get_alpha_channel();
    if (alpha_image) {
      std::shared_ptr<HeifPixelImage> alpha;
      Error err = alpha_image->decode_image(alpha);
      if (err) {
        return err;
      }

      // TODO: check that sizes are the same and that we have an Y channel
      // BUT: is there any indication in the standard that the alpha channel should have the same size?

      img->transfer_plane_from_image_as(alpha, heif_channel_Y, heif_channel_Alpha);
    }
  }


  // --- apply image transformations

  if (!options || options->ignore_transformations == false) {
    std::vector<Box_ipco::Property> properties;
    auto ipco_box = m_heif_file->get_ipco_box();
    auto ipma_box = m_heif_file->get_ipma_box();
    error = ipco_box->get_properties_for_item_ID(ID, ipma_box, properties);

    for (const auto& property : properties) {
      auto rot = std::dynamic_pointer_cast<Box_irot>(property.property);
      if (rot) {
        std::shared_ptr<HeifPixelImage> rotated_img;
        error = img->rotate_ccw(rot->get_rotation(), rotated_img);
        if (error) {
          return error;
        }

        img = rotated_img;
      }


      auto mirror = std::dynamic_pointer_cast<Box_imir>(property.property);
      if (mirror) {
        error = img->mirror_inplace(mirror->get_mirror_axis() == Box_imir::MirrorAxis::Horizontal);
        if (error) {
          return error;
        }
      }


      auto clap = std::dynamic_pointer_cast<Box_clap>(property.property);
      if (clap) {
        std::shared_ptr<HeifPixelImage> clap_img;

        int img_width = img->get_width();
        int img_height = img->get_height();
        assert(img_width >= 0);
        assert(img_height >= 0);

        int left = clap->left_rounded(img_width);
        int right = clap->right_rounded(img_width);
        int top = clap->top_rounded(img_height);
        int bottom = clap->bottom_rounded(img_height);

        if (left < 0) { left = 0; }
        if (top  < 0) { top  = 0; }

        if (right >= img_width) { right = img_width-1; }
        if (bottom >= img_height) { bottom = img_height-1; }

        if (left >= right ||
            top >= bottom) {
          return Error(heif_error_Invalid_input,
                       heif_suberror_Invalid_clean_aperture);
        }

        std::shared_ptr<HeifPixelImage> cropped_img;
        error = img->crop(left,right,top,bottom, cropped_img);
        if (error) {
          return error;
        }

        img = cropped_img;
      }
    }
  }

  return Error::Ok;
}


// TODO: this function only works with YCbCr images, chroma 4:2:0, and 8 bpp at the moment
// It will crash badly if we get anything else.
Error HeifContext::decode_full_grid_image(heif_image_id ID,
                                          std::shared_ptr<HeifPixelImage>& img,
                                          const std::vector<uint8_t>& grid_data) const
{
  ImageGrid grid;
  grid.parse(grid_data);
  // std::cout << grid.dump();


  auto iref_box = m_heif_file->get_iref_box();

  if (!iref_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iref_box,
                 "No iref box available, but needed for grid image");
  }

  std::vector<heif_image_id> image_references = iref_box->get_references(ID);

  if ((int)image_references.size() != grid.get_rows() * grid.get_columns()) {
    std::stringstream sstr;
    sstr << "Tiled image with " << grid.get_rows() << "x" <<  grid.get_columns() << "="
         << (grid.get_rows() * grid.get_columns()) << " tiles, but only "
         << image_references.size() << " tile images in file";

    return Error(heif_error_Invalid_input,
                 heif_suberror_Missing_grid_images,
                 sstr.str());
  }

  // --- generate image of full output size

  int w = grid.get_width();
  int h = grid.get_height();
  int bpp = 8; // TODO: how do we know ?

  img = std::make_shared<HeifPixelImage>();
  img->create(w,h,
              heif_colorspace_YCbCr, // TODO: how do we know ?
              heif_chroma_420); // TODO: how do we know ?

  img->add_plane(heif_channel_Y,  w,h, bpp);
  img->add_plane(heif_channel_Cb, w/2,h/2, bpp);
  img->add_plane(heif_channel_Cr, w/2,h/2, bpp);

  int y0=0;
  int reference_idx = 0;

  for (int y=0;y<grid.get_rows();y++) {
    int x0=0;
    int tile_height=0;

    for (int x=0;x<grid.get_columns();x++) {

      std::shared_ptr<HeifPixelImage> tile_img;

      Error err = decode_image(image_references[reference_idx], tile_img);
      if (err != Error::Ok) {
        return err;
      }


      // --- copy tile into output image

      int src_width  = tile_img->get_width();
      int src_height = tile_img->get_height();
      assert(src_width >= 0);
      assert(src_height >= 0);

      tile_height = src_height;

      for (heif_channel channel : { heif_channel_Y, heif_channel_Cb, heif_channel_Cr }) {
        int tile_stride;
        uint8_t* tile_data = tile_img->get_plane(channel, &tile_stride);

        int out_stride;
        uint8_t* out_data = img->get_plane(channel, &out_stride);

        int copy_width  = std::min(src_width, w - x0);
        int copy_height = std::min(src_height, h - y0);

        int xs=x0, ys=y0;

        if (channel != heif_channel_Y) {
          copy_width /= 2;
          copy_height /= 2;
          xs /= 2;
          ys /= 2;
        }

        for (int py=0;py<copy_height;py++) {
          memcpy(out_data + xs + (ys+py)*out_stride,
                 tile_data + py*tile_stride,
                 copy_width);
        }
      }

      x0 += src_width;

      reference_idx++;
    }

    y0 += tile_height;
  }

  return Error::Ok;
}


Error HeifContext::decode_derived_image(heif_image_id ID,
                                        std::shared_ptr<HeifPixelImage>& img) const
{
  // find the ID of the image this image is derived from

  auto iref_box = m_heif_file->get_iref_box();

  if (!iref_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iref_box,
                 "No iref box available, but needed for iden image");
  }

  std::vector<heif_image_id> image_references = iref_box->get_references(ID);

  if ((int)image_references.size() != 1) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Missing_grid_images,
                 "'iden' image with more than one reference image");
  }


  heif_image_id reference_image_id = image_references[0];


  Error error = decode_image(reference_image_id, img);
  return error;
}


Error HeifContext::decode_overlay_image(heif_image_id ID,
                                        std::shared_ptr<HeifPixelImage>& img,
                                        const std::vector<uint8_t>& overlay_data) const
{
  // find the IDs this image is composed of

  auto iref_box = m_heif_file->get_iref_box();

  if (!iref_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iref_box,
                 "No iref box available, but needed for iovl image");
  }

  std::vector<heif_image_id> image_references = iref_box->get_references(ID);

  /* TODO: probably, it is valid that an iovl image has no references ?

  if (image_references.empty()) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Missing_grid_images,
                 "'iovl' image with more than one reference image");
  }
  */


  ImageOverlay overlay;
  overlay.parse(image_references.size(), overlay_data);
  //std::cout << overlay.dump();

  if (image_references.size() != overlay.get_num_offsets()) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Invalid_overlay_data,
                 "Number of image offsets does not match the number of image references");
  }

  int w = overlay.get_canvas_width();
  int h = overlay.get_canvas_height();

  // TODO: seems we always have to compose this in RGB since the background color is an RGB value
  img = std::make_shared<HeifPixelImage>();
  img->create(w,h,
              heif_colorspace_RGB,
              heif_chroma_444);
  img->add_plane(heif_channel_R,w,h,8); // TODO: other bit depths
  img->add_plane(heif_channel_G,w,h,8); // TODO: other bit depths
  img->add_plane(heif_channel_B,w,h,8); // TODO: other bit depths

  uint16_t bkg_color[4];
  overlay.get_background_color(bkg_color);

  Error err = img->fill_RGB_16bit(bkg_color[0], bkg_color[1], bkg_color[2], bkg_color[3]);
  if (err) {
    return err;
  }


  for (size_t i=0;i<image_references.size();i++) {
    std::shared_ptr<HeifPixelImage> overlay_img;
    err = decode_image(image_references[i], overlay_img);
    if (err != Error::Ok) {
      return err;
    }

    overlay_img = overlay_img->convert_colorspace(heif_colorspace_RGB, heif_chroma_444);
    if (!overlay_img) {
      return Error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion);
    }

    int32_t dx,dy;
    overlay.get_offset(i, &dx,&dy);

    err = img->overlay(overlay_img, dx,dy);
    if (err) {
      if (err.error_code == heif_error_Invalid_input &&
          err.sub_error_code == heif_suberror_Overlay_image_outside_of_canvas) {
        // NOP, ignore this error

        err = Error::Ok;
      }
      else {
        return err;
      }
    }
  }

  return err;
}
