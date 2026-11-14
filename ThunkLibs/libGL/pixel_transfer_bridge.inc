#include "pixel_unpack_core.h"
#include "bitmap_unpack_core.h"

static vexa_pixel::PixelTransferState& VexaPixelState() { return vexa_ff::S().pixels; }
static bool VexaPixelInsideBegin() { return vexa_ff::in_begin(); }
static bool VexaPixelRecord(std::function<void()> callback) {
  if (!vexa_ff::S().compiling) return false;
  vexa_ff::run_or_record_callback(std::move(callback));
  return true;
}
static bool VexaPixelMapRead(const void* source, void* destination, size_t bytes) {
  GLint buffer = 0;
  fexfn_pack_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
  if (!buffer) {
    if (!source && bytes) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return false; }
    std::memcpy(destination, source, bytes);
    return true;
  }
  GLint length = 0, mapped = 0;
  fexfn_pack_glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &length);
  fexfn_pack_glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_MAPPED, &mapped);
  const auto offset = reinterpret_cast<uintptr_t>(source);
  if (mapped || length < 0 || offset > static_cast<size_t>(length) || bytes > static_cast<size_t>(length) - offset) {
    fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return false;
  }
  void* address = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
  if (!address) return false;
  std::memcpy(destination, address, bytes);
  return glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
}
#include "pixel_transfer_guest.inc"

static vexa_pixel::Unpack VexaUnpack() {
  vexa_pixel::Unpack unpack;
  GLint value = 0;
  fexfn_pack_glGetIntegerv(GL_UNPACK_ALIGNMENT, &value); unpack.alignment = value;
  fexfn_pack_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value); unpack.row_length = value;
  fexfn_pack_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &value); unpack.skip_rows = value;
  fexfn_pack_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &value); unpack.skip_pixels = value;
  unpack.swap_bytes = vexa_ff::S().unpack_swap_bytes;
  return unpack;
}
static bool VexaPixelBytes(const vexa_pixel::Unpack& unpack, size_t width, size_t height, size_t pixelSize, bool bitmap, size_t& bytes) {
  size_t row = unpack.row_length ? unpack.row_length : width, skip = 0, last = 0, tail = 0;
  if (bitmap) { if (!vexa_pixel::checked_add(row, 7, row)) return false; row /= 8; }
  else if (!vexa_pixel::checked_mul(row, pixelSize, row)) return false;
  if (!vexa_pixel::checked_add(row, unpack.alignment - 1, row)) return false;
  row &= ~(size_t(unpack.alignment) - 1);
  if (!vexa_pixel::checked_mul(row, unpack.skip_rows, skip) ||
      !vexa_pixel::checked_mul(row, height - 1, last) ||
      !vexa_pixel::checked_add(unpack.skip_pixels, width, tail)) return false;
  if (bitmap) { if (!vexa_pixel::checked_add(tail, 7, tail)) return false; tail /= 8; }
  else if (!vexa_pixel::checked_mul(tail, pixelSize, tail)) return false;
  return vexa_pixel::checked_add(skip, last, bytes) && vexa_pixel::checked_add(bytes, tail, bytes) && bytes <= INT_MAX;
}
static void VexaSubmitPixels(const std::vector<float>& rgba, int width, int height, bool bitmap, float xorig = 0, float yorig = 0) {
  const auto& position = VexaRasterPosition();
  if (!position.valid || !width || !height) return;
  auto& selection=vexa_ff::S().selection;
  if(selection.mode==vexa_select::kSelect)return;
  if(selection.mode==vexa_select::kFeedback){
    vexa_select::FeedbackVertex vertex;
    for(int i=0;i<4;++i){vertex.position[i]=position.window[i];vertex.color[i]=position.color[i];vertex.texture[i]=position.texcoord[i];}
    selection.feedbackValue(bitmap?0x0704:0x0705);selection.feedbackVertex(vertex,false);return;
  }
  const auto& transfer = VexaPixelState();
  const float raster[] = {float(position.window[0]) - xorig, float(position.window[1]) - yorig,
    float(position.window[2]), bitmap ? 1.f : transfer.zoom_x, bitmap ? 1.f : transfer.zoom_y};
  float color[4]; std::copy(position.color.begin(), position.color.end(), color);
  if (!fexfn_pack_VEXA_DrawPixels(rgba.data(), width, height, raster, bitmap ? color : nullptr))
    fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);
}
VEXA_GL_GUEST_EXPORT void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels) {
  if (!VexaPixelCheckBegin()) return;
  if (width < 0 || height < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return; }
  vexa_pixel::Format fmt; unsigned channels;
  switch (format) {
    case GL_ALPHA: fmt=vexa_pixel::Format::Alpha; channels=1; break;
    case 0x1909: fmt=vexa_pixel::Format::Luminance; channels=1; break;
    case 0x190A: fmt=vexa_pixel::Format::LuminanceAlpha; channels=2; break;
    case GL_RGB: fmt=vexa_pixel::Format::RGB; channels=3; break;
    case GL_RGBA: fmt=vexa_pixel::Format::RGBA; channels=4; break;
    case 0x80E1: fmt=vexa_pixel::Format::BGRA; channels=4; break;
    default: fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return;
  }
  vexa_pixel::Type typ; unsigned component;
  switch (type) {
    case GL_UNSIGNED_BYTE: typ=vexa_pixel::Type::U8; component=1; break;
    case GL_UNSIGNED_SHORT: typ=vexa_pixel::Type::U16; component=2; break;
    case GL_FLOAT: typ=vexa_pixel::Type::F32; component=4; break;
    default: fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return;
  }
  if (!width || !height) return;
  const auto unpack = VexaUnpack(); size_t bytes = 0;
  if (!VexaPixelBytes(unpack, width, height, channels * component, false, bytes)) { fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY); return; }
  std::vector<unsigned char> source(bytes);
  if (!VexaPixelMapRead(pixels, source.data(), bytes)) return;
  // Capture source bytes/unpack now, but apply transfer and raster state at replay.
  auto draw = [source=std::move(source), unpack, width, height, fmt, typ] {
    const auto& state=VexaPixelState();
    vexa_pixel::Transfer transfer; transfer.scale=state.scale; transfer.bias=state.bias; transfer.map_color=state.map_color;
    for (unsigned i=0; i<4; ++i) transfer.maps[i]=state.maps[6+i];
    std::vector<float> rgba;
    if (!vexa_pixel::rgba32f(source.data(), source.size(), width, height, fmt, typ, unpack, transfer, rgba)) {
      fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return;
    }
    VexaSubmitPixels(rgba, width, height, false);
  };
  if (!VexaPixelRecord(draw)) draw();
}
VEXA_GL_GUEST_EXPORT void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte* bitmap) {
  if (!VexaPixelCheckBegin()) return;
  if (width < 0 || height < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return; }
  std::vector<float> mask;
  if (width && height) {
    const auto unpack=VexaUnpack(); size_t bytes=0;
    if (!VexaPixelBytes(unpack,width,height,1,true,bytes)) { fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY); return; }
    std::vector<unsigned char> source(bytes);
    if (!VexaPixelMapRead(bitmap,source.data(),bytes)) return;
    if (!vexa_pixel::bitmap_mask(source.data(),source.size(),width,height,unpack,vexa_ff::S().unpack_lsb_first,mask)) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  }
  auto draw=[mask=std::move(mask),width,height,xorig,yorig,xmove,ymove] {
    if (!VexaRasterPosition().valid) return;
    VexaSubmitPixels(mask,width,height,true,xorig,yorig);
    VexaRasterPosition().window[0]+=xmove; VexaRasterPosition().window[1]+=ymove;
  };
  if (!VexaPixelRecord(draw)) draw();
}

