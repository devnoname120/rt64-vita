#pragma once
#include "../rt64_fast.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace RT64::Tests::Depth {
inline void check(bool value,const char *message) {
    if(!value)throw std::runtime_error(message);
}
inline void rectangle(RT64::FastDrawSink &sink,uint32_t color,uint32_t depth,
               unsigned width,unsigned height,std::array<unsigned,4> bounds,
               float z,bool write=true) {
    RT64::FastDraw draw;
    draw.colorAddress=color;draw.depthAddress=depth;draw.width=width;draw.height=height;
    draw.scissor={0,0,int32_t(width*4),int32_t(height*4)};
    draw.fill=true;draw.fillColor={1,0,0,1};draw.depthTest=true;draw.depthWrite=write;
    const float left=2.0f*bounds[0]/width-1,right=2.0f*bounds[2]/width-1;
    const float top=1-2.0f*bounds[1]/height,bottom=1-2.0f*bounds[3]/height;
    const float xy[][2]={{left,top},{right,top},{right,bottom},{left,top},{right,bottom},{left,bottom}};
    draw.vertices.resize(6);
    for(unsigned i=0;i<6;++i) {
        draw.vertices[i].position[0]=xy[i][0];draw.vertices[i].position[1]=xy[i][1];
        draw.vertices[i].position[2]=2*z-1;
    }
    sink.draw(draw);
}
inline void clear(RT64::FastDrawSink &sink,uint32_t address,unsigned width,unsigned height,
           std::array<unsigned,4> bounds) {
    RT64::FastDraw draw;draw.clearDepth=true;draw.colorAddress=address;
    draw.width=width;draw.height=height;
    draw.scissor={int32_t(bounds[0]*4),int32_t(bounds[1]*4),int32_t(bounds[2]*4),int32_t(bounds[3]*4)};
    sink.draw(draw);
}
inline void expect(RT64::FastDrawSink &sink,uint32_t address,const std::vector<uint16_t> &pixels) {
    std::vector<uint8_t> bytes;
    check(sink.readDepthFramebuffer(address,pixels.size()*2,bytes),"rendered depth image was not resident");
    check(bytes.size()==pixels.size()*2,"depth readback returned the wrong byte count");
    for(size_t i=0;i<pixels.size();++i) {
        const uint16_t actual=(uint16_t(bytes[i*2])<<8)|bytes[i*2+1];
        if(actual!=pixels[i]) {
            std::fprintf(stderr,"Depth pixel %u: got %04x, expected %04x\n",unsigned(i),unsigned(actual),unsigned(pixels[i]));
            throw std::runtime_error("depth precision, row layout, packing or visibility changed");
        }
    }
    std::vector<uint8_t> partial;
    check(sink.readDepthFramebuffer(address+3,bytes.size()-7,partial),"partial depth readback was unavailable");
    check(std::equal(partial.begin(),partial.end(),bytes.begin()+3,bytes.end()-4),"partial depth bytes changed alignment");
    partial={0x12,0x34};
    check(!sink.readDepthFramebuffer(address-1,2,partial)&&partial==std::vector<uint8_t>({0x12,0x34}),"out-of-range depth read changed its destination");
    check(!sink.readDepthFramebuffer(address+bytes.size()-1,2,partial),"depth read crossed the resident image boundary");
    check(!sink.readDepthFramebuffer(0xfffffff0,64,partial),"depth read accepted address overflow");
}
inline unsigned run(FastDrawSink &sink,unsigned width,unsigned height,uint32_t address) {
    unsigned cases=0;
    const uint32_t color=address+0x80000;
    std::vector<uint8_t> absent{0x5a};
    check(!sink.readDepthFramebuffer(address,2,absent)&&absent==std::vector<uint8_t>({0x5a}),"unrendered depth destroyed caller data");
    std::vector<uint16_t> expected(width*height,0x2000);
    rectangle(sink,color,address,width,height,{0,0,width,height},0.5f);
    expect(sink,address,expected);++cases;
    clear(sink,address,width,height,{0,0,width,height});
    std::fill(expected.begin(),expected.end(),0xfffc);
    expect(sink,address,expected);++cases;
    rectangle(sink,color,address,width,height,{0,0,width,height},0.5f);
    std::fill(expected.begin(),expected.end(),0x2000);
    expect(sink,address,expected);++cases;
    rectangle(sink,color,address,width,height,{3,5,width/2,height-2},0.25f);
    rectangle(sink,color,address,width,height,{0,0,width,height},0.875f);
    for(unsigned y=5;y<height-2;++y)for(unsigned x=3;x<width/2;++x)expected[y*width+x]=0x1000;
    expect(sink,address,expected);++cases;
    rectangle(sink,color,address,width,height,{0,0,width,height},0.125f,false);
    expect(sink,address,expected);++cases;
    clear(sink,address,width,height,{1,1,7,4});
    for(unsigned y=1;y<4;++y)for(unsigned x=1;x<7;++x)expected[y*width+x]=0xfffc;
    expect(sink,address,expected);++cases;
    rectangle(sink,color+0x40000,address,width,height,{width/2,height/2,width,height},0.125f);
    for(unsigned y=height/2;y<height;++y)for(unsigned x=width/2;x<width;++x)expected[y*width+x]=0x0800;
    expect(sink,address,expected);++cases;
    clear(sink,address+0x40000,width,height,{0,0,width,height});
    rectangle(sink,color,address+0x40000,width,height,{0,0,width,height},0.875f);
    expect(sink,address+0x40000,std::vector<uint16_t>(width*height,0x5ffc));++cases;
    expect(sink,address,expected);++cases;
    constexpr std::array<uint32_t,20> fixed={0,1,63,64,0x1ffff,0x20000,0x2ffff,0x30000,
        0x37fff,0x38000,0x3bfff,0x3c000,0x3dfff,0x3e000,0x3efff,0x3f000,0x3f7ff,0x3f800,0x3fffe,0x3ffff};
    constexpr std::array<uint16_t,20> packed={0,0,0,4,0x1ffc,0x2000,0x3ffc,0x4000,
        0x5ffc,0x6000,0x7ffc,0x8000,0x9ffc,0xa000,0xbffc,0xc000,0xdffc,0xe000,0xfff8,0xfffc};
    clear(sink,address,width,height,{0,0,width,height});
    for(unsigned i=0;i<fixed.size();++i) {
        const unsigned left=i*width/fixed.size(),right=(i+1)*width/fixed.size();
        rectangle(sink,color,address,width,height,{left,0,right,height},float(fixed[i])/262143.0f);
        for(unsigned y=0;y<height;++y)for(unsigned x=left;x<right;++x)expected[y*width+x]=packed[i];
    }
    std::vector<uint8_t> first_partial;
    check(sink.readDepthFramebuffer(address+3,width*2+3,first_partial),"first odd depth query was not resident");
    check(first_partial.size()==width*2+3,"first odd depth query returned the wrong byte count");
    for(unsigned i=0;i<first_partial.size();++i) {
        const unsigned offset=i+3;
        check(first_partial[i]==uint8_t(expected[offset/2]>>((offset&1)?0:8)),
            "first odd depth query changed packing or crossed a row incorrectly");
    }
    expect(sink,address,expected);++cases;
    std::vector<uint8_t> empty{0xff};
    check(sink.readDepthFramebuffer(address,0,empty)&&empty.empty(),"empty depth readback failed");
    return cases;
}
inline void stress(FastDrawSink &sink,unsigned iterations,uint32_t address,bool present=false) {
    constexpr unsigned width=320,height=240;
    for(unsigned i=0;i<iterations;++i) {
        clear(sink,address,width,height,{0,0,width,height});
        const bool near=i&1;
        rectangle(sink,address+0x40000,address,width,height,{0,0,width,height},near?0.25f:0.5f);
        std::vector<uint8_t> pixel;
        check(sink.readDepthFramebuffer(address+(60*width+160)*2,2,pixel),"depth stress query was not resident");
        check(pixel.size()==2&&pixel[0]==(near?0x10:0x20)&&pixel[1]==0,"depth stress query retained stale data");
        if(present)sink.present(address+0x40000);
    }
}
}
