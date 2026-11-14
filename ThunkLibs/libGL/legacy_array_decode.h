#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
namespace vexa_array {
template<class T> inline double read(const uint8_t* data,bool normalize) {
  T value;std::memcpy(&value,data,sizeof(value));
  if(normalize && std::numeric_limits<T>::is_integer)
    return std::max(-1.,double(value)/double(std::numeric_limits<T>::max()));
  return value;
}
inline bool decode(const uint8_t* data,unsigned type,unsigned count,bool normalized,bool bgra,float* output) {
  if(!data||!output||count<1||count>4)return false;
  std::fill_n(output,4,0.f);output[3]=1;
  for(unsigned c=0;c<count;++c) {
    double value;
    switch(type) {
      case 0x1400:value=read<int8_t>(data+c,normalized);break;
      case 0x1401:value=read<uint8_t>(data+c,normalized);break;
      case 0x1402:value=read<int16_t>(data+c*2,normalized);break;
      case 0x1403:value=read<uint16_t>(data+c*2,normalized);break;
      case 0x1404:value=read<int32_t>(data+c*4,normalized);break;
      case 0x1405:value=read<uint32_t>(data+c*4,normalized);break;
      case 0x1406:value=read<float>(data+c*4,false);break;
      case 0x140A:value=read<double>(data+c*8,false);break;
      case 0x140C:value=read<int32_t>(data+c*4,false)/65536.;break;
      default:return false;
    }
    output[c]=static_cast<float>(value);
  }
  if(bgra)std::swap(output[0],output[2]);
  return true;
}
}