VEXA_GL_GUEST_EXPORT void glPixelStorei(GLenum pname, GLint value) {
  if (!VexaPixelCheckBegin()) return;
  // Pixel-store commands execute immediately, even while compiling a list.
  if (pname == 0x0CF0) { vexa_ff::S().unpack_swap_bytes = value != 0; fexfn_pack_glPixelStorei(pname, value); return; }
  if (pname == 0x0CF1) { vexa_ff::S().unpack_lsb_first = value != 0; return; }
  if (pname == 0x0D00) { vexa_ff::S().pack_swap_bytes = value != 0; return; }
  if (pname == 0x0D01) { vexa_ff::S().pack_lsb_first = value != 0; return; }
  fexfn_pack_glPixelStorei(pname, value);
}

VEXA_GL_GUEST_EXPORT void glCopyPixels(GLint x,GLint y,GLsizei width,GLsizei height,GLenum type) {
  if(!VexaPixelCheckBegin())return;
  if(width<0||height<0){fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE);return;}
  if(type!=0x1800 && type!=0x1801 && type!=0x1802){fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM);return;}
  auto copy=[=]{
    auto& s=vexa_ff::S();const auto& position=VexaRasterPosition();
    if(!position.valid||!width||!height||s.selection.mode==vexa_select::kSelect)return;
    if(s.selection.mode==vexa_select::kFeedback){vexa_select::FeedbackVertex vertex;
      for(int i=0;i<4;++i){vertex.position[i]=position.window[i];vertex.color[i]=position.color[i];vertex.texture[i]=position.texcoord[i];}
      s.selection.feedbackValue(0x0706);s.selection.feedbackVertex(vertex,false);return;}
    // GLES cannot portably read/write desktop depth or stencil pixel rectangles.
    if(type!=0x1800){fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);return;}
    if(fexfn_pack_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fexfn_pack_VEXA_CompatError(GL_INVALID_FRAMEBUFFER_OPERATION);return;}
    GLint readBuffer=0;fexfn_pack_glGetIntegerv(GL_READ_BUFFER,&readBuffer);
    if(readBuffer==GL_NONE){fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);return;}
    size_t bytes=0;vexa_pixel::Unpack unpack;unpack.alignment=1;
    if(!VexaPixelBytes(unpack,width,height,4,false,bytes)||bytes>256u*1024*1024){fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY);return;}
    GLint saved[5];const GLenum names[]={GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS,GL_PIXEL_PACK_BUFFER_BINDING};
    for(int i=0;i<5;++i)fexfn_pack_glGetIntegerv(names[i],&saved[i]);
    fexfn_pack_glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
    for(int i=0;i<4;++i)fexfn_pack_glPixelStorei(names[i],i==0?1:0);
    std::vector<unsigned char> source(bytes);
    fexfn_pack_glReadPixels(x,y,width,height,GL_RGBA,GL_UNSIGNED_BYTE,source.data());
    for(int i=0;i<4;++i)fexfn_pack_glPixelStorei(names[i],saved[i]);
    fexfn_pack_glBindBuffer(GL_PIXEL_PACK_BUFFER,saved[4]);
    vexa_pixel::Transfer transfer;transfer.scale=s.pixels.scale;transfer.bias=s.pixels.bias;transfer.map_color=s.pixels.map_color;
    for(int i=0;i<4;++i)transfer.maps[i]=s.pixels.maps[6+i];
    std::vector<float> rgba;
    if(vexa_pixel::rgba32f(source.data(),bytes,width,height,vexa_pixel::Format::RGBA,vexa_pixel::Type::U8,unpack,transfer,rgba))VexaSubmitPixels(rgba,width,height,false);
  };
  if(!VexaPixelRecord(copy))copy();
}
